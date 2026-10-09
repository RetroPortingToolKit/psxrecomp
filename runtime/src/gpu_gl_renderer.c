/* gpu_gl_renderer.c — hardware OpenGL renderer backend.
 *
 * ARCHITECTURE (v2 — GPU-authoritative VRAM)
 * -------------------------------------------
 * The FBO color texture (`s_hr_tex`, RGBA8, 1024*S x 512*S where S is the
 * internal-resolution scale from [video] supersampling) is the single
 * authoritative copy of VRAM during normal 15-bit rendering. Every mutation
 * goes through the GPU; depth24 scanout additionally keeps the CPU mirror
 * authoritative because packed RGB888 framebuffer uploads are not staged as
 * 1555 FBO pixels. GP0 clears/draws/copies must also update that mirror:
 *
 *   - polys / rects / lines  -> rasterized into the hr FBO
 *   - GP0(02h) fills         -> scissored glClear (color + stencil)
 *   - VRAM->VRAM copies      -> hr FBO -> scratch texture blit, then a
 *                               masked quad draw back into the hr FBO
 *   - CPU->VRAM transfers    -> written to the CPU mirror immediately and
 *                               accumulated into a pending-upload rect; the
 *                               rect is flushed (staging texture + quad into
 *                               the hr FBO, plus a direct R16UI subimage)
 *                               before the next GPU op, preserving op order
 *
 * PS1 MASK BIT (bit15). The FBO alpha channel carries bit15 exactly
 * (1.0 = set), and a stencil buffer mirrors it (stencil bit0 == bit15):
 *   - "set mask"  -> fragment alpha + stencil write value
 *   - "check mask"-> stencil test (pass iff stored == 0). The stencil write
 *     value is coupled to the test reference in GL, so when checking we use
 *     GL_INVERT on pass to write a 1 (stored is known 0 when the test
 *     passes) and GL_KEEP to write a 0.
 *   - textured prims (and copies/uploads, whose pixels carry per-texel STP
 *     bits) are drawn in TWO passes split by the STP bit via discard, so
 *     each pass writes a single known stencil value. The same split already
 *     existed for semi-transparent texture blending.
 *   - blending uses glBlendFuncSeparate so the alpha (mask) channel is
 *     always REPLACED by the source fragment's mask bit, never blended.
 *
 * TEXTURE SAMPLING / RENDER-TO-TEXTURE. Textured prims sample a native-res
 * R16UI mirror (`s_raw_tex`) holding raw 1555 VRAM values (CLUT decode +
 * texture window + optional bilinear in the fragment shader). GPU draws
 * mark a native-coords dirty union; before any textured draw whose texture
 * page or CLUT intersects the union, a PACK pass re-encodes the dirty
 * region of the hr FBO into the raw mirror (point-sampled at native
 * coords). CPU->VRAM uploads update the raw mirror directly. So content
 * rendered by the GPU is immediately valid as a texture source.
 *
 * CPU READBACKS (VRAM->CPU transfers, GPUREAD, screenshots) in 15-bit mode
 * flush uploads + pack, then glReadPixels the raw mirror straight into CPU
 * VRAM (raw 1555, no conversion loop). Depth24 entry synchronizes once before
 * the first write; readback is then suppressed until 15-bit rendering resumes.
 *
 * PRESENT is deterministic: 15-bit frames always blit the display region
 * from the hr FBO into a 4:3 letterboxed rect (single path — no more
 * frame-to-frame alternation between FBO and CPU presents). 24-bit (FMV)
 * frames use the authoritative CPU mirror and quad-present, also letterboxed.
 * PSX_GL_FORCE_CPU_PRESENT=1 (read by main.cpp) forces the CPU path as a
 * diagnostic.
 *
 * Known divergences from the software rasterizer (accepted, documented):
 *   - GL triangle/line coverage rules differ from the PS1 DDA by ±1px on
 *     edges; lines use GL_LINES at 1x and, above 1x, a quad one native pixel
 *     thick that covers both endpoints (line_to_quad) instead of Bresenham.
 *   - No dithering (the software path doesn't dither either).
 *   - Gouraud interpolation happens at 8-bit precision instead of 5-bit
 *     (smoother gradients; readback re-quantizes to 5-bit).
 *   - VRAM-wrapping draws are clamped, except GP0(02h) fills which split
 *     into wrapped segments. Wrapping copies/draws are unused by real SDKs.
 *
 * INTERNAL SCALE LIMITS. S is clamped at init to the driver's texture,
 * renderbuffer and viewport limits and to a memory budget (gl_scale_limits.h),
 * and an FBO that still fails to allocate is retried one scale lower. A large
 * S therefore never costs the GL backend itself.
 *
 * Init is all-or-nothing: if any shader/FBO fails, gl_renderer_init_context
 * returns 0 and the runtime falls back to the pure software renderer — no
 * half-GL hybrid. */

#include "gpu.h"
#include "gpu_render.h"
#include "gl_scale_limits.h"
#include "gpu_sw_renderer.h"
#include "gpu_gl_renderer.h"
#include "gpu_hd_textures.h"
#include "mod_texture_banks.h"
#include "frame_interpolation.h"
#include "render_pass_plan.h"
#include "render_pass.h"
#include "psx_cycles.h"
#include "psx_video_timing.h"
#include "mod_plugins.h"      /* PSX_MOD_RENDER_PASS_* reasons */
#include "host_osd.h"
#include "psx_savestate_menu.h"
#include "host_time.h"
#include "latency_ring.h"
#include "gpu_timeline.h"
#include "frame_pacing.h"
#include "psx_rewind.h"
#include "psx_openxr.h"
#include "openxr_color.h"
#include "present_thread.h"

#include "psx_sdl.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "png_write.h"   /* png_write_rgb — present_shot readback */
static uint64_t s_fg_flips;   /* frame generation (below): game frames seen */

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
/* GL 1.4+ enums absent from MinGW's GL 1.1 headers. */
#define PSXGL_FRAGMENT_SHADER       0x8B30
#define PSXGL_VERTEX_SHADER         0x8B31
#define PSXGL_COMPILE_STATUS        0x8B81
#define PSXGL_LINK_STATUS           0x8B82
#define PSXGL_TEXTURE0              0x84C0
#define PSXGL_ARRAY_BUFFER          0x8892
#define PSXGL_STREAM_DRAW           0x88E0
#define PSXGL_FRAMEBUFFER           0x8D40
#define PSXGL_READ_FRAMEBUFFER      0x8CA8
#define PSXGL_DRAW_FRAMEBUFFER      0x8CA9
#define PSXGL_COLOR_ATTACHMENT0     0x8CE0
#define PSXGL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define PSXGL_FRAMEBUFFER_COMPLETE  0x8CD5
#define PSXGL_RENDERBUFFER          0x8D41
#define PSXGL_DEPTH24_STENCIL8      0x88F0
#define PSXGL_R16UI                 0x8234
#define PSXGL_RED_INTEGER           0x8D94
#define PSXGL_FUNC_ADD              0x8006
#define PSXGL_FUNC_REVERSE_SUBTRACT 0x800B
#define PSXGL_CONSTANT_ALPHA        0x8003
#define PSXGL_UNPACK_ROW_LENGTH     0x0CF2
#define PSXGL_PACK_ROW_LENGTH       0x0D02
#define PSXGL_SRC1_ALPHA            0x8589

#ifndef APIENTRY
#define APIENTRY
#endif

#define VRAM_W 1024
#define VRAM_H 512
/* GL_MAX_INTERNAL_SCALE (the compile-time ceiling) lives in gpu_render.h; the
 * effective scale is clamped to the driver and memory limits at init. */
#ifndef PSXGL_MAX_RENDERBUFFER_SIZE
#define PSXGL_MAX_RENDERBUFFER_SIZE 0x84E8
#endif
#ifndef GL_MAX_VIEWPORT_DIMS
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#endif
/* Scratch tile edge (hr px) for the tiled stencil rebuild at S > 1. */
#define GL_SCRATCH_TILE 2048

/* ---- Loaded modern-GL entry points ------------------------------------- */
typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void   (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
typedef void   (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void   (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef void   (APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void   (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void   (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void   (APIENTRY *PFN_glUseProgram)(GLuint);
typedef GLint  (APIENTRY *PFN_glGetUniformLocation)(GLuint, const char *);
typedef void   (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *PFN_glUniform2i)(GLint, GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform4i)(GLint, GLint, GLint, GLint, GLint);
typedef void   (APIENTRY *PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glUniform4fv)(GLint, GLsizei, const GLfloat *);
typedef void   (APIENTRY *PFN_glBlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (APIENTRY *PFN_glBlendEquationSeparate)(GLenum, GLenum);
typedef void   (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void   (APIENTRY *PFN_glActiveTexture)(GLenum);
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void   (APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void   (APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_glBindFragDataLocationIndexed)(GLuint, GLuint, GLuint, const char *);
typedef void   (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void   (APIENTRY *PFN_glBlitFramebuffer)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
typedef void   (APIENTRY *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
/* GPU timer queries (ARB_timer_query / core GL 3.3) — frame_perf instrumentation. */
typedef void   (APIENTRY *PFN_glGenQueries)(GLsizei, GLuint *);
typedef void   (APIENTRY *PFN_glDeleteQueries)(GLsizei, const GLuint *);
typedef void   (APIENTRY *PFN_glBeginQuery)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glEndQuery)(GLenum);
typedef void   (APIENTRY *PFN_glGetQueryObjectui64v)(GLuint, GLenum, GLuint64 *);
typedef void   (APIENTRY *PFN_glGetQueryObjectiv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFN_glQueryCounter)(GLuint, GLenum);
/* Buffer mapping for asynchronous pixel-pack readback (presented-image ring). */
typedef void * (APIENTRY *PFN_glMapBuffer)(GLenum, GLenum);
typedef GLboolean (APIENTRY *PFN_glUnmapBuffer)(GLenum);
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED            0x88BF
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP               0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT            0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE  0x8867
#endif

static PFN_glMapBuffer         p_glMapBuffer;
static PFN_glUnmapBuffer       p_glUnmapBuffer;
static PFN_glCreateShader      p_glCreateShader;
static PFN_glShaderSource      p_glShaderSource;
static PFN_glCompileShader     p_glCompileShader;
static PFN_glGetShaderiv       p_glGetShaderiv;
static PFN_glGetShaderInfoLog  p_glGetShaderInfoLog;
static PFN_glDeleteShader      p_glDeleteShader;
static PFN_glDeleteProgram     p_glDeleteProgram;
static PFN_glCreateProgram     p_glCreateProgram;
static PFN_glAttachShader      p_glAttachShader;
static PFN_glLinkProgram       p_glLinkProgram;
static PFN_glGetProgramiv      p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram        p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform1i         p_glUniform1i;
static PFN_glUniform1f         p_glUniform1f;
static PFN_glUniform2i         p_glUniform2i;
static PFN_glUniform4i         p_glUniform4i;
static PFN_glUniform2f         p_glUniform2f;
static PFN_glUniform4f         p_glUniform4f;
static PFN_glUniform4fv        p_glUniform4fv;
static PFN_glBlendColor        p_glBlendColor;
static PFN_glBlendFuncSeparate p_glBlendFuncSeparate;
static PFN_glBlendEquationSeparate p_glBlendEquationSeparate;
static PFN_glGenVertexArrays   p_glGenVertexArrays;
static PFN_glBindVertexArray   p_glBindVertexArray;
static PFN_glActiveTexture     p_glActiveTexture;
static PFN_glGenBuffers        p_glGenBuffers;
static PFN_glDeleteBuffers     p_glDeleteBuffers;
static PFN_glDeleteVertexArrays p_glDeleteVertexArrays;
static PFN_glBindBuffer        p_glBindBuffer;
static PFN_glBufferData        p_glBufferData;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
static PFN_glBindFragDataLocationIndexed p_glBindFragDataLocationIndexed;
static PFN_glGenFramebuffers   p_glGenFramebuffers;
static PFN_glDeleteFramebuffers p_glDeleteFramebuffers;
static PFN_glBindFramebuffer   p_glBindFramebuffer;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;
static PFN_glBlitFramebuffer   p_glBlitFramebuffer;
static PFN_glGenQueries          p_glGenQueries;
static PFN_glDeleteQueries       p_glDeleteQueries;
static PFN_glBeginQuery          p_glBeginQuery;
static PFN_glEndQuery            p_glEndQuery;
static PFN_glGetQueryObjectui64v p_glGetQueryObjectui64v;
static PFN_glGetQueryObjectiv    p_glGetQueryObjectiv;
static PFN_glQueryCounter        p_glQueryCounter;
static void gl_perf_init(void);   /* frame_perf — defined below, called from init_gpu_raster */
static void gl_perf_mirror_begin(void); /* frame_perf: GPU-time bracket around ONE native-wide */
static void gl_perf_mirror_end(void);   /* mirror pass (timestamp pair; splits scene canonical/mirror) */
/* Native-wide mirror ABLATION (perf attribution, debug cmd gl_ws_ablate):
 * 0 = normal, 1 = skip the whole mirror pass, 2 = full mirror state churn but no
 * draw calls, 3 = mirror draws land in the hr FBO (no per-pass FBO rebind; wide
 * margins go stale + hr gets garbage — perf probe only). */
static int s_ws_ablate = 0;
static void flush_tex_batch(void); /* textured-prim batch — defined below, flushed from coherency points */
static void flush_flat_batch(void); /* flat/gouraud GEO batch (MotK starfield 0x68 dots) */
static PFN_glGenRenderbuffers  p_glGenRenderbuffers;
static PFN_glDeleteRenderbuffers p_glDeleteRenderbuffers;
static PFN_glBindRenderbuffer  p_glBindRenderbuffer;
static PFN_glRenderbufferStorage p_glRenderbufferStorage;
static PFN_glFramebufferRenderbuffer p_glFramebufferRenderbuffer;

/* Present thread (docs/RENDER_THREAD.md "Present thread"): while it runs,
 * the default framebuffer of the composing context is an offscreen slot.
 * Every bind of framebuffer 0 on that context lands on the current slot; the
 * present thread's own context binds the real one (p_glBindFramebuffer_raw). */
typedef void *(APIENTRY *PFN_glFenceSync)(GLenum, GLbitfield);
typedef void   (APIENTRY *PFN_glWaitSync)(void *, GLbitfield, uint64_t);
typedef void   (APIENTRY *PFN_glDeleteSync)(void *);
static PFN_glFenceSync  p_glFenceSync;
static PFN_glWaitSync   p_glWaitSync;
static PFN_glDeleteSync p_glDeleteSync;
static PFN_glBindFramebuffer p_glBindFramebuffer_raw;
static int    s_pt_on = 0;               /* redirect active (present thread running) */
static volatile int s_pt_interval_gen = 0; /* bumped per swap-interval change */
static GLuint pt_target_fbo(void);
static void APIENTRY gl_bind_fb_redirect(GLenum target, GLuint fb) {
    if (fb == 0 && s_pt_on) fb = pt_target_fbo();
    p_glBindFramebuffer_raw(target, fb);
}

static int load_modern_gl(void) {
    int ok = 1;
#define LOAD(p, n) do { p = (void *)SDL_GL_GetProcAddress(n); if (!p) ok = 0; } while (0)
    LOAD(p_glCreateShader, "glCreateShader");   LOAD(p_glShaderSource, "glShaderSource");
    LOAD(p_glCompileShader, "glCompileShader"); LOAD(p_glGetShaderiv, "glGetShaderiv");
    LOAD(p_glGetShaderInfoLog, "glGetShaderInfoLog"); LOAD(p_glDeleteShader, "glDeleteShader");
    LOAD(p_glCreateProgram, "glCreateProgram"); LOAD(p_glAttachShader, "glAttachShader");
    LOAD(p_glLinkProgram, "glLinkProgram");     LOAD(p_glGetProgramiv, "glGetProgramiv");
    LOAD(p_glGetProgramInfoLog, "glGetProgramInfoLog"); LOAD(p_glUseProgram, "glUseProgram");
    LOAD(p_glGetUniformLocation, "glGetUniformLocation"); LOAD(p_glUniform1i, "glUniform1i");
    LOAD(p_glUniform1f, "glUniform1f");
    LOAD(p_glUniform2i, "glUniform2i"); LOAD(p_glUniform4i, "glUniform4i");
    LOAD(p_glUniform2f, "glUniform2f");
    LOAD(p_glUniform4f, "glUniform4f");
    LOAD(p_glUniform4fv, "glUniform4fv");
    LOAD(p_glBlendColor, "glBlendColor");
    LOAD(p_glBlendFuncSeparate, "glBlendFuncSeparate");
    LOAD(p_glBlendEquationSeparate, "glBlendEquationSeparate");
    LOAD(p_glGenVertexArrays, "glGenVertexArrays"); LOAD(p_glBindVertexArray, "glBindVertexArray");
    LOAD(p_glActiveTexture, "glActiveTexture");  LOAD(p_glGenBuffers, "glGenBuffers");
    LOAD(p_glDeleteBuffers, "glDeleteBuffers"); LOAD(p_glDeleteVertexArrays, "glDeleteVertexArrays");
    LOAD(p_glBindBuffer, "glBindBuffer");        LOAD(p_glBufferData, "glBufferData");
    LOAD(p_glVertexAttribPointer, "glVertexAttribPointer");
    LOAD(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(p_glBindFragDataLocationIndexed, "glBindFragDataLocationIndexed");
    LOAD(p_glGenFramebuffers, "glGenFramebuffers"); LOAD(p_glBindFramebuffer, "glBindFramebuffer");
    LOAD(p_glDeleteFramebuffers, "glDeleteFramebuffers");
    LOAD(p_glFramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(p_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD(p_glBlitFramebuffer, "glBlitFramebuffer");
    LOAD(p_glGenRenderbuffers, "glGenRenderbuffers");
    LOAD(p_glDeleteRenderbuffers, "glDeleteRenderbuffers");
    LOAD(p_glBindRenderbuffer, "glBindRenderbuffer");
    LOAD(p_glRenderbufferStorage, "glRenderbufferStorage");
    LOAD(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    /* GPU timer queries — optional (frame_perf). Don't fail the renderer if
     * absent; gl_perf just stays disabled. */
    p_glGenQueries          = (void *)SDL_GL_GetProcAddress("glGenQueries");
    p_glDeleteQueries       = (void *)SDL_GL_GetProcAddress("glDeleteQueries");
    p_glBeginQuery          = (void *)SDL_GL_GetProcAddress("glBeginQuery");
    p_glEndQuery            = (void *)SDL_GL_GetProcAddress("glEndQuery");
    p_glGetQueryObjectui64v = (void *)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
    p_glGetQueryObjectiv    = (void *)SDL_GL_GetProcAddress("glGetQueryObjectiv");
    p_glQueryCounter        = (void *)SDL_GL_GetProcAddress("glQueryCounter");
    /* Optional (XR colour program cleanup only): never fails modern-GL init. */
    p_glDeleteProgram       = (void *)SDL_GL_GetProcAddress("glDeleteProgram");
    /* Optional: only the debug presented-image ring maps pack buffers. */
    p_glMapBuffer           = (void *)SDL_GL_GetProcAddress("glMapBuffer");
    p_glUnmapBuffer         = (void *)SDL_GL_GetProcAddress("glUnmapBuffer");
    /* Optional: the present thread needs sync objects (GL 3.2). */
    p_glFenceSync           = (void *)SDL_GL_GetProcAddress("glFenceSync");
    p_glWaitSync            = (void *)SDL_GL_GetProcAddress("glWaitSync");
    p_glDeleteSync          = (void *)SDL_GL_GetProcAddress("glDeleteSync");
    if (p_glBindFramebuffer && p_glBindFramebuffer != gl_bind_fb_redirect) {
        p_glBindFramebuffer_raw = p_glBindFramebuffer;
        p_glBindFramebuffer = gl_bind_fb_redirect;
    }
#undef LOAD
    return ok;
}

/* ---- state ------------------------------------------------------------- */
static SDL_Window   *s_win = NULL;
static SDL_GLContext s_ctx = NULL;
static uint16_t     *s_vram = NULL;       /* CPU VRAM array (gpu.c's storage) */
static int           s_swap_interval = 1; /* SDL_GL swap interval (vsync mode) */

/* ---- render thread ([video] render_thread, docs/RENDER_THREAD.md) --------
 * While s_rth_on, the render thread owns the GL context and replays the
 * backend calls the emulation thread recorded (GL_RT_BACKEND, end of file).
 * A GL entry point reached on the emulation thread is a sync point
 * (GL_RT_SYNC): it drains the queue and moves the context to the emulation
 * thread, which then runs every call directly, exactly as without the render
 * thread, until the next frame boundary hands the context back.
 *
 * s_vram is the guest-visible VRAM (gpu.c's array) whenever the emulation
 * thread runs GL, and a private copy while the render thread does: replayed
 * uploads stage from the private copy, which receives each CPU->VRAM payload
 * in command order, so the emulation thread can keep writing gpu.c's array.
 *
 * Guest state the backend reads while drawing (per-primitive widescreen tags,
 * the native-wide fast-path latch, depth24) is captured when a call is
 * recorded and read back through the ctx_* accessors below during replay. */
#include "render_thread.h"
static int       s_rth_on = 0;
static uint16_t *s_rth_vram_pub = NULL;   /* gpu.c's VRAM (guest-visible)      */
static uint16_t *s_rth_vram_priv = NULL;  /* render thread's upload source     */
/* Replay-side capture of guest state (valid only on the render thread). */
static int       s_rthx_prim = 0;         /* current record carries prim tags  */
static int       s_rthx_tagged = 0, s_rthx_backdrop = 0;
static int       s_rths_flat_bd = 0, s_rths_vp_w = 0, s_rths_bg_full = 0;
static inline int rth_replaying(void) { return s_rth_on && rt_on_render_thread(); }
/* Frame generation (defined with the render-thread code near the end). */
static int       s_fg_on;
static int       s_fg_drawing, s_fg_presenting, s_fg_broken, s_fg_capture_real;
static void      fg_capture(const RtCmd *c, const void *payload);
static int       fg_on_present(uint16_t op, const uint8_t *p, uint32_t bytes, int stale);
static void      fg_flush(void);
static void      fg_invalidate(void);
static uint64_t  fg_tick(void *user, uint64_t now);
static void      fg_note_guest_frame(void);
extern int psx_ws_prim_is_tagged(void);   /* gpu.c: is the current GP0 prim sprite-tagged? */
extern int psx_ws_prim_in_backdrop(void); /* gpu.c: is its source addr in the flower-field struct? */
extern int gpu_ws_nw_flat_backdrop_enabled(void); /* gpu.c: per-title flat backdrop opt-in */
static int ctx_prim_tagged(void) {
    return rth_replaying() ? (s_rthx_prim ? s_rthx_tagged : 0) : psx_ws_prim_is_tagged();
}
static int ctx_prim_in_backdrop(void) {
    return rth_replaying() ? (s_rthx_prim ? s_rthx_backdrop : 0) : psx_ws_prim_in_backdrop();
}
static int ctx_flat_backdrop(void) {
    return rth_replaying() ? s_rths_flat_bd : gpu_ws_nw_flat_backdrop_enabled();
}
static int ctx_vp_width(void) {
    return rth_replaying() ? s_rths_vp_w : gpu_ws_netplay_local_viewport_width();
}
static int ctx_bg_full(void) {
    return rth_replaying() ? s_rths_bg_full : gpu_ws_background_requires_full_composite();
}
/* The render thread only ever replays 15-bit frames: a depth24 display is a
 * sync point (the emulation thread draws those frames itself). */
static int ctx_depth24(void) { return rth_replaying() ? 0 : gpu_display_is_depth24(); }
static void gl_rth_acquire(const char *reason);
#define GL_RT_SYNC(reason) \
    do { if (s_rth_on && !rt_on_render_thread()) gl_rth_acquire(reason); } while (0)
/* Recording (defined with GL_RT_BACKEND at the end of the file). */
enum {
    RTH_SEMI = 1, RTH_MASK, RTH_TWIN, RTH_MOD, RTH_PRECISE, RTH_PERSP,
    RTH_FILL, RTH_COPY, RTH_FLAT_TRI, RTH_GOURAUD_TRI, RTH_TEX_TRI,
    RTH_SHADED_TEX_TRI, RTH_FLAT_RECT, RTH_TEX_RECT, RTH_TEX_RECT_SCALED,
    RTH_LINE, RTH_SHADED_LINE, RTH_VRAM_WRITE, RTH_XFER_IN, RTH_AREA,
    RTH_OFFSET, RTH_WIDE_CONFIGURE, RTH_WIDE_VIEW, RTH_WIDE_TARGET,
    RTH_WIDE_DISABLE, RTH_WIDE_CLEAR, RTH_WIDE_CLEAR_MARGINS, RTH_PROJ_TRI,
    RTH_WIDE_RECOVERY, RTH_INTERP_SUSPENDED, RTH_PRESENT_VRAM,
    RTH_PRESENT_WIDE, RTH_STATE, RTH_PEEK, RTH_RING_CAPTURE, RTH_DYN_STEP,
    RTH_FRAME, RTH_FG_SRC, RTH_DEPTH, RTH_HD_NOTE, RTH_DITHER
};
/* GP0(A0) whose payload gpu.c is still streaming into its array (the
 * facade's vram_upload_open, emulation thread), tracked whatever the HD
 * state: under HD authority the emulation thread holds the context from the
 * header, or from the authority switch, to the commit. A residency note
 * (even a full-VRAM TRACK_UPLOAD) never ends it; only the commit, GP1(01h),
 * a reset or a savestate restored outside A0 do. */
static int s_rth_a0_open = 0;
/* HD authority requested while an A0 streams under GPU authority: its
 * received words exist only in gpu.c's array (the FBO gets the whole rect at
 * the commit), so ensure_cpu's readback would overwrite them. The switch
 * waits for the commit, which ends the upload in both copies. */
static int s_hd_authority_pending = 0;

static int  rth_record_mode(void);
/* Render-thread frame cost (dynamic resolution; defined with GL_RT_BACKEND). */
static uint64_t s_rthf_swap_ns = 0;        /* render thread: time in the swap */
static uint64_t host_now_ns_rthf(void);
/* The SW rasterizer and the HD texture residency read the same native VRAM
 * copy as the GL backend (gpu.c's array, or the render thread's private one). */
static void rth_rebind_vram(uint16_t *vram) {
    sw_renderer_rebind_vram(vram);
    gpu_hd_textures_bind_vram(vram);
}
static int  rth_rec_ints(uint16_t op, uint16_t flags, int n, const int32_t *v);
static uint16_t rth_prim_flags(void);
static int  rth_record_present(uint16_t op, int n, const int32_t *v);
static int  rth_mirror_wide_present_ok(int base_x);
static void rth_ring_capture_now(const int32_t *a);
/* Emulation-side mirror: the recorded wide target is a live surface. */
static int  s_rthm_cur = 0;
#define RTH_REC(op, fl, ...) do { const int32_t rth_v_[] = { __VA_ARGS__ }; \
    rth_rec_ints((op), (fl), (int)(sizeof rth_v_ / sizeof rth_v_[0]), rth_v_); } while (0)

/* ---- Two scales ------------------------------------------------------------
 * s_hr_scale   the scale of s_hr_fbo, the authoritative VRAM surface, and of
 *              everything that reads, mirrors, backs up or restores it: hr
 *              draws and scissors, pack, CPU readback, VRAM copies and their
 *              scratch, mask stencils on hr.
 * s_out_scale  the internal resolution presented (gr_scale/glb_scale): the
 *              high-resolution window (s_hiw_t tiles), the native-wide surfaces,
 *              present, hold-last, interpolation captures, screenshot_hires.
 * They are equal except in windowed high-resolution mode (s_hiw, below),
 * where s_hr_scale is 1 and the presented image lives in the window, not in
 * s_hr_fbo. Code that reads s_hr_fbo at s_out_scale, or takes the presented
 * image from s_hr_fbo, is wrong only in that mode. The old single name
 * s_scale is therefore poisoned below: code written against it (an older
 * branch) fails to build instead of merging silently, and each use must pick
 * one of the two. A transaction that backs up and restores VRAM must also
 * back up and restore the window's rows while hiw_on(), or refuse to run. */
static int           s_out_scale = 1;
static int           s_hr_scale = 1;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC poison s_scale
#endif
/* Dynamic internal resolution ([video] dynamic_resolution; the step block
 * near the end of this file). 0 = off, and then every surface is allocated at
 * the scale it renders at, exactly as before. Otherwise the scale the hr
 * ceiling: the highest level (the scale the surfaces were first allocated
 * at, which the GPU limit and memory budget were checked against). Every
 * surface is allocated at the current level (s_hr_scale == s_out_scale,
 * never above the ceiling) and reallocated at each step (dyn_apply): on
 * Apple's GL-on-Metal every render pass into an attachment loads and stores
 * the whole attachment, so a surface held at the ceiling cost the ceiling's
 * bandwidth at every level. s_hr_alloc and s_wide_as hold the allocations
 * (above the level only where a reallocation failed). */
static int           s_alloc_scale = 0;
/* The scale a surface rendering at `cur` is (to be) allocated at. */
static inline int    alloc_scale_for(int cur) {
    return s_alloc_scale > cur ? s_alloc_scale : cur;
}
static void          dyn_context_ready(void);   /* after init_gpu_raster */
static void          dyn_after_present(void);   /* a step's post-present point */
static int           s_dyn_on = 0;              /* gl_renderer_set_dynamic_resolution */
static GlHostLedger  s_dyn_ledger;              /* host time totals (s_dyn_on only) */
static uint64_t      s_dyn_last_swap_ticks = 0;
/* Netplay: SW@1× + GPU@s_out_scale dual write; CPU VRAM always authoritative. */
static int           s_cpu_auth_dual = 0;
/* Independent of netplay: the replacement surface must never become VRAM. */
static int           s_hd_native_authority = 0;
static int           s_req_scale = 1;      /* requested before context init */
/* Driver limits queried at init: min(GL_MAX_TEXTURE_SIZE,
 * GL_MAX_RENDERBUFFER_SIZE, GL_MAX_VIEWPORT_DIMS); 0 before init. */
static int           s_gl_max_dim = 0;
static int           s_scale_clamp_reason = 0;   /* PSX_GL_SCALE_* mask */
static int           s_scale_alloc_retries = 0;  /* FBO failures stepped down */
static uint64_t      s_vram_budget = 0;          /* bytes; 0 = none */
static int           s_wide_refused_logged = 0;

/* ---- windowed high-resolution surface (true 8K on 16384-limit GPUs) -------
 * The full-VRAM hr surface is 1024*S wide, so a 16384 texture limit stops it
 * at 16x (3840 lines). 8K needs 18x. In windowed mode the AUTHORITATIVE VRAM
 * stays the ordinary hr surface at 1x — exactly the native renderer, so pack,
 * CPU readback, render-to-texture and copies are the 1x results — and a
 * second, presentation-only surface at S covers just the columns the game
 * displays: W = [x0, x1) x full VRAM height, W*S x 512*S. Every
 * GPU write that touches W (draws, fills, copies, uploads, the depth24 clear)
 * is mirrored into it, like the native-wide surfaces; present, hold-last,
 * interpolation and the wide centre read it. W starts empty and grows to the
 * union of displayed rects (rounded to 64 px) the first time each is shown,
 * seeded by upscaling the 1x content, so it never needs to guess the layout.
 * When the union no longer fits one surface (side-by-side 512-wide buffers at
 * 18x need 18432 px), W becomes up to HIW_MAX_TILES tiles, each a column
 * range of its own surface; every write goes to each tile it touches, so a
 * game flipping between horizontally adjacent buffers presents every buffer
 * at S. Tiles may overlap; overlapping columns hold identical pixels.
 * A copy whose source lies outside every tile is taken from the 1x surface
 * (nearest). Engaged only when the full-VRAM surface cannot hold the
 * requested scale (driver limit or memory budget), or with
 * PSX_GL_HIRES_WINDOW=1. */
#define HIW_ALIGN 64
#define HIW_MAX_TILES 4
typedef struct HiwTile {
    int    x0, x1;                              /* native columns [x0, x1) */
    GLuint tex, fbo, rb;
} HiwTile;
static int           s_hiw = 0;                 /* windowed mode engaged */
static HiwTile       s_hiw_t[HIW_MAX_TILES];    /* empty until a display is shown */
static int           s_hiw_n = 0;
static int           s_hiw_last_x0 = 0, s_hiw_last_x1 = 0; /* last presented columns */
/* Staging for a copy's S-scaled source (hiw_mirror_copy), apart from the
 * shared scratch that holds the 1x source. */
static GLuint        s_hiw_scratch_tex = 0, s_hiw_scratch_fbo = 0;
static int           s_hiw_scratch_w = 0, s_hiw_scratch_h = 0;
static int           s_hiw_refused_logged = 0;
static int           s_hiw_grows = 0;           /* tile allocations (diagnostic) */
static int  hiw_on(void);
static const HiwTile *hiw_ensure(int x0, int x1);
static void hiw_flush_queue(void);
static void wst_rebuild(int i, int defer);
static int  wide_fast_center_valid(void);
static GLuint make_tex(GLenum internal, int w, int h, GLenum fmt, GLenum type);
static int  make_fbo(GLuint *out_fbo, GLuint color_tex, GLuint stencil_rb);
static GLenum s_last_fbo_status;
static void hiw_clear_rect(int x, int y, int w, int h, float r, float g, float b,
                           float a, int stencil);
static void hiw_mirror_copy(int sx, int sy, int dx, int dy, int w, int h);

static GLuint        s_present_tex = 0;    /* CPU-readout present path (24bpp) */
static int           s_present_w = 0, s_present_h = 0;
static GLuint        s_osd_tex = 0;
static int           s_osd_tw = 0, s_osd_th = 0;
static GLuint        s_present_prog = 0, s_present_vao = 0;
static GLuint        s_xr_color_prog = 0, s_xr_native_tex = 0;
static void          gl_swap_with_osd(void);
static int           s_post_aa = 0;   /* GL_POST_AA_*; post-process AA (post_aa_apply) */
static int s_native_surface_enabled, s_native_surface_pending;
static int s_native_surface_rect[4]; /* Fresh native backbuffer content, GL coordinates. */
static double s_native_surface_distance, s_native_surface_width, s_native_surface_units;
static void openxr_present_native(void);
static GLint         s_present_uTex = -1, s_present_uUvRect = -1;
static GLint         s_present_uTexSize = -1, s_present_uSharpScale = -1;
static GLint         s_present_uSharp = -1;
static GLint         s_present_uGamma = -1;
/* Host-side presentation state. The shader applies this only to the source
 * image being presented; overlays and already-composed hold-last images set
 * the uniform back to the identity explicitly. */
static float         s_present_gamma = 1.0f;
/* Scanline post-process state (host display setting; see gl_renderer_set_scanlines).
 * s_scanline_on/strength are pushed to whichever program draws game content;
 * OSD/bezel and the already-composed hold-last re-present force it off so the
 * effect is applied exactly once, at native-line pitch. */
static int           s_scanline_on = 0;
static float         s_scanline_strength = 0.5f;
static GLint         s_present_uScanline = -1, s_present_uScanStrength = -1;
static GLint         s_present_uScanLines = -1, s_present_uScanScale = -1;
static GLuint        s_interp_prog = 0, s_interp_tex[3];
static GLint         s_interp_uPrev = -1, s_interp_uCurr = -1;
static GLint         s_interp_uAlpha = -1, s_interp_uUvRect = -1;
static GLint         s_interp_uBlendMode = -1, s_interp_uGamma = -1;
static GLint         s_interp_uScanline = -1, s_interp_uScanStrength = -1;
static GLint         s_interp_uScanLines = -1, s_interp_uScanScale = -1;
static int           s_interp_enabled = 0, s_interp_valid = 0;
static int           s_interp_suspended = 0;
static int           s_interp_blend_mode = 0;
static int           s_interp_prev = 0, s_interp_cur = 0;
static int           s_interp_w = 0, s_interp_h = 0, s_interp_linear = 0;
static int           s_interp_src_w = 0, s_interp_src_h = 0; /* source band, S px */
static GLuint        s_interp_fbo = 0;   /* draw target of a scaled capture */
static int           s_interp_force_4_3 = 0, s_interp_source_path = -1;
static uint64_t      s_interp_swaps = 0;
static uint64_t      s_interp_captures = 0;
static int           s_interp_diag = 0;
static double        s_interp_host_hz = 0.0;
static double        s_interp_target_hz = 0.0;
static double        s_interp_source_hz = 0.0;
static FrameInterpolationSchedule s_interp_schedule;
/* Blend source (psx_mod_set_frame_interpolation_source). VBLANK (0, the
 * default) treats every guest VBlank present as a new frame, exactly as
 * before. FLIP (1) rotates history only on a real flip -- the display origin
 * moved or the displayed rect was redrawn -- and spreads one crossfade over
 * the measured flip period, so 30 Hz content blends for the whole frame
 * instead of blending for one VBlank and holding for the next. */
static int           s_interp_source = 0;
static FrameFlipTracker s_interp_flip;
static int           s_interp_origin_x = -1, s_interp_origin_y = -1;
static double        s_interp_phase_lo = 0.0, s_interp_phase_hi = 1.0;
static uint64_t      s_interp_duplicates = 0;
/* PSX_MOD_FRAME_INTERPOLATION_HOLD: no crossfade, repeat the newest frame
 * wherever render passes supply no in-between image. */
static int           s_interp_hold = 0;
static void pass_gens_invalidate(void);
static void pass_note_new_frame(int origin_x, int origin_y, int source_path,
                                int pw, int ph);
static void pass_apply_promotion(void);
static int pass_gen_present(uint64_t deadline);
static int stereo_present(int w, int h);
static void stereo_resources_release(void);
static void stereo_invalidate(void);
static uint64_t s_idle_ticks_accum_fwd(uint64_t add);
/* Render-pass VRAM transaction (see "Render passes" below). While a pass is
 * open, GPU writes are confined to its rect: the scissor is intersected with
 * it and writes that bypass the scissor are refused and counted. */
static int      s_pass_active = 0;
/* The open transaction is a netplay local view (gl_renderer_local_view_*):
 * its draws reach only the presenter surface, never the authoritative CPU
 * VRAM, and end(keep) keeps the rect's presented colour. */
static int      s_pass_local = 0;
static uint64_t s_local_commits = 0;
/* Textures/framebuffers made for passes (pass_make_color_fbo, pass image
 * slots), and the count when the current pass began: a pass that allocated
 * is left out of the pass-cost average (render_pass_cost_add). */
static uint32_t s_pass_allocs = 0, s_pass_allocs_begin = 0;
static int      s_pass_x = 0, s_pass_y = 0, s_pass_w = 0, s_pass_h = 0;
static uint32_t s_pass_leaks = 0;
/* Policy and CPU rows of the out-of-rect journal (render_pass_plan.c,
 * render_pass_sandbox_test); the GPU copies live in s_pj below. */
static RenderPassJournal s_pj_cpu;
static int pass_journal_protect(int x, int y, int w, int h);
/* A fill, copy or upload outside the pass rect (e.g. a game that moves a
 * few pixels of VRAM as part of every frame) is journaled -- its destination
 * backed up and restored at the end of the pass -- or, when the journal is
 * full, refused and counted (the pass is then rolled back without an image).
 * Native-wide needs nothing more: fills, copies, uploads and pokes write the
 * hr surface, the raw mirror and the CPU rows only, never a wide surface
 * (the wide margins come from mirrored draws, which the pass scissor keeps
 * inside the pass band). */
static int pass_refuse_write(const char *what, int x, int y, int w, int h) {
    int policy;
    if (!s_pass_active) return 0;
    policy = render_pass_vram_policy(&s_pj_cpu, s_pass_x, s_pass_y, s_pass_w,
                                     s_pass_h, VRAM_W, VRAM_H, 1,
                                     &x, &y, &w, &h);
    if (policy == RENDER_PASS_VRAM_ALLOW) return 0;
    if (policy == RENDER_PASS_VRAM_JOURNAL && pass_journal_protect(x, y, w, h))
        return 0;
    if (s_pass_leaks < 8)
        fprintf(stderr, "psxrecomp: render pass refused %s %d,%d %dx%d outside "
                "%d,%d %dx%d\n", what, x, y, w, h, s_pass_x, s_pass_y,
                s_pass_w, s_pass_h);
    s_pass_leaks++;
    return 1;
}
static void interp_reset_history(void);
static int interp_present(float alpha);
static void interp_present_source_interval(void);
static void present_bezel(int ww, int wh, int lx, int ly, int lw, int lh);

static int           s_raster_ok = 0;      /* full GPU pipeline available */
static GLuint s_bank_tex[65536];
static GLuint s_selected_bank_tex;
static int s_selected_bank_live_clut, s_tb_bank_live_clut;

/* Authoritative VRAM: hr color texture + stencil (mask bit) FBO. */
static GLuint        s_hr_tex = 0, s_hr_fbo = 0, s_hr_rb = 0;
/* The scale s_hr_tex/s_hr_rb are allocated at (dynamic resolution
 * reallocates them at every step; see s_alloc_scale). */
static int           s_hr_alloc = 0;
/* Native raw-1555 sampling mirror + readback source. */
static GLuint        s_raw_tex = 0, s_raw_fbo = 0;
/* CPU->VRAM upload staging (native RGBA8). */
static GLuint        s_up_tex = 0;
/* copy_rect staging (hr-sized RGBA8). */
static GLuint        s_scratch_tex = 0, s_scratch_fbo = 0;
static int           s_scratch_w = 0, s_scratch_h = 0;

/* Programs. */
static GLuint s_geo_prog = 0, s_geo_vao = 0, s_geo_vbo = 0;
static GLuint s_tex_prog = 0, s_tex_vao = 0, s_tex_vbo = 0;
/* Textured vertex: pos(2) uv(2) col(4) tpage(2) clut(2) depth(1) raw(1) limits(4)
 * semi(1) q(1) twin(1)
 * — per-prim texture state in flat attributes so prims batch (see flush_tex_batch).
 * q is the perspective weight ([video] perspective_texturing): 0 = affine, the
 * PS1-faithful default, which makes the vertex shader's w exactly 1.0 and the
 * fragment shader read the noperspective varying — i.e. bit-identical to the
 * pre-feature pipeline. twin is the prim's GP0(E2h) texture window, its low 20
 * bits as a whole float (mask x, mask y, offset x, offset y; 5 bits each). */
#define TEXV 26
static GLuint s_blit_prog = 0, s_blit_vao = 0, s_blit_vbo = 0;
static GLuint s_blit_hi_prog = 0;            /* windowed hi surface blit */
static GLint  s_uBhSrc = -1, s_uBhPass = -1, s_uBhMaskset = -1, s_uBhSrcDiv = -1;
static GLint  s_uBhSrcOff = -1, s_uBhShift = -1, s_uBhXoff = -1, s_uBhXhalf = -1;
/* Sample-grid shift per surface scale (see GEO_VS): the hr surface at
 * s_hr_scale, the hi window and the wide surfaces at s_out_scale. */
static float  s_shift_hr = 0.0f, s_shift_hi = 0.0f;
static GLint  s_geo_uShift = -1, s_tex_uShift = -1;
static GLuint s_pack_prog = 0, s_stencil_prog = 0, s_empty_vao = 0;

/* Sub-pixel vertex override + perspective UV weights for the next triangle
 * ([video] geometry_correction / perspective_texturing; see gpu_render.h).
 * gpu.c sets these immediately before the matching gr_draw_*_triangle and they
 * are consumed by it. All-zero == the faithful integer/affine path. */
static int     s_pc_valid = 0;                  /* sub-pixel positions present */
static int s_projected_uv_valid;
static float s_projected_u[3], s_projected_v[3];
static float   s_pc_x[3], s_pc_y[3];            /* native VRAM px, fractional  */
static int     s_pq_valid = 0;                  /* perspective weights present */
static float   s_pq[3];
/* PGXP depth for the next triangle (gr_set_depth_triangle): GTE SZ per
 * vertex, 1..65535, from validated dataflow shadows. */
static int     s_pz_valid = 0;
static float   s_pz[3];
/* PGXP renderer features (docs/ENHANCEMENTS.md G1.14), all off by default:
 * depth buffer for opaque 3D polygons, perspective-correct Gouraud colour,
 * and the seam expansion of opaque 3D polygons (0 off, 1 = 1 output px
 * above 1x, 2 = half a native px). */
static int     s_pgxp_depth = 0, s_pgxp_cpersp = 0, s_pgxp_seam = 0;
static float   s_pgxp_depth_threshold = 4096.0f;   /* SZ units, as DuckStation */
static int     s_depth_need_clear = 1, s_depth_used = 0;
static float   s_depth_last_avg = 0.0f;
static uint64_t s_depth_clears = 0, s_depth_tris = 0, s_seam_tris = 0;

/* TEX program uniforms. */
static GLint s_uVram = -1, s_uTpage = -1, s_uClut = -1, s_uDepth = -1;
static GLint s_uHdTexture = -1;
static GLint s_uPalette = -1;
static GLint s_uRaw = -1, s_uSemipass = -1, s_uSemimode = -1;
static GLint s_uMaskset = -1, s_uFilter = -1;
static GLint s_uLimits = -1;
/* Native-wide x-projection uniforms (per program). u_xoff = x translation in
 * native px (0 canonical), u_xhalf = x clip half-extent in native px (512
 * canonical). When wide is off these stay 0 / 512 so the canonical pass is
 * bit-identical to the pre-native-wide projection. */
static GLint s_geo_uXoff = -1, s_geo_uXhalf = -1;
static GLint s_tex_uXoff = -1, s_tex_uXhalf = -1;
/* Native-wide 2D-backdrop x-stretch uniforms. The far 2D backdrop layer is a
 * ~4:3-width set of static prims scrolled by the draw offset; native-wide widens
 * 3D via the GTE but never these, so they leave a void in the 16:9 margins. The
 * wide mirror scales them about screen centre (u_xscale, u_xcenter) to fill it.
 * Applied only to "backdrop-phase" prims (drawn before the first clearly-wide
 * prim each frame). 1.0 / 0.0 => no-op, so the canonical pass stays identical. */
static GLint s_geo_uXscale = -1, s_geo_uXcenter = -1;
static GLint s_tex_uXscale = -1, s_tex_uXcenter = -1;
/* Runtime controls (ws_backdrop_stretch debug command). */
int g_ws_bd_stretch_on   = 1;   /* feature on (gated by native-wide + per-prim gate) */
int g_ws_bd_stretch_pct  = 0;   /* 0 = auto (g_wide_w/native_w); else pct/100 */
int g_ws_bd_phase_thresh = 24;  /* "narrow" margin (px): a prim overhanging the 4:3
                                 * region by more than this is treated as already-
                                 * widened GTE geometry and NOT stretched */
int g_ws_bd_phase_mode   = 1;   /* (retained for the debug command; unused since the
                                 * gate is now per-prim !tagged && narrow) */
/* Per-prim 2D-backdrop gate (replaces the old draw-order phase): a prim is
 * stretched iff native-wide + feature on + NOT sprite-tagged (foreground chars/
 * HUD are tagged) + NARROW (the GTE far-parallax/3D extend into the margins).
 * s_bd_gate is what wide_set_bd_scale reads; s_tb_gate is the open textured
 * batch's gate (batch flushes when a prim's gate differs, so a batch is uniform). */
static int s_bd_gate = 0;
static int s_tb_gate = 0;
/* ws_backdrop_stretch diagnostics: per-frame snapshot reported by the command. */
int g_bdg_applied = 0, g_bdg_prims = 0, g_bdg_clearx = -999999;
int g_bdg_cur = 0, g_bdg_base = 0, g_bdg_w = 0, g_bdg_off = 0;
static int s_bdg_applied = 0, s_bdg_prims = 0, s_bdg_clearx = -999999;
/* per-prim draw-order trace (ws_backdrop_trace): x-extent + textured flag, in
 * draw order, for the last frame -- so we can SEE where the background sits. */
typedef struct { short x0, x1, y0, y1; unsigned char tex; } PrimRec;
#define PTRACE_CAP 200
static PrimRec s_ptrace[PTRACE_CAP]; static int s_ptrace_n = 0;
PrimRec g_ptrace[PTRACE_CAP]; int g_ptrace_n = 0;   /* snapshot (extern) */
/* BLIT program uniforms. */
static GLint s_uBlitSrc = -1, s_uBlitPass = -1, s_uBlitMaskset = -1;
static GLint s_uBlitSrcDiv = -1, s_uBlitSrcOff = -1;
/* PACK program uniforms. */
static GLint s_uPackHr = -1, s_uPackScale = -1;
static GLint s_uStencilSrc = -1, s_uStencilOff = -1;

static uint32_t     *s_conv = NULL;        /* RGBA8 staging for uploads      */
static int           s_gpu_dirty = 0;      /* CPU VRAM array may be stale    */

/* Dirty-rect unions, native VRAM coords, inclusive bounds. */
typedef struct { int x0, y0, x1, y1, set; } DirtyRect;
static DirtyRect s_cpu_dirty; /* conservative GPU-written area not yet read into CPU VRAM */
static DirtyRect s_pack_dirty;             /* hr FBO content not in raw mirror */
/* Native-coords union of primitive bboxes since the last stencil rebuild: the
 * only area whose stencil can lag its alpha (deferred-stencil draws). Used at
 * S > 1 so a mask-check toggle rebuilds that rect, not the whole surface. */
static DirtyRect s_stencil_stale;
static void hiw_mirror_uploads(const DirtyRect *rects, int nrects);

/* CPU writes not yet in the FBO — an EXACT rect list, NOT a single union.
 *
 * THE FLICKER CLASS BUG (MMX6 GL black-frame flicker, ISSUES.md #7): the old
 * single-union s_up_pending merged DISJOINT uploads (e.g. a sprite column at
 * x>=320 and a tile row at y>=480) into one bounding box that covered the
 * framebuffers in between; flush_cpu_upload then painted that whole box from
 * the CPU VRAM array — which is STALE under GL (the FBO is authoritative,
 * ensure_cpu only syncs on demand) — stomping freshly-rendered framebuffer
 * content with stale (typically black) pixels for 1-2 presents until the game
 * redrew each buffer. Fix: track the exact uploaded rects and flush each one;
 * only pixels the CPU actually wrote are ever painted. Merging is allowed only
 * when it adds NO uncovered pixels (containment / same-band extension). On
 * overflow the pending set is flushed and the new rect starts a fresh list —
 * order-preserving and still batched for the common poke patterns.
 * (Pre-context, s_raster_ok == 0, the CPU array is fully authoritative — the
 * software rasterizer mirrors every draw — so union-merging is harmless and
 * used as the overflow strategy there.) */
#define UP_RECTS_MAX 16
static DirtyRect s_up_rects[UP_RECTS_MAX];
static int       s_up_nrects = 0;
static uint64_t  s_rt_up_diag[6];

static int runtime_upload_diag_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PSX_RUNTIME_PERF_DIAG");
        enabled = e && e[0] && e[0] != '0';
    }
    return enabled;
}

void gl_renderer_runtime_diag(uint64_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = s_rt_up_diag[i];
}

/* Draw state mirrored from the vtable set_* calls. */
static int s_off_x = 0, s_off_y = 0;
static int s_area_x1 = 0, s_area_y1 = 0, s_area_x2 = VRAM_W - 1, s_area_y2 = VRAM_H - 1;
static int s_semi_en = 0, s_semi_mode = 0;
static int s_mod_r = 128, s_mod_g = 128, s_mod_b = 128, s_mod_raw = 0;
static int s_mask_set = 0, s_mask_check = 0;
static int s_tw_mask_x = 0, s_tw_mask_y = 0, s_tw_off_x = 0, s_tw_off_y = 0;
static int s_tex_filter = 0;
/* [video] dithering (see psx_dither in the shaders): 0 off = true colour (the
 * historical path), 1 the PS1 4x4 pattern at internal resolution, 2 scaled to
 * the native pixel grid. s_dither_bit is the last GP0(E1h) bit 9; the shaders
 * only ever see s_dither_mode while the guest has dithering enabled. */
static int s_dither_mode = 0, s_dither_bit = 0, s_dither_live = 0;
/* [video] texture_lod (0 off, 1 mipmap emulation) and anisotropic_filtering
 * (taps along the footprint's major axis, 1..16). */
static int s_tex_lod = 0, s_tex_aniso = 1;
/* Opaque textured draws carry the exact mask bit in FBO alpha. Keeping the
 * duplicate stencil copy current is deferred until mask checking is requested. */
static int s_stencil_valid = 1;

/* ---- native-wide compositor (mirrors gpu_sw_renderer.c g_wide_*) ----------
 * Canonical VRAM (the hr FBO) stays 100% faithful. Native-wide lives in
 * SEPARATE wide FBOs keyed by framebuffer base_x; each framebuffer-targeting
 * primitive is mirrored into the active wide surface at local x = vram_x -
 * base_x + OFFSET. Present reads the displayed buffer's wide surface. Textures
 * always sample canonical VRAM (s_raw_tex). 4:3 / non-opted games never call
 * wide_configure, so g_wide_cur stays 0 and the wide pass never runs. */
#define WIDE_MAX_SURF 4
static GLuint s_wide_tex[WIDE_MAX_SURF];     /* color tex per surface (0 = free) */
static GLuint s_wide_fbo[WIDE_MAX_SURF];     /* FBO per surface */
static GLuint s_wide_rb[WIDE_MAX_SURF];      /* depth-stencil RB per surface (mask
                                              * mirror, same as hr). PERF-CRITICAL:
                                              * a stencil-less wide FBO made every
                                              * stencil-enabled mirror draw cost
                                              * ~0.6ms of driver work (Tomba2 16:9
                                              * collapsed to 12fps); with the RB
                                              * attached the pass costs ~2us. */
static int    s_wide_base[WIDE_MAX_SURF];    /* base_x per surface (-1 = free) */
/* Native rows [y0, y1) each surface has presented (dynamic resolution: the
 * rows a scale step rescales; y1 <= y0 = none yet, then all of them). */
static int    s_dyn_wide_y0[WIDE_MAX_SURF], s_dyn_wide_y1[WIDE_MAX_SURF];
/* The scale each surface is allocated at. Dynamic resolution allocates a wide
 * surface at the level it renders at (not the ceiling) and reallocates it at
 * every step: on Apple's GL-on-Metal every render pass into an attachment
 * loads and stores the whole attachment, so a surface allocated at the
 * ceiling costs the ceiling's bandwidth at every level. */
static int    s_wide_as[WIDE_MAX_SURF];
static int    g_wide_w        = 0;           /* wide width (native px); 0 = disabled */
static int    g_wide_off      = 0;           /* centering OFFSET (native px) */
static GLuint g_wide_cur      = 0;           /* active mirror FBO (0 = no mirror) */
static int    g_wide_cur_base = 0;           /* base_x of g_wide_cur */
/* Set by gpu_flat_rect for the full-screen-overlay case so the generic
 * gpu_geometry wide mirror is skipped and the flat path emits its own
 * full-wide-width pass instead (mirrors sw_draw_flat_rect). */
static int    s_wide_suppress = 0;
/* Diagnostic only. The frontend admits proven world geometry into both the
 * canonical and wide passes so a face cannot split at the center-copy edge. */
static uint64_t s_wide_triangle_recovery_count;
void gl_renderer_note_wide_triangle_recovery(int recovered) {
    if (rth_record_mode()) { RTH_REC(RTH_WIDE_RECOVERY, 0, recovered); return; }
    if (recovered && s_raster_ok && g_wide_cur) ++s_wide_triangle_recovery_count;
}
uint64_t gl_renderer_wide_triangle_recovery_count(void) {
    GL_RT_SYNC("wide_triangle_recovery_count");
    return s_wide_triangle_recovery_count;
}


/* X-translation (native px) from canonical VRAM space into the active wide
 * surface: local_x = vram_x - base_x + OFFSET. Same as SW wide_dx(). */
static int view_enabled, view_shift, view_pad_left, view_pad_right;
static inline int wide_dx(void) { return g_wide_off + view_shift - g_wide_cur_base; }

/* Wide-surface mask stencil: the rect (wide-local native px, [x0,x1) x
 * [y0,y1)) where a surface's stencil may differ from its alpha (bit 15), per
 * surface. At S > 1 the rebuild (rebuild_mask_stencils) covers only this rect
 * instead of the whole surface: the stencil equals alpha everywhere else, so
 * rebuilding it there would change nothing. Every write that can leave the
 * stencil behind alpha adds to it: a mirror draw without the mask check (its
 * bbox), the centre splice (wide_blit_center, colour only), a scale step's
 * in-place rescale and a render pass's restore (colour only), a new surface.
 * Clears write both and add nothing. While the centre is spliced from the hr
 * surface at every present (wide_fast_center_valid), the centre columns'
 * stencil is never observed (their pixels are overwritten before anything
 * reads them), so a rebuild leaves the centre part stale and it is rebuilt
 * only if a mask-checked mirror draw meets it with the splice off
 * (wide_stencil_ready). A whole-surface rebuild was the cost of every
 * GP0(E6h) mask-check enable at high internal scales (R4 2P VS: ~85% of the
 * emulation thread at 10x, waiting on Metal command buffers). */
static int    s_wst_x0[WIDE_MAX_SURF], s_wst_y0[WIDE_MAX_SURF];
static int    s_wst_x1[WIDE_MAX_SURF], s_wst_y1[WIDE_MAX_SURF];
static uint64_t s_wst_rebuilds = 0, s_wst_px = 0, s_wst_deferred = 0;
/* PSX_GL_WIDE_STENCIL_FULL=1: rebuild whole surfaces, as before (A/B). */
static int wst_full(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PSX_GL_WIDE_STENCIL_FULL"); v = e && e[0] == '1'; }
    return v;
}
static int wide_index(GLuint fbo) {
    for (int i = 0; fbo && i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] == fbo) return i;
    return -1;
}
static void wst_add(int i, int x0, int y0, int x1, int y1) {
    if (i < 0 || i >= WIDE_MAX_SURF) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_wide_w) x1 = g_wide_w;
    if (y1 > VRAM_H) y1 = VRAM_H;
    if (x1 <= x0 || y1 <= y0) return;
    if (s_wst_x1[i] <= s_wst_x0[i] || s_wst_y1[i] <= s_wst_y0[i]) {
        s_wst_x0[i] = x0; s_wst_y0[i] = y0; s_wst_x1[i] = x1; s_wst_y1[i] = y1;
        return;
    }
    if (x0 < s_wst_x0[i]) s_wst_x0[i] = x0;
    if (y0 < s_wst_y0[i]) s_wst_y0[i] = y0;
    if (x1 > s_wst_x1[i]) s_wst_x1[i] = x1;
    if (y1 > s_wst_y1[i]) s_wst_y1[i] = y1;
}
static void wst_all(int i) { wst_add(i, 0, 0, g_wide_w, VRAM_H); }
static void wst_clear(int i) { s_wst_x0[i] = s_wst_y0[i] = s_wst_x1[i] = s_wst_y1[i] = 0; }
/* A mirror draw of `n` vertices (x at v[0], y at v[1], `stride` floats
 * apart, canonical VRAM px) into surface `fbo`, shifted by dx and stretched
 * by (scale, centre) exactly as GEO_VS/TEX_VS do; check: the mask check it
 * was drawn under (checked draws keep the stencil equal to alpha). The bbox
 * is widened by 2 px each way for sub-pixel positions and line quads. */
static void wst_note_draw(GLuint fbo, const float *v, int n, int stride,
                          int dx, float scale, float centre, int check) {
    if (check || n <= 0) return;
    int i = wide_index(fbo);
    if (i < 0) return;
    float lo = v[0], hi = v[0], ylo = v[1], yhi = v[1];
    for (int k = 1; k < n; k++) {
        float x = v[(size_t)k * stride], y = v[(size_t)k * stride + 1];
        if (x < lo) lo = x; if (x > hi) hi = x;
        if (y < ylo) ylo = y; if (y > yhi) yhi = y;
    }
    if (scale != 1.0f) {   /* monotonic in x: transform the ends */
        float t[2] = { lo, hi };
        for (int k = 0; k < 2; k++) {
            float xb = t[k];
            if (scale < 0.0f) {
                float s = -scale, h = ((float)g_wide_w / 2.0f) / s;
                float l = centre - h, r = centre + h;
                if (xb < l) xb = l + (xb - l) * s; else if (xb > r) xb = r + (xb - r) * s;
            } else {
                xb = (xb - centre) * scale + centre;
            }
            t[k] = xb;
        }
        lo = t[0]; hi = t[1];
    }
    wst_add(i, (int)floorf(lo) + dx - 2, (int)floorf(ylo) - 2,
            (int)ceilf(hi) + dx + 3, (int)ceilf(yhi) + 3);
}

/* ---- dirty-rect helpers ------------------------------------------------- */
static void rect_clear(DirtyRect *r) { r->set = 0; }
static void rect_add(DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    if (!r->set) { r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; r->set = 1; return; }
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}
static int rect_intersects(const DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (!r->set) return 0;
    return !(x1 < r->x0 || x0 > r->x1 || y1 < r->y0 || y0 > r->y1);
}

/* ---- exact pending-upload rect list (see s_up_rects comment) ------------- */
static void flush_cpu_upload(void);   /* fwd: overflow flushes then re-adds */

/* Add an uploaded rect. Merges ONLY when the merge adds no uncovered pixels:
 * containment either way, or an extension within the same row-band / column-
 * band (equal y-range with touching/overlapping x-ranges, or equal x-range
 * with touching/overlapping y-ranges — the row-scan / column-scan poke
 * patterns). Never unions disjoint rects post-init (that was the flicker bug). */
static void up_add(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int i = 0; i < s_up_nrects; i++) {
        DirtyRect *r = &s_up_rects[i];
        if (x0 >= r->x0 && x1 <= r->x1 && y0 >= r->y0 && y1 <= r->y1)
            return;                                   /* contained */
        if (x0 <= r->x0 && x1 >= r->x1 && y0 <= r->y0 && y1 >= r->y1) {
            r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;  /* contains */
            return;
        }
        if (y0 == r->y0 && y1 == r->y1 &&
            x0 <= r->x1 + 1 && x1 >= r->x0 - 1) {     /* same row-band extension */
            if (x0 < r->x0) r->x0 = x0;
            if (x1 > r->x1) r->x1 = x1;
            return;
        }
        if (x0 == r->x0 && x1 == r->x1 &&
            y0 <= r->y1 + 1 && y1 >= r->y0 - 1) {     /* same column-band extension */
            if (y0 < r->y0) r->y0 = y0;
            if (y1 > r->y1) r->y1 = y1;
            return;
        }
    }
    if (s_up_nrects >= UP_RECTS_MAX) {
        if (s_raster_ok) {
            flush_cpu_upload();       /* order-preserving: land the old ones */
        } else {
            /* Pre-context: the CPU array is fully authoritative (software
             * rasterizer mirrors every op), so a union is harmless. */
            DirtyRect *r = &s_up_rects[0];
            for (int i = 1; i < s_up_nrects; i++) {
                if (s_up_rects[i].x0 < r->x0) r->x0 = s_up_rects[i].x0;
                if (s_up_rects[i].y0 < r->y0) r->y0 = s_up_rects[i].y0;
                if (s_up_rects[i].x1 > r->x1) r->x1 = s_up_rects[i].x1;
                if (s_up_rects[i].y1 > r->y1) r->y1 = s_up_rects[i].y1;
            }
            if (x0 < r->x0) r->x0 = x0;
            if (y0 < r->y0) r->y0 = y0;
            if (x1 > r->x1) r->x1 = x1;
            if (y1 > r->y1) r->y1 = y1;
            s_up_nrects = 1;
            return;
        }
    }
    DirtyRect *r = &s_up_rects[s_up_nrects++];
    r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1; r->set = 1;
}

/* Add a GP0(A0) transfer's exact touched region. The software reference wraps
 * per pixel (px = (x+col) & 1023, py = (y+row) & 511), so a wrapping transfer
 * touches up to four exact rects — NOT all of VRAM (the old "wrapped: take
 * all" full-VRAM union painted stale CPU content over the framebuffers). */
static void up_add_transfer(int x, int y, int w, int h) {
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    if (w <= 0 || h <= 0) return;
    int w1 = w, w2 = 0, h1 = h, h2 = 0;
    if (x + w > VRAM_W) { w1 = VRAM_W - x; w2 = w - w1; }
    if (y + h > VRAM_H) { h1 = VRAM_H - y; h2 = h - h1; }
    up_add(x, y, x + w1 - 1, y + h1 - 1);
    if (w2)       up_add(0, y, w2 - 1, y + h1 - 1);
    if (h2)       up_add(x, 0, x + w1 - 1, h2 - 1);
    if (w2 && h2) up_add(0, 0, w2 - 1, h2 - 1);
}

/* ---- coherency event ring (always-on, debug server "gl_coh_ring") -------- */
/* Every coherency-relevant operation — upload flushes, fills, copies, draw
 * bboxes, packs, full readbacks, presents, and probe perturbations — is
 * recorded with its rect and frame number. Per CLAUDE.md ring-buffer rule:
 * capture is continuous, observers query a window after the fact. Trigger
 * attribution convention: an op that flushes internally (fill/copy/draw/
 * present/peek) records its own event AFTER the FLUSH event it caused, so
 * the event following a FLUSH names the trigger. 16 B * 64 Ki = 1 MB. */
extern uint64_t s_frame_count;  /* defined in debug_server.c */

#define GL_COH_RING_CAP (1u << 16)
static GlCohEvent s_coh_ring[GL_COH_RING_CAP];
static uint64_t   s_coh_seq = 0;

/* 16x16 native-pixel tiles changed since their last on-screen present. This
 * lets a double-buffered 30 Hz game avoid swapping the unchanged front buffer
 * on the intervening 60 Hz vblank without guessing from game identity.
 *
 * INVARIANT: eliding a swap also elides whatever that swap was blocking on.
 * The frontend calls present once per guest vblank, so when the driver's swap
 * block owns the guest cadence (psx_present_vsync_owns_cadence(), the XOR
 * partner of the wall-clock pacer) an elided frame is an unthrottled frame:
 * a 30 Hz-presenting game then advances two vblanks per block and runs at 2x.
 * Every skip site below is therefore gated on that query. */
#define PRES_TILE 16
#define PRES_ROWS (VRAM_H / PRES_TILE)
static uint64_t s_present_dirty[PRES_ROWS];
static int s_last_present_path = -1;
static int s_last_dx, s_last_dy, s_last_dw, s_last_dh;
/* After savestate restore: keep swapping for a few presents even when the
 * display rect is byte-identical to the last swap. Double/triple-buffered
 * windows otherwise can leave a stale back buffer on screen while vblanks
 * (and FPS) keep advancing — especially on a 2nd+ load of the same slot. */
static int s_force_present_remaining = 0;

/* §33 rollback resim hold-last: sticky copy of the last Live present so
 * mid-resim ticks can Swap a frozen frame without reading mutating VRAM.
 * HOLD_DRAWABLE = full backbuffer copy taken just before SwapWindow.
 * HOLD_NATIVE   = GPU blit of a display band (used when interp owns Swap). */
#define HOLD_NONE     0
#define HOLD_DRAWABLE 1
#define HOLD_NATIVE   2
static int    s_hold_kind = HOLD_NONE;
static GLuint s_hold_tex = 0;
static GLuint s_hold_fbo = 0;
static int    s_hold_tw = 0, s_hold_th = 0; /* texture size in texels */
static int    s_hold_force_4_3 = 0;
static int    s_hold_linear = 0;

/* Post-load freeze probe (main.cpp): accumulate skip/swap/dirty marks. */
static uint64_t s_probe_skip = 0;
static uint64_t s_probe_swap = 0;
static uint64_t s_probe_dirty_marks = 0;

static void present_dirty_rect(int x0, int y0, int x1, int y1, int set) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1; if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    int tx0 = x0 / PRES_TILE, tx1 = x1 / PRES_TILE;
    uint64_t mask = (~0ull << tx0) & (~0ull >> (63 - tx1));
    for (int ty = y0 / PRES_TILE; ty <= y1 / PRES_TILE; ty++) {
        if (set) s_present_dirty[ty] |= mask; else s_present_dirty[ty] &= ~mask;
    }
    if (set) s_probe_dirty_marks++;
}

static int present_dirty_test(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1; if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return 0;
    int tx0 = x0 / PRES_TILE, tx1 = x1 / PRES_TILE;
    uint64_t mask = (~0ull << tx0) & (~0ull >> (63 - tx1));
    for (int ty = y0 / PRES_TILE; ty <= y1 / PRES_TILE; ty++)
        if (s_present_dirty[ty] & mask) return 1;
    return 0;
}

static void present_force_consumed(void) {
    if (s_force_present_remaining > 0)
        s_force_present_remaining--;
}

static void hold_ensure_tex(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!s_hold_tex) {
        glGenTextures(1, &s_hold_tex);
        if (!s_hold_fbo)
            p_glGenFramebuffers(1, &s_hold_fbo);
    }
    if (s_hold_tw == w && s_hold_th == h)
        return;
    glBindTexture(GL_TEXTURE_2D, s_hold_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hold_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, s_hold_tex, 0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    s_hold_tw = w;
    s_hold_th = h;
}

/* Snapshot the just-drawn default backbuffer (letterbox + content) before Swap.
 * Hold-last can then redraw this exact image without touching guest VRAM. */
static void hold_capture_drawable(void) {
    int ww = 0, wh = 0;
    if (!s_ctx || !s_win)
        return;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww < 1 || wh < 1)
        return;
    hold_ensure_tex(ww, wh);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, s_hold_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, ww, wh);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_hold_kind = HOLD_DRAWABLE;
    s_hold_force_4_3 = 0;
    s_hold_linear = 0;
}

/* Defined after present_target_quad (letterbox helpers). */
static void letterbox_rect_aspect(int ww, int wh, int num, int den,
                                  int *x, int *y, int *w, int *h);
static void letterbox_rect(int ww, int wh, int *x, int *y, int *w, int *h);

/* Size of a display-band capture (temporal-blend history, hold-last) whose
 * source band is sw x sh pixels at the output scale. Normally that size. In
 * the windowed high-resolution mode the band is up to 15372x4320 (8K at
 * 32:9), and copying it at full size every game frame (twice: blend history
 * and hold-last) cost more than the frame itself, only for the presenter to
 * draw it at the letterbox size. There the capture is taken at the letterbox
 * size instead (one filtered blit; the presenter then draws it 1:1), unless
 * the band is already smaller. force_4_3 picks the letterbox the present
 * uses. */
static void hiw_capture_size(int sw, int sh, int force_4_3, int *cw, int *ch) {
    int ww = 0, wh = 0, lx, ly, lw, lh;
    *cw = sw; *ch = sh;
    if (!s_hiw || !s_win) return;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww < 1 || wh < 1) return;
    if (force_4_3) letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    if (lw > 0 && lh > 0 && (int64_t)lw * lh < (int64_t)sw * sh) {
        *cw = lw; *ch = lh;
    }
}

/* Snapshot a native display band from an FBO when the main thread will not
 * Swap (interpolation owns the cadence). Scale-aware blit into hold tex (at
 * the presented size in the windowed high-resolution mode). */
static void hold_capture_native_fbo(GLuint src_fbo, int dx, int dy, int dw, int dh,
                                    int force_4_3, int linear) {
    int S = s_out_scale > 0 ? s_out_scale : 1, cw, ch;
    if (!s_ctx || !src_fbo || dw < 1 || dh < 1)
        return;
    hiw_capture_size(dw * S, dh * S, force_4_3, &cw, &ch);
    hold_ensure_tex(cw, ch);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, src_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_hold_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(dx * S, dy * S, (dx + dw) * S, (dy + dh) * S,
                        0, 0, cw, ch, GL_COLOR_BUFFER_BIT,
                        (cw == dw * S && ch == dh * S) || !linear ? GL_NEAREST : GL_LINEAR);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    s_hold_kind = HOLD_NATIVE;
    s_hold_force_4_3 = force_4_3 ? 1 : 0;
    s_hold_linear = linear ? 1 : 0;
}

static void hold_invalidate(void) {
    s_hold_kind = HOLD_NONE;
}

/* Defined after present_target_quad / letterbox helpers. */
static void present_target_quad(GLuint tex, float tex_w, float tex_h,
                                int x, int y, int w, int h, int linear,
                                int lx, int ly, int lw, int lh, int v_flip,
                                int apply_gamma, int src_scale);

static void coh_record(int kind, int x0, int y0, int x1, int y1) {
    GlCohEvent *e = &s_coh_ring[s_coh_seq % GL_COH_RING_CAP];
    e->frame = (uint32_t)s_frame_count;
    e->kind  = (uint8_t)kind;
    e->x0 = (int16_t)x0; e->y0 = (int16_t)y0;
    e->x1 = (int16_t)x1; e->y1 = (int16_t)y1;
    s_coh_seq++;
    if (kind == GL_COH_FLUSH || kind == GL_COH_FILL ||
        kind == GL_COH_COPY || kind == GL_COH_DRAW)
        present_dirty_rect(x0, y0, x1, y1, 1);
}

uint64_t gl_renderer_coh_total(void) {
    GL_RT_SYNC("coh_total"); return s_coh_seq; }
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out) {
    GL_RT_SYNC("coh_get");
    if (seq >= s_coh_seq) return 0;
    if (s_coh_seq - seq > GL_COH_RING_CAP) return 0;  /* evicted */
    *out = s_coh_ring[seq % GL_COH_RING_CAP];
    return 1;
}

/* ---- present ring (always-on, debug server "present_ring") --------------- */
/* Records every SwapWindow (vram/wide/cpu/blank/interpolated) and its
 * source + letterbox rects. PSX_GL_PRESENT_PROBE=1 additionally drains
 * glGetError and samples one backbuffer pixel before the swap; that synchronous
 * diagnostic is intentionally opt-in. Observers query a window after the fact. */
#define GL_PRES_RING_CAP 4096
static GlPresEvent s_pres_ring[GL_PRES_RING_CAP];
static uint64_t    s_pres_seq = 0;

static void post_aa_apply(int lx, int ly, int lw, int lh);
static void pres_record(int path, int dx, int dy, int w, int h,
                        int lx, int ly, int lw, int lh) {
    /* The composed game image, before hold-last capture and the OSD. */
    if (s_post_aa && (path == GL_PRES_VRAM || path == GL_PRES_WIDE || path == GL_PRES_INTERP))
        post_aa_apply(lx, ly, lw, lh);
    s_native_surface_pending=s_native_surface_enabled &&
        (path==GL_PRES_VRAM || path==GL_PRES_WIDE || path==GL_PRES_CPU || path==GL_PRES_BLANK);
    s_native_surface_rect[0]=lx;s_native_surface_rect[1]=ly;
    s_native_surface_rect[2]=lw;s_native_surface_rect[3]=lh;
    /* The ring metadata stays always-on, but pixel probing must not: each
     * glReadPixels synchronously drains queued GPU work. Two probes per frame
     * were enough to make Tomba 2 miss its frame budget. */
    static int probe_pixels = -1;
    if (probe_pixels < 0) {
        const char *cfg = getenv("PSX_GL_PRESENT_PROBE");
        probe_pixels = (cfg && cfg[0] == '1') ? 1 : 0;
    }
    GlPresEvent *e = &s_pres_ring[s_pres_seq % GL_PRES_RING_CAP];
    e->frame = (uint32_t)s_frame_count;
    e->t_ms  = (uint32_t)SDL_GetTicks();
    e->path  = (uint8_t)path;
    e->glerr = probe_pixels ? (uint16_t)glGetError() : 0;
    e->dx = (int16_t)dx; e->dy = (int16_t)dy;
    e->w  = (int16_t)w;  e->h  = (int16_t)h;
    e->lx = (int16_t)lx; e->ly = (int16_t)ly;
    e->lw = (int16_t)lw; e->lh = (int16_t)lh;
    /* Backbuffer sample at the letterbox centre (GL bottom-origin; the rects
     * we pass in are already bottom-origin GL window coords). */
    uint8_t px[3] = { 0, 0, 0 };
    if (probe_pixels && lw > 0 && lh > 0) {
        glReadBuffer(s_pt_on ? PSXGL_COLOR_ATTACHMENT0 : GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(lx + lw / 2, ly + lh / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    e->px_r = px[0]; e->px_g = px[1]; e->px_b = px[2];
    /* Blit-source sample: the hr FBO pixel at the display-rect centre. Splits
     * "FBO content was black" from "the blit malfunctioned". */
    uint8_t sp[3] = { 0, 0, 0 };
    e->src_valid = 0;
    if (probe_pixels && (path == GL_PRES_VRAM) && w > 0 && h > 0 && s_hr_fbo) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels((dx + w / 2) * s_hr_scale, (dy + h / 2) * s_hr_scale,
                     1, 1, GL_RGB, GL_UNSIGNED_BYTE, sp);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        e->src_valid = 1;
    }
    e->src_r = sp[0]; e->src_g = sp[1]; e->src_b = sp[2];
    s_pres_seq++;
}

uint64_t gl_renderer_pres_total(void) {
    GL_RT_SYNC("pres_total"); return s_pres_seq; }
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out) {
    GL_RT_SYNC("pres_get");
    if (seq >= s_pres_seq) return 0;
    if (s_pres_seq - seq > GL_PRES_RING_CAP) return 0;  /* evicted */
    *out = s_pres_ring[seq % GL_PRES_RING_CAP];
    return 1;
}

/* ---- shaders ------------------------------------------------------------ */
static const char *PRESENT_VS =
    "#version 330\n"
    "out vec2 v_uv;\n"
    "uniform vec4 u_uv_rect;\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  v_uv = vec2(mix(u_uv_rect.x,u_uv_rect.z,p.x),\n"
    "              mix(u_uv_rect.y,u_uv_rect.w,1.0-p.y));\n"
    "  gl_Position = vec4(p*2.0-1.0,0.0,1.0); }\n";
/* ---- Post-process anti-aliasing ([video] antialiasing_mode) ----------------
 * Opt-in, off by default: with the mode off nothing below runs and the present
 * is byte-identical to a build without it. When on, the composed game image
 * (the letterbox rect pres_record is told about, after the game quad and
 * before hold-last capture, OSD, screenshots and the swap) is copied out of
 * the drawable and redrawn through an edge filter. It works on the output
 * pixels only: guest VRAM, the hr/wide surfaces, readbacks, render passes and
 * the Smooth motion / frame generation sources are never touched, so it
 * composes with every present path (4:3, native-wide, interpolated and
 * generated frames) and with internal resolution / dynamic resolution, whose
 * area resolve has already happened by then. OSD and bezel stay sharp.
 *
 * FXAA here is an independent implementation of the published technique
 * (luma contrast test, edge orientation, bounded end-of-edge search,
 * sub-pixel blend); it is one copy plus one full-screen pass. */
static GLuint build_program(const char *vs, const char *fs);
static GLuint s_paa_prog = 0, s_paa_tex = 0;
static int    s_paa_tw = 0, s_paa_th = 0, s_paa_failed = 0;
static GLint  s_paa_uTex = -1, s_paa_uRcp = -1, s_paa_uUvRect = -1, s_paa_uQuality = -1;
static uint64_t s_paa_passes = 0;
#ifndef PSX_NO_DEBUG_TOOLS
/* Cost of the pass (debug server post_aa), only with PSX_POST_AA_TIME=N: the
 * pass runs N times (1..64; N > 1 re-filters the frame and amortizes the
 * fence) bracketed by glFinish and timed on the host clock. That serializes
 * the GPU, so it is a measurement mode, never on by default. (Timestamp
 * queries read 0 on Apple's GL.) */
static int    s_paa_time = -1;
static double s_paa_gpu_us_sum = 0.0;
static uint64_t s_paa_gpu_n = 0, s_paa_t0 = 0;
#endif

static const char *POST_AA_FS =
    "#version 330\n"
    "in vec2 v_uv; out vec4 o;\n"
    "uniform sampler2D u_tex; uniform vec2 u_rcp; uniform int u_quality;\n"
    "float L(vec3 c){ return dot(c, vec3(0.299,0.587,0.114)); }\n"
    "float LS(vec2 p){ return L(textureLod(u_tex,p,0.0).rgb); }\n"
    "void main(){\n"
    "  vec3 cM = textureLod(u_tex, v_uv, 0.0).rgb; float lM = L(cM);\n"
    "  float lN = LS(v_uv+vec2(0.0, u_rcp.y)), lS = LS(v_uv-vec2(0.0, u_rcp.y));\n"
    "  float lE = LS(v_uv+vec2(u_rcp.x,0.0)), lW = LS(v_uv-vec2(u_rcp.x,0.0));\n"
    "  float mx = max(lM,max(max(lN,lS),max(lE,lW)));\n"
    "  float mn = min(lM,min(min(lN,lS),min(lE,lW)));\n"
    "  float range = mx - mn;\n"
    /* thresholds: absolute 1/16 (1/24 at high quality), relative 1/8 */
    "  float thr = u_quality > 0 ? 0.0417 : 0.0625;\n"
    "  if (range < max(thr, mx*0.125)) { o = vec4(cM,1.0); return; }\n"
    "  float lNE = LS(v_uv+u_rcp), lSW = LS(v_uv-u_rcp);\n"
    "  float lNW = LS(v_uv+vec2(-u_rcp.x,u_rcp.y)), lSE = LS(v_uv+vec2(u_rcp.x,-u_rcp.y));\n"
    "  float eH = abs(lNW+lNE-2.0*lN) + 2.0*abs(lW+lE-2.0*lM) + abs(lSW+lSE-2.0*lS);\n"
    "  float eV = abs(lNW+lSW-2.0*lW) + 2.0*abs(lN+lS-2.0*lM) + abs(lNE+lSE-2.0*lE);\n"
    "  bool horz = eH >= eV;\n"
    "  float l1 = horz ? lS : lW, l2 = horz ? lN : lE;\n"
    "  float g1 = abs(l1-lM), g2 = abs(l2-lM);\n"
    "  bool neg = g1 >= g2;\n"
    "  float grad = 0.25*max(g1,g2);\n"
    "  float step = horz ? u_rcp.y : u_rcp.x;\n"
    "  float lLocal; if (neg) { step = -step; lLocal = 0.5*(l1+lM); } else lLocal = 0.5*(l2+lM);\n"
    "  vec2 p = v_uv; if (horz) p.y += 0.5*step; else p.x += 0.5*step;\n"
    "  vec2 off = horz ? vec2(u_rcp.x,0.0) : vec2(0.0,u_rcp.y);\n"
    "  vec2 p1 = p - off, p2 = p + off;\n"
    "  float e1 = LS(p1)-lLocal, e2 = LS(p2)-lLocal;\n"
    "  bool d1 = abs(e1) >= grad, d2 = abs(e2) >= grad;\n"
    "  int steps = u_quality > 0 ? 12 : 8;\n"
    "  for (int i = 1; i < steps && !(d1 && d2); i++) {\n"
    "    float k = i < 2 ? 1.0 : (i < 5 ? 1.5 : (i < 8 ? 2.0 : 4.0));\n"
    "    if (!d1) { p1 -= off*k; e1 = LS(p1)-lLocal; d1 = abs(e1) >= grad; }\n"
    "    if (!d2) { p2 += off*k; e2 = LS(p2)-lLocal; d2 = abs(e2) >= grad; }\n"
    "  }\n"
    "  float dist1 = horz ? v_uv.x-p1.x : v_uv.y-p1.y;\n"
    "  float dist2 = horz ? p2.x-v_uv.x : p2.y-v_uv.y;\n"
    "  bool near1 = dist1 < dist2; float dmin = min(dist1,dist2);\n"
    "  float len = dist1 + dist2;\n"
    "  bool mLess = (lM - lLocal) < 0.0;\n"
    "  bool good = ((near1 ? e1 : e2) < 0.0) != mLess;\n"
    "  float eo = good ? (0.5 - dmin/len) : 0.0;\n"
    "  float avg = (2.0*(lN+lS+lE+lW) + lNE+lNW+lSE+lSW) / 12.0;\n"
    "  float sp = clamp(abs(avg-lM)/range, 0.0, 1.0);\n"
    "  sp = (-2.0*sp + 3.0)*sp*sp; sp = sp*sp*0.75;\n"
    "  float f = max(eo, sp);\n"
    "  vec2 q = v_uv; if (horz) q.y += f*step; else q.x += f*step;\n"
    "  o = vec4(textureLod(u_tex, q, 0.0).rgb, 1.0);\n"
    "}\n";

int gl_renderer_set_post_aa(int mode) {
    if (mode < GL_POST_AA_OFF || mode > GL_POST_AA_FXAA_HQ) mode = GL_POST_AA_OFF;
    /* Live debug/launcher changes use the same context-ownership handoff as
     * the other renderer settings, never racing an in-flight present. */
    GL_RT_SYNC("post_aa");
    s_post_aa = mode;
    return 1;
}
int gl_renderer_post_aa(void) {
    GL_RT_SYNC("post_aa");
    return s_post_aa;
}
uint64_t gl_renderer_post_aa_passes(void) {
    GL_RT_SYNC("post_aa_stats");
    return s_paa_passes;
}

static void post_aa_release(void) {
    s_paa_prog = 0; s_paa_tex = 0; s_paa_tw = s_paa_th = 0; s_paa_failed = 0;
}

/* The composed image in [lx,ly,lw,lh] of the bound window target, in place. */
static void post_aa_apply(int lx, int ly, int lw, int lh) {
    if (!s_post_aa || !s_ctx || s_paa_failed) return;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (lx < 0) { lw += lx; lx = 0; }
    if (ly < 0) { lh += ly; ly = 0; }
    if (lx + lw > ww) lw = ww - lx;
    if (ly + lh > wh) lh = wh - ly;
    if (lw < 3 || lh < 3) return;
    if (!s_paa_prog) {
        s_paa_prog = build_program(PRESENT_VS, POST_AA_FS);
        if (!s_paa_prog) { s_paa_failed = 1; return; }
        s_paa_uTex = p_glGetUniformLocation(s_paa_prog, "u_tex");
        s_paa_uRcp = p_glGetUniformLocation(s_paa_prog, "u_rcp");
        s_paa_uUvRect = p_glGetUniformLocation(s_paa_prog, "u_uv_rect");
        s_paa_uQuality = p_glGetUniformLocation(s_paa_prog, "u_quality");
    }
    p_glActiveTexture(PSXGL_TEXTURE0);
    if (!s_paa_tex || s_paa_tw != lw || s_paa_th != lh) {
        if (!s_paa_tex) glGenTextures(1, &s_paa_tex);
        glBindTexture(GL_TEXTURE_2D, s_paa_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        s_paa_tw = lw; s_paa_th = lh;
    } else {
        glBindTexture(GL_TEXTURE_2D, s_paa_tex);
    }
#ifndef PSX_NO_DEBUG_TOOLS
    if (s_paa_time < 0) {
        const char *e = getenv("PSX_POST_AA_TIME");
        s_paa_time = e ? atoi(e) : 0;
        if (s_paa_time < 0) s_paa_time = 0;
        if (s_paa_time > 64) s_paa_time = 64;
    }
    if (s_paa_time) { glFinish(); s_paa_t0 = SDL_GetPerformanceCounter(); }
    const int reps = s_paa_time > 1 ? s_paa_time : 1;
#else
    const int reps = 1;
#endif
    for (int r = 0; r < reps; r++) {
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lx, ly, lw, lh);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(lx, ly, lw, lh);
    p_glUseProgram(s_paa_prog);
    p_glUniform1i(s_paa_uTex, 0);
    p_glUniform2f(s_paa_uRcp, 1.0f / (float)lw, 1.0f / (float)lh);
    p_glUniform1i(s_paa_uQuality, s_post_aa == GL_POST_AA_FXAA_HQ ? 1 : 0);
    /* PRESENT_VS flips v; (0,1,1,0) cancels it for a bottom-up copy. */
    p_glUniform4f(s_paa_uUvRect, 0.0f, 1.0f, 1.0f, 0.0f);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
#ifndef PSX_NO_DEBUG_TOOLS
    if (s_paa_time) {
        glFinish();
        s_paa_gpu_us_sum += (double)(SDL_GetPerformanceCounter() - s_paa_t0) * 1e6 /
                            (double)SDL_GetPerformanceFrequency() / (double)reps;
        s_paa_gpu_n++;
    }
#endif
    s_paa_passes++;
}

/* Mean microseconds per pass since the last call (debug builds with
 * PSX_POST_AA_TIME=N; 0 otherwise). Resets the mean. */
double gl_renderer_post_aa_gpu_us(void) {
#ifndef PSX_NO_DEBUG_TOOLS
    GL_RT_SYNC("post_aa_stats");
    double m = s_paa_gpu_n ? s_paa_gpu_us_sum / (double)s_paa_gpu_n : 0.0;
    s_paa_gpu_us_sum = 0.0; s_paa_gpu_n = 0;
    return m;
#else
    return 0.0;
#endif
}

/* Present sampling. u_sharp==0 is the historical behaviour: sample straight at
 * v_uv, so the texture's own filter (NEAREST or LINEAR) decides everything.
 *
 * u_sharp==1 selects SHARP-BILINEAR, for upscaling a low-res source (FMV) to a
 * much larger window. Plain GL_LINEAR blends across the whole texel and turns a
 * 320x192 movie to mush; plain GL_NEAREST keeps it crisp but blocky and makes
 * the non-integer scale factor beat (some source pixels land 3 window pixels
 * wide, their neighbours 4). Sharp-bilinear keeps each texel flat across its
 * interior and confines the linear ramp to a ONE-OUTPUT-PIXEL-wide band at the
 * texel boundary: crisp like nearest, but without the uneven pixel widths.
 *
 * u_sharp_scale is output pixels per texel. At <=1 (downscale) the band covers
 * the whole texel and this degrades to plain bilinear, which is what you want
 * there. The result is clamped to u_uv_rect so the half-texel edge inset the
 * caller applied still holds — that inset is what keeps LINEAR from bleeding
 * the border texel into the image (the old reason FMV was pinned to NEAREST). */
/* Scanline post-process, shared by the present and interpolation shaders.
 * Darkens toward the gap between PS1 scanlines with a soft sinusoidal beam
 * centred on each line. The pitch is u_scanline_lines (the number of native
 * display lines across the source rect), so it tracks the PS1 line grid, not
 * the internal SSAA scale or the window size. u_scanline_scale is output pixels
 * per PS1 line; the gate fades the effect in from 1x to 2x so it never shimmers
 * on a window too small to resolve one line as two rows (Nyquist). Computed
 * against v_uv.y (position across the display region) so it is orientation- and
 * scale-agnostic. */
#define PSX_SCANLINE_UNIFORMS \
    "uniform int   u_scanline;\n" \
    "uniform float u_scanline_strength;\n" \
    "uniform float u_scanline_lines;\n" \
    "uniform float u_scanline_scale;\n"
#define PSX_SCANLINE_FUNC \
    "vec3 psx_scanline(vec3 rgb, float vy){\n" \
    "  if (u_scanline == 0) return rgb;\n" \
    "  float gate = clamp(u_scanline_scale - 1.0, 0.0, 1.0);\n" \
    "  if (gate <= 0.0) return rgb;\n" \
    "  float p    = fract(vy * u_scanline_lines);\n" \
    "  float beam = sin(3.14159265 * p);\n" \
    "  float mult = 1.0 - (u_scanline_strength * gate) * (1.0 - beam);\n" \
    "  return rgb * mult;\n" \
    "}\n"
static const char *PRESENT_FS =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D u_tex; out vec4 frag;\n"
    "uniform vec4 u_uv_rect;\n"
    "uniform vec2 u_tex_size;\n"
    "uniform vec2 u_sharp_scale;\n"
    "uniform int  u_sharp;\n"
    "uniform float u_gamma;\n"
    "uniform int  u_chroma;   /* [video] fmv_chroma_smoothing (24-bit frames) */\n"
    PSX_SCANLINE_UNIFORMS
    /* Catmull-Rom bicubic via 9 bilinear taps. Sharper than plain bilinear at
     * the same smoothness, with mild overshoot that reads as edge definition.
     * The present texture holds exactly the source rect and wraps CLAMP_TO_EDGE,
     * so the +/-2 texel footprint clamps to real edge pixels, never garbage. */
    "vec4 bicubic(vec2 uv){\n"
    "  vec2 sp = uv * u_tex_size;\n"
    "  vec2 t1 = floor(sp - 0.5) + 0.5;\n"
    "  vec2 f  = sp - t1;\n"
    "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "  vec2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "  vec2 w12 = w1 + w2;\n"
    "  vec2 t0 = (t1 - 1.0) / u_tex_size;\n"
    "  vec2 t3 = (t1 + 2.0) / u_tex_size;\n"
    "  vec2 t12 = (t1 + w2 / w12) / u_tex_size;\n"
    "  vec4 r = vec4(0.0);\n"
    "  r += texture(u_tex, vec2(t0.x , t0.y )) * (w0.x  * w0.y );\n"
    "  r += texture(u_tex, vec2(t12.x, t0.y )) * (w12.x * w0.y );\n"
    "  r += texture(u_tex, vec2(t3.x , t0.y )) * (w3.x  * w0.y );\n"
    "  r += texture(u_tex, vec2(t0.x , t12.y)) * (w0.x  * w12.y);\n"
    "  r += texture(u_tex, vec2(t12.x, t12.y)) * (w12.x * w12.y);\n"
    "  r += texture(u_tex, vec2(t3.x , t12.y)) * (w3.x  * w12.y);\n"
    "  r += texture(u_tex, vec2(t0.x , t3.y )) * (w0.x  * w3.y );\n"
    "  r += texture(u_tex, vec2(t12.x, t3.y )) * (w12.x * w3.y );\n"
    "  r += texture(u_tex, vec2(t3.x , t3.y )) * (w3.x  * w3.y );\n"
    "  return r;\n"
    "}\n"
    PSX_SCANLINE_FUNC
    /* Area resolve (u_sharp == 3) for a supersampled source larger than the
     * output: average the whole footprint of the output pixel instead of one
     * tap, which skips source texels (aliasing) once the ratio passes ~1.25.
     * u_sharp_scale is output px per texel, so 1/u_sharp_scale is the
     * footprint in texels. Each tap is bilinear and taps are spaced up to two
     * texels apart, so every tap averages a 2x2 texel block: an exact box at
     * even ratios, a close tent otherwise. Taps clamp to u_uv_rect so the
     * neighbouring VRAM never bleeds in. */
    "vec4 area_resolve(vec2 uv){\n"
    "  vec2 fp = 1.0 / max(u_sharp_scale, vec2(1.0e-6));\n"
    "  vec2 nf = clamp(ceil(fp * 0.5), vec2(1.0), vec2(8.0));\n"
    "  ivec2 n = ivec2(nf);\n"
    "  vec2 st = fp / nf;\n"
    "  vec2 base = uv * u_tex_size - 0.5 * fp + 0.5 * st;\n"
    "  vec2 lo = min(u_uv_rect.xy, u_uv_rect.zw);\n"
    "  vec2 hi = max(u_uv_rect.xy, u_uv_rect.zw);\n"
    "  vec4 acc = vec4(0.0);\n"
    "  for (int j = 0; j < n.y; j++)\n"
    "    for (int i = 0; i < n.x; i++)\n"
    "      acc += texture(u_tex, clamp((base + vec2(i, j) * st) / u_tex_size, lo, hi));\n"
    "  return acc / float(n.x * n.y);\n"
    "}\n"
    "void main(){\n"
    "  vec2 uv = v_uv;\n"
    "  vec4 c;\n"
    "  if (u_sharp == 2) { c = bicubic(uv); }\n"
    "  else if (u_sharp == 3) { c = area_resolve(uv); }\n"
    "  else {\n"
    "    if (u_sharp == 1) {\n"
    "      vec2 scale = max(u_sharp_scale, vec2(1.0));\n"
    "      vec2 texel = uv * u_tex_size;\n"
    "      vec2 tf    = floor(texel);\n"
    "      vec2 cd    = (texel - tf) - 0.5;\n"
    "      vec2 band  = 0.5 - 0.5 / scale;\n"
    "      vec2 f     = (cd - clamp(cd, -band, band)) * scale + 0.5;\n"
    "      uv = (tf + f) / u_tex_size;\n"
    "      vec2 lo = min(u_uv_rect.xy, u_uv_rect.zw);\n"
    "      vec2 hi = max(u_uv_rect.xy, u_uv_rect.zw);\n"
    "      uv = clamp(uv, lo, hi);\n"
    "    }\n"
    "    c = texture(u_tex, uv);\n"
    "  }\n"
    /* MDEC decodes 4:2:0 video: one Cb/Cr pair per 2x2 pixels, upsampled by
     * replication, so saturated edges in a 24-bit movie frame show 2-pixel
     * colour blocks around sharp luma. Chroma smoothing keeps the luma of the
     * reconstructed sample and takes its chroma from a [1 2 1] tent over the
     * 3x3 source pixels around it (BT.601), which spreads each block's colour
     * into its neighbours without softening the luma detail. */
    "  if (u_chroma != 0) {\n"
    "    vec2 ts = 1.0 / u_tex_size;\n"
    "    vec2 lo = min(u_uv_rect.xy, u_uv_rect.zw), hi = max(u_uv_rect.xy, u_uv_rect.zw);\n"
    "    vec2 base = (floor(v_uv * u_tex_size) + 0.5) * ts;\n"
    "    vec2 cc = vec2(0.0);\n"
    "    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {\n"
    "      vec3 s = texture(u_tex, clamp(base + vec2(i, j) * ts, lo, hi)).rgb;\n"
    "      float w = float((2 - abs(i)) * (2 - abs(j)));\n"
    "      cc += w * vec2(dot(s, vec3(-0.168736, -0.331264, 0.5)), dot(s, vec3(0.5, -0.418688, -0.081312)));\n"
    "    }\n"
    "    cc /= 16.0;\n"
    "    float y = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    c.rgb = clamp(vec3(y + 1.402 * cc.y, y - 0.344136 * cc.x - 0.714136 * cc.y, y + 1.772 * cc.x), 0.0, 1.0);\n"
    "  }\n"
    "  c.rgb = psx_scanline(c.rgb, v_uv.y);\n"
    "  if (u_gamma > 0.0 && u_gamma != 1.0) c.rgb = pow(max(c.rgb, vec3(0.0)), vec3(1.0 / u_gamma));\n"
    "  frag = c;\n"
    "}\n";
static const char *INTERP_FS =
    "#version 330\n"
    "in vec2 v_uv; uniform sampler2D u_prev; uniform sampler2D u_curr;\n"
    "uniform float u_alpha; uniform int u_blend_mode; uniform float u_gamma; out vec4 frag;\n"
    PSX_SCANLINE_UNIFORMS
    PSX_SCANLINE_FUNC
    "void main(){\n"
    "  vec4 prev=texture(u_prev,v_uv), curr=texture(u_curr,v_uv);\n"
    "  float alpha=u_alpha;\n"
    "  if(u_blend_mode==1){\n"
    "    vec3 d=abs(prev.rgb-curr.rgb);\n"
    "    float change=max(max(d.r,d.g),d.b);\n"
    "    float safe_blend=1.0-smoothstep(0.08,0.20,change);\n"
    "    alpha=mix(step(0.5,u_alpha),u_alpha,safe_blend);\n"
    "  }\n"
    "  vec4 c=mix(prev,curr,alpha);\n"
    "  c.rgb=psx_scanline(c.rgb, v_uv.y);\n"
    "  if (u_gamma > 0.0 && u_gamma != 1.0) c.rgb=pow(max(c.rgb, vec3(0.0)), vec3(1.0/u_gamma));\n"
    "  frag=c;\n"
    "}\n";

/* Geometry: position in VRAM pixels (draw offset already applied by gpu.c),
 * color rgb in 0..1, color a = mask bit (0/1). The clip transform is in
 * native VRAM space; the viewport at S* the size scales rasterization.
 *
 * ALL drawn prims shift positions by u_shift = half an HR pixel (0.5/S in
 * native units): GL samples coverage/attributes at pixel CENTERS, the PS1
 * DDA at INTEGER coords. The shift aligns GL's sample grid with the PS1
 * grid — without it, any texture mapping with slope != 1 (scaled sprites,
 * squished menu fonts) samples one texel off per row/column (striped
 * glyphs, seam lines). Half an HR pixel (not half a native pixel!) keeps
 * rect coverage exactly [x*S, (x+w)*S) at every scale AND makes the
 * top-left subpixel of each S*S block sample the exact PS1 value (which is
 * what the PACK pass reads back). */
/* PS1 dithering ([video] dithering). The GPU adds the 4x4 offset matrix below
 * to a shaded or texture-modulated pixel before truncating it to 5 bits, when
 * GP0(E1h) bit 9 is set. Flat and raw-texture pixels are already 5-bit exact
 * and are left alone, which is what the hardware's shading/modulation rule
 * amounts to. u_dither 0 = off (true colour, the default and the historical
 * renderer), 1 = pattern per internal-resolution pixel, 2 = pattern scaled
 * to the native grid (v_npos is the fragment's native VRAM position). */
#define PSX_DITHER_GLSL \
    "uniform int u_dither;\n" \
    "vec3 psx_dither(vec3 c, vec2 npos){\n" \
    "  if (u_dither == 0) return c;\n" \
    "  vec3 q8 = c * (255.0 / 8.0), q31 = c * 31.0;\n" \
    "  if (all(lessThan(abs(q8 - floor(q8 + 0.5)), vec3(0.02))) ||\n" \
    "      all(lessThan(abs(q31 - floor(q31 + 0.5)), vec3(0.02)))) return c;\n" \
    "  ivec2 p = (u_dither == 2 ? ivec2(floor(npos)) : ivec2(gl_FragCoord.xy)) & 3;\n" \
    "  const float m[16] = float[16](-4.0,0.0,-3.0,1.0, 2.0,-2.0,3.0,-1.0,\n" \
    "                                -3.0,1.0,-4.0,0.0, 3.0,-1.0,2.0,-2.0);\n" \
    "  vec3 k = clamp(floor((c * 255.0 + m[p.y * 4 + p.x]) / 8.0), 0.0, 31.0);\n" \
    "  return k / 31.0;\n" \
    "}\n"
static const char *GEO_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "uniform float u_shift;\n"
    "uniform float u_xoff;   /* native-wide x translation (px); 0 canonical */\n"
    "uniform float u_xhalf;  /* x clip half-extent (px); 512 canonical */\n"
    "uniform float u_xscale; /* native-wide 2D-backdrop x-stretch; 1 canonical */\n"
    "uniform float u_xcenter;/* stretch centre in VRAM px; 0 canonical */\n"
    "uniform float u_zbias;  /* PGXP depth: relative near bias of the colour pass */\n"
    "noperspective out vec4 v_col;\n"
    "smooth out vec3 v_col_p;   /* perspective-correct colour (PGXP, G1.14) */\n"
    "flat out int v_cp;\n"
    "noperspective out vec2 v_npos;  /* native VRAM position (dithering) */\n"
    "void main(){\n"
    "  v_npos = a_pos;\n"
    "  /* a_col.a carries the mask bit (0/1) and, for a PGXP 3D vertex, its\n"
    "   * GTE SZ: a = mask + 2*(sz + 65536*cp), cp = perspective colour. A\n"
    "   * negative a is a depth-clear vertex (beyond every SZ, inside the far plane). */\n"
    "  float a = a_col.a, code = floor(a * 0.5), zn = 0.0, w = 1.0;\n"
    "  float m = a - 2.0 * code;\n"
    "  float cp = code >= 65536.0 ? 1.0 : 0.0, sz = code - 65536.0 * cp;\n"
    "  if (a < 0.0) { m = 0.0; zn = 0.9999; sz = 0.0; cp = 0.0; }  /* inside the far plane: never clipped */\n"
    "  else if (sz > 0.5) { zn = 1.0 - 512.0 / (max(sz * (1.0 - u_zbias) - (u_zbias > 0.0 ? 48.0 : 0.0), 1.0) + 256.0); if (cp > 0.5) w = sz / 1024.0; }\n"
    "  v_col = vec4(a_col.rgb, m); v_col_p = a_col.rgb; v_cp = int(cp);\n"
    "  float xb = a_pos.x;\n"
    "  if (u_xscale < 0.0) {\n"
    "    float s = -u_xscale; float h = u_xhalf / s;\n"
    "    float l = u_xcenter - h, r = u_xcenter + h;\n"
    "    if (xb < l) xb = l + (xb-l)*s; else if (xb > r) xb = r + (xb-r)*s;\n"
    "  } else xb = (xb - u_xcenter)*u_xscale + u_xcenter;\n"
    "  gl_Position = vec4(((xb+u_shift+u_xoff)/u_xhalf - 1.0) * w, ((a_pos.y+u_shift)/256.0 - 1.0) * w, zn * w, w); }\n";
static const char *GEO_FS =
    "#version 330\n"
    "noperspective in vec4 v_col; smooth in vec3 v_col_p; flat in int v_cp; out vec4 frag;\n"
    "noperspective in vec2 v_npos;\n"
    PSX_DITHER_GLSL
    "void main(){ frag = v_cp != 0 ? vec4(v_col_p, v_col.a) : v_col;\n"
    "  frag.rgb = psx_dither(frag.rgb, v_npos); }\n";

/* Textured prims: sample raw 1555 VRAM (integer), CLUT decode per depth,
 * texture window, optional bilinear, texel-0 discard, STP-split discard,
 * PS1 *2-around-0x80 modulation. Output alpha = bit15 of the written pixel.
 * Texel coords use floor() to match the software rasterizer's truncation
 * (rounding shifted sampling +1 texel half the time: smeared text). */
/* Textured program. Per-prim texture state (texpage, clut, depth, raw, uv
 * limits) is carried in FLAT vertex attributes — constant across a prim's
 * vertices — instead of uniforms, so consecutive textured prims with the same
 * blend/mask state batch into one draw (see flush_tex_batch). The texture
 * window rides in the vertex too (a_twin), so a GP0(E2h) change need not end
 * a batch. The remaining uniforms (u_maskset/u_filter/u_semipass) are the
 * batch keys + per-pass state. */
static const char *TEX_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec2 a_tpage;\n"
    "layout(location=4) in vec2 a_clut;\n"
    "layout(location=5) in float a_depth;\n"
    "layout(location=6) in float a_raw;\n"
    "layout(location=7) in vec4 a_limits;\n"
    "layout(location=8) in float a_semi;\n"
    "layout(location=9) in float a_q;   /* persp weight; 0 = affine (default) */\n"
    "layout(location=10) in float a_twin; /* GP0(E2h) bits 0..19 */\n"
    "layout(location=11) in vec4 a_hd_source; /* page origin + native extent */\n"
    "layout(location=12) in float a_hd_mode;\n"
    "uniform float u_shift;\n"
    "uniform float u_xoff;   /* native-wide x translation (px); 0 canonical */\n"
    "uniform float u_xhalf;  /* x clip half-extent (px); 512 canonical */\n"
    "uniform float u_xscale; /* native-wide 2D-backdrop x-stretch; 1 canonical */\n"
    "uniform float u_xcenter;/* stretch centre in VRAM px; 0 canonical */\n"
    "uniform float u_zbias;  /* PGXP depth: relative near bias of the colour pass */\n"
    "noperspective out vec2 v_uv; noperspective out vec4 v_col;\n"
    "smooth out vec2 v_uv_p;  /* perspective-correct UV (used when v_persp!=0) */\n"
    "flat out int v_persp;\n"
    "flat out ivec2 v_tpage; flat out ivec2 v_clut; flat out int v_depth;\n"
    "flat out int v_raw; flat out ivec4 v_limits; flat out int v_semi;\n"
    "flat out int v_twin;\n"
    "flat out vec4 v_hd_source; flat out int v_hd_mode;\n"
    "smooth out vec3 v_col_p; flat out int v_cp;\n"
    "noperspective out vec2 v_npos;  /* native VRAM position (dithering) */\n"
    "void main(){ v_uv = a_uv; v_uv_p = a_uv; v_col = a_col; v_col_p = a_col.rgb;\n"
    "  v_npos = a_pos;\n"
    "  v_persp = (a_q > 0.0) ? 1 : 0;\n"
    "  v_tpage = ivec2(a_tpage + 0.5); v_clut = ivec2(a_clut + 0.5);\n"
    "  v_depth = int(a_depth + 0.5); v_raw = int(a_raw + 0.5);\n"
    "  v_semi = int(a_semi + 0.5);\n"
    "  v_twin = int(a_twin + 0.5);\n"
    "  v_hd_source=a_hd_source; v_hd_mode=int(a_hd_mode+0.5);\n"
    "  v_limits = ivec4(floor(a_limits + 0.5));\n"
    "  /* u_shift: align GL's center-sample grid with the PS1 integer grid (see\n"
    "   * GEO_VS) so interpolated uv at a fragment equals the PS1 DDA value. */\n"
    "  float xb = a_pos.x;\n"
    "  if (u_xscale < 0.0) {\n"
    "    float s = -u_xscale; float h = u_xhalf / s;\n"
    "    float l = u_xcenter - h, r = u_xcenter + h;\n"
    "    if (xb < l) xb = l + (xb-l)*s; else if (xb > r) xb = r + (xb-r)*s;\n"
    "  } else xb = (xb - u_xcenter)*u_xscale + u_xcenter;\n"
    "  /* Perspective-correct UV rides the standard w divide: emit clip coords\n"
    "   * pre-multiplied by w = 1/q so the post-divide NDC is unchanged while\n"
    "   * the rasterizer interpolates the smooth varying hyperbolically. With\n"
    "   * a_q == 0 (feature off) w is exactly 1.0 and this is the old expression. */\n"
    "  float w = (a_q > 0.0) ? (1.0 / a_q) : 1.0;\n"
    "  /* PGXP (G1.14): a_col.a is 1.0 (unused) unless a PGXP 3D vertex\n"
    "   * stores -(sz + 65536*cp) there (cp: perspective-correct colour), so\n"
    "   * the vertex stays TEXV floats for every title. */\n"
    "  float pz = a_col.a < 0.0 ? -a_col.a : 0.0;\n"
    "  float cp = pz >= 65536.0 ? 1.0 : 0.0, sz = pz - 65536.0 * cp, zn = 0.0;\n"
    "  if (sz > 0.5) { zn = 1.0 - 512.0 / (max(sz * (1.0 - u_zbias) - (u_zbias > 0.0 ? 48.0 : 0.0), 1.0) + 256.0); if (a_q <= 0.0 && cp > 0.5) w = sz / 1024.0; }\n"
    "  v_cp = int(cp);\n"
    "  vec2 ndc = vec2((xb+u_shift+u_xoff)/u_xhalf - 1.0, (a_pos.y+u_shift)/256.0 - 1.0);\n"
    "  gl_Position = vec4(ndc * w, zn * w, w); }\n";
static const char *TEX_FS =
    "#version 330\n"
    "noperspective in vec2 v_uv; noperspective in vec4 v_col;\n"
    "smooth in vec2 v_uv_p; flat in int v_persp;\n"
    "smooth in vec3 v_col_p; flat in int v_cp;\n"
    "noperspective in vec2 v_npos;\n"
    PSX_DITHER_GLSL
    "out vec4 frag; out vec4 blend_factor;\n"
    "flat in ivec2 v_tpage;   /* texture page base, VRAM px */\n"
    "flat in ivec2 v_clut;    /* CLUT base, VRAM px */\n"
    "flat in int v_depth;     /* 0=4bit 1=8bit 2=15bit */\n"
    "flat in int v_raw;       /* 1 = no color modulation */\n"
    "flat in ivec4 v_limits;  /* prim uv sampling bounds (inclusive, post-wrap) */\n"
    "flat in int v_semi;      /* GP0 command has semi-transparency enabled */\n"
    "flat in int v_twin;      /* texture window, GP0(E2h) bits 0..19 */\n"
    "flat in vec4 v_hd_source; flat in int v_hd_mode;\n"
    "uniform sampler2D u_hd_texture;\n"
    "uniform usampler2D u_vram;\n"
    "uniform usampler2D u_palette;\n"
    "uniform int u_semipass;  /* 0=all texels, 1=STP=0 only, 2=STP=1 only */\n"
    "uniform int u_semimode;  /* PS1 blend mode; drives dual-source factors */\n"
    "uniform int u_maskset;   /* GP0(E6h) set-mask: OR bit15 into output */\n"
    "uniform int u_filter;    /* bits 0-3: 0 nearest, 1 bilinear, 2 stable; bit 4: LOD */\n"
    "uniform int u_aniso;     /* [video] anisotropic_filtering, 1..16 */\n"
    "uniform float u_shift;\n"
    "int vram_at(int x, int y){\n"
    "  ivec2 p = ivec2(x & 1023, y & 511);\n"
    "  if (any(greaterThanEqual(p, textureSize(u_vram, 0)))) return 0;\n"
    "  return int(texelFetch(u_vram, p, 0).r);\n"
    "}\n"
    "int palette_at(int x, int y){\n"
    "  ivec2 p = ivec2(x & 1023, y & 511);\n"
    "  if (any(greaterThanEqual(p, textureSize(u_palette, 0)))) return 0;\n"
    "  return int(texelFetch(u_palette, p, 0).r);\n"
    "}\n"
    "int fetch_texel(int u, int v){\n"
    "  u &= 255; v &= 255;\n"
    "  /* texture window: mask_x, mask_y, off_x, off_y (8-px units) */\n"
    "  ivec4 tw = ivec4(v_twin & 31, (v_twin >> 5) & 31,\n"
    "                   (v_twin >> 10) & 31, (v_twin >> 15) & 31);\n"
    "  if ((tw.x | tw.y) != 0) {\n"
    "    u = (u & ~(tw.x * 8)) | ((tw.z & tw.x) * 8);\n"
    "    v = (v & ~(tw.y * 8)) | ((tw.w & tw.y) * 8);\n"
    "  } else {\n"
    "    u = clamp(u, v_limits.x, v_limits.z);\n"
    "    v = clamp(v, v_limits.y, v_limits.w);\n"
    "  }\n"
    "  if (v_depth == 0) {\n"
    "    int px = vram_at(v_tpage.x + (u >> 2), v_tpage.y + v);\n"
    "    return palette_at(v_clut.x + ((px >> ((u & 3) * 4)) & 0xF), v_clut.y);\n"
    "  } else if (v_depth == 1) {\n"
    "    int px = vram_at(v_tpage.x + (u >> 1), v_tpage.y + v);\n"
    "    return palette_at(v_clut.x + ((px >> ((u & 1) * 8)) & 0xFF), v_clut.y);\n"
    "  }\n"
    "  return vram_at(v_tpage.x + u, v_tpage.y + v);\n"
    "}\n"
    "vec3 col5(int raw){\n"
    "  return vec3(float(raw & 31), float((raw >> 5) & 31), float((raw >> 10) & 31)) / 31.0;\n"
    "}\n"
    "int stable_texel(ivec2 p){\n"
    "  /* Clamp before wrap, so a footprint left of u=0 cannot pick up u=255. */\n"
    "  if ((v_twin & 1023) == 0) p=clamp(p,v_limits.xy,v_limits.zw);\n"
    "  return fetch_texel(p.x,p.y);\n"
    "}\n"
    "vec4 stable_bilinear(vec2 uv,int stp){\n"
    "  vec2 p=uv-vec2(0.5), f=fract(p); ivec2 b=ivec2(floor(p));\n"
    "  vec4 sum=vec4(0.0);\n"
    "  for(int y=0;y<2;++y) for(int x=0;x<2;++x){\n"
    "    int raw=stable_texel(b+ivec2(x,y));\n"
    "    float w=(x==0 ? 1.0-f.x : f.x)*(y==0 ? 1.0-f.y : f.y);\n"
    "    if(raw!=0 && ((raw>>15)&1)==stp) sum+=vec4(col5(raw),1.0)*w;\n"
    "  }\n"
    "  return sum;\n"
    "}\n"
    "vec4 hd_texel(vec2 uv){\n"
    "  ivec2 p=ivec2(floor(uv)); vec2 f=fract(uv);\n"
    "  p &= ivec2(255);\n"
    "  ivec4 tw=ivec4(v_twin&31,(v_twin>>5)&31,(v_twin>>10)&31,(v_twin>>15)&31);\n"
    "  if((tw.x|tw.y)!=0) p=(p & ~(tw.xy*8)) | ((tw.zw & tw.xy)*8);\n"
    "  else p=clamp(p,v_limits.xy,v_limits.zw);\n"
    "  vec2 t=(vec2(p)+f-v_hd_source.xy)/v_hd_source.zw;\n"
    "  ivec2 size=textureSize(u_hd_texture,0);\n"
    "  ivec2 at=clamp(ivec2(floor(t*vec2(size))),ivec2(0),size-ivec2(1));\n"
    "  return texelFetch(u_hd_texture,at,0);\n"
    "}\n"
    /* [video] texture_lod: mip-level emulation for CLUT/15-bit pages, which
     * have no GL mip chain (they are decoded per fetch from raw VRAM). The
     * screen-space UV footprint gives a major and a minor axis; up to u_aniso
     * taps go along the major axis (anisotropic filtering) and each averages
     * a k x k texel box the size of the minor footprint (the mip level).
     * Every fetch clamps to the primitive's own UV bounds (or its texture
     * window), so a packed atlas never bleeds into the neighbour; texels
     * that are transparent or of the other STP class carry no weight. */
    "vec3 lod_sample(vec2 uv, vec2 dx, vec2 dy, int stp, vec3 base){\n"
    "  float lx=length(dx), ly=length(dy);\n"
    "  vec2 ax = lx >= ly ? dx : dy;\n"
    "  float major=max(lx,ly), minor=max(min(lx,ly),1.0);\n"
    "  if (major <= 1.0) return base;\n"
    "  int n = clamp(int(ceil(major/minor)), 1, u_aniso);\n"
    "  float box = max(minor, major/float(n));\n"
    "  int k = clamp(int(ceil(box)), 1, 8);\n"
    "  while (k > 1 && n*k*k > 64) k--;\n"
    "  vec2 bx = normalize(dx) * box, by = normalize(dy) * box;\n"
    "  if (lx < 1e-6) bx = vec2(box, 0.0);\n"
    "  if (ly < 1e-6) by = vec2(0.0, box);\n"
    "  vec4 sum = vec4(0.0);\n"
    "  for (int t=0; t<n; ++t) {\n"
    "    vec2 c = uv + ax * ((float(t)+0.5)/float(n) - 0.5);\n"
    "    for (int j=0; j<k; ++j) for (int i=0; i<k; ++i) {\n"
    "      vec2 o = bx*((float(i)+0.5)/float(k)-0.5) + by*((float(j)+0.5)/float(k)-0.5);\n"
    "      int raw = stable_texel(ivec2(floor(c+o)));\n"
    "      if (raw != 0 && ((raw>>15)&1) == stp) sum += vec4(col5(raw), 1.0);\n"
    "    }\n"
    "  }\n"
    "  return sum.a > 0.0 ? sum.rgb / sum.a : base;\n"
    "}\n"
    "void main(){\n"
    "  int stp; vec3 rgb;\n"
    "  int fmode = u_filter & 15;\n"
    "  /* v_persp is 0 for every prim unless [video] perspective_texturing is on\n"
    "   * AND this prim's packet carried full GTE projection provenance, so the\n"
    "   * default is the PS1's affine (noperspective) mapping. */\n"
    "  vec2 uv = (v_persp != 0) ? v_uv_p : v_uv;\n"
    "  vec2 dx=dFdx(uv), dy=dFdy(uv);\n"
    "  vec2 uv0=uv, dx0=dx, dy0=dy;   /* the PS1 sample point (texture_lod) */\n"
    "  if (v_hd_mode != 0) {\n"
    "    vec4 hd=hd_texel(uv);\n"
    "    rgb=hd.rgb;\n"
    "    if(v_hd_mode==1){\n"
    "      if(hd.a==0.0) discard;\n"
    "      int native=fetch_texel(int(floor(uv.x)),int(floor(uv.y)));\n"
    "      if(native==0) discard; stp=(native>>15)&1;\n"
    "    } else if(v_hd_mode==2){\n"
    "      if(hd.a<128.0/255.0) discard; stp=0;\n"
    "    } else if(v_hd_mode==4){\n"
    "      if(hd.a==0.0) discard; stp=hd.a<1.0 ? 1 : 0;\n"
    "    } else {\n"
    "      stp=hd.a<=242.0/255.0 ? 1 : 0;\n"
    "      if(all(equal(hd,vec4(0.0))) || (stp==0 && all(equal(hd.rgb,vec3(0.0))))) discard;\n"
    "    }\n"
    "  } else if (fmode == 0) {\n"
    "    int raw = fetch_texel(int(floor(uv.x)), int(floor(uv.y)));\n"
    "    if (raw == 0) discard;\n"
    "    rgb = col5(raw);\n"
    "    stp = (raw >> 15) & 1;\n"
    "  } else if (fmode == 2) {\n"
    "    uv+=vec2(u_shift); int raw=stable_texel(ivec2(floor(uv)));\n"
    "    if(raw==0) discard; stp=(raw>>15)&1;\n"
    "    float lx=length(dx), ly=length(dy);\n"
    "    dx*=min(1.0,8.0/max(lx,0.001)); dy*=min(1.0,8.0/max(ly,0.001));\n"
    "    int nx=clamp(int(ceil(lx)),1,4), ny=clamp(int(ceil(ly)),1,4);\n"
    "    vec4 sum=vec4(0.0);\n"
    "    for(int y=0;y<ny;++y) for(int x=0;x<nx;++x){\n"
    "      vec2 offset=dx*((float(x)+0.5)/float(nx)-0.5)\n"
    "                 +dy*((float(y)+0.5)/float(ny)-0.5);\n"
    "      sum+=stable_bilinear(uv+offset,stp);\n"
    "    }\n"
    "    rgb=sum.a>0.00001 ? sum.rgb/sum.a : col5(raw);\n"
    "  } else {\n"
    "    /* Bilinear, Beetle-PSX formulation: the NEAREST texel is the base\n"
    "     * (cutout + STP authority), the neighbours lie toward the sub-texel\n"
    "     * offset and clamp to u_limits, and each texel's weight is gated by\n"
    "     * its opacity with the colour renormalised — so prim edges and\n"
    "     * cutout borders keep their colour instead of dissolving into the\n"
    "     * transparent (black) neighbour and discarding whole edge columns.\n"
    "     * Recenter from PS1 top-left point sampling to the bilinear footprint. */\n"
    "    uv += vec2(u_shift);\n"
    "    int iu = int(floor(uv.x)), iv = int(floor(uv.y));\n"
    "    float fx = uv.x - float(iu) - 0.5, fy = uv.y - float(iv) - 0.5;\n"
    "    int sx = fx < 0.0 ? -1 : 1, sy = fy < 0.0 ? -1 : 1;\n"
    "    fx = abs(fx); fy = abs(fy);\n"
    "    int c00 = fetch_texel(iu, iv);\n"
    "    if (c00 == 0) discard;\n"
    "    int c10 = fetch_texel(iu + sx, iv);\n"
    "    int c01 = fetch_texel(iu, iv + sy);\n"
    "    int c11 = fetch_texel(iu + sx, iv + sy);\n"
    "    float w00 = (c00 == 0 ? 0.0 : 1.0) * (1.0 - fx) * (1.0 - fy);\n"
    "    float w10 = (c10 == 0 ? 0.0 : 1.0) * fx * (1.0 - fy);\n"
    "    float w01 = (c01 == 0 ? 0.0 : 1.0) * (1.0 - fx) * fy;\n"
    "    float w11 = (c11 == 0 ? 0.0 : 1.0) * fx * fy;\n"
    "    float opac = w00 + w10 + w01 + w11;\n"
    "    rgb = (col5(c00)*w00 + col5(c10)*w10 + col5(c01)*w01 + col5(c11)*w11) / opac;\n"
    "    float stpf = (float((c00 >> 15) & 1) * w00 + float((c10 >> 15) & 1) * w10\n"
    "                + float((c01 >> 15) & 1) * w01 + float((c11 >> 15) & 1) * w11) / opac;\n"
    "    stp = stpf >= 0.5 ? 1 : 0;\n"
    "  }\n"
    "  if ((u_filter & 16) != 0 && v_hd_mode == 0) rgb = lod_sample(uv0, dx0, dy0, stp, rgb);\n"
    "  if (u_semipass == 1 && stp == 1) discard;\n"
    "  if (u_semipass == 2 && stp == 0) discard;\n"
    "  if (v_raw == 0) rgb = psx_dither(clamp(rgb * (v_cp != 0 ? v_col_p : v_col.rgb) * 2.0, 0.0, 1.0), v_npos);\n"
    "  float dst_factor = 0.0;\n"
    "  if (u_semimode == 4 && v_semi != 0 && stp != 0) {\n"
    "    dst_factor = v_semi == 1 ? 0.5 : 1.0;\n"
    "    if (v_semi == 1) rgb *= 0.5; else if (v_semi == 4) rgb *= 0.25;\n"
    "  }\n"
    "  frag = vec4(rgb, (stp == 1 || u_maskset == 1) ? 1.0 : 0.0);\n"
    "  blend_factor = vec4(0.0, 0.0, 0.0, dst_factor);\n"
    "}\n";

/* Quad blit: used for CPU->VRAM upload flushes and VRAM->VRAM copies.
 * Samples an RGBA8 source (alpha = bit15), splits by STP for the stencil
 * write, optionally ORs set-mask into the output alpha. */
static const char *BLIT_VS =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;   /* native VRAM px */\n"
    "uniform float u_shift;\n"
    "void main(){\n"
    "  gl_Position = vec4((a_pos.x+u_shift)/512.0 - 1.0, (a_pos.y+u_shift)/256.0 - 1.0, 0.0, 1.0); }\n";
/* The same blit into the windowed high-resolution surface: its viewport is the
 * window (W*S px), so x is translated by u_xoff and clipped at u_xhalf like
 * the native-wide passes. A separate program keeps BLIT_VS (the canonical
 * path) untouched. */
static const char *BLIT_VS_HI =
    "#version 330\n"
    "layout(location=0) in vec2 a_pos;   /* native VRAM px */\n"
    "uniform float u_shift;\n"
    "uniform float u_xoff;\n"
    "uniform float u_xhalf;\n"
    "void main(){\n"
    "  gl_Position = vec4((a_pos.x+u_shift+u_xoff)/u_xhalf - 1.0, (a_pos.y+u_shift)/256.0 - 1.0, 0.0, 1.0); }\n";
static const char *BLIT_FS =
    "#version 330\n"
    "out vec4 frag;\n"
    "uniform sampler2D u_src;\n"
    "uniform int u_stp_pass;  /* 0=all, 1=bit15=0 only, 2=bit15=1 only */\n"
    "uniform int u_maskset;\n"
    "uniform int u_src_div;   /* fragcoord -> src texel divisor (S for native-res\n"
    "                            sources, 1 for hr-res sources) */\n"
    "uniform ivec2 u_src_off; /* added after the divide, in src texel units */\n"
    "void main(){\n"
    "  /* Exact integer source fetch — no normalized-uv edge precision. */\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 c = texelFetch(u_src, p / u_src_div + u_src_off, 0);\n"
    "  bool stp = c.a >= 0.5;\n"
    "  if (u_stp_pass == 1 && stp) discard;\n"
    "  if (u_stp_pass == 2 && !stp) discard;\n"
    "  frag = vec4(c.rgb, (stp || u_maskset != 0) ? 1.0 : 0.0);\n"
    "}\n";

/* Pack: re-encode the hr FBO into the native R16UI raw mirror. Runs over a
 * native-res viewport with scissor = dirty rect; each native pixel takes the
 * top-left sample of its S*S block (the sample at the exact native coord). */
static const char *PACK_VS =
    "#version 330\n"
    "void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
static const char *PACK_FS =
    "#version 330\n"
    "uniform sampler2D u_hr;\n"
    "uniform int u_scale;\n"
    "out uint o_pix;\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 c = texelFetch(u_hr, p * u_scale, 0);\n"
    "  uint r = uint(c.r * 255.0 + 0.5) >> 3;\n"
    "  uint g = uint(c.g * 255.0 + 0.5) >> 3;\n"
    "  uint b = uint(c.b * 255.0 + 0.5) >> 3;\n"
    "  o_pix = r | (g << 5) | (b << 10) | (c.a >= 0.5 ? 0x8000u : 0u);\n"
    "}\n";

/* Rebuild stencil bit 0 from a copied RGBA target's alpha. Color writes are
 * disabled while this shader runs; only alpha>=0.5 fragments replace stencil. */
static const char *STENCIL_FS =
    "#version 330\n"
    "uniform sampler2D u_src; out vec4 frag;\n"
    "uniform ivec2 u_off;  /* tile origin in the target; (0,0) for a full copy */\n"
    "void main(){ vec4 c=texelFetch(u_src,ivec2(gl_FragCoord.xy)-u_off,0);\n"
    "  if(c.a<0.5) discard; frag=vec4(0.0); }\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, PSXGL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL shader compile failed: %s\n", log);
        p_glDeleteShader(s); return 0; }
    return s;
}
static GLuint build_program_ex(const char *vs, const char *fs, int dual_source) {
    GLuint v = compile_shader(PSXGL_VERTEX_SHADER, vs), f = compile_shader(PSXGL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, v); p_glAttachShader(p, f);
    if (dual_source) {
        p_glBindFragDataLocationIndexed(p, 0, 0, "frag");
        p_glBindFragDataLocationIndexed(p, 0, 1, "blend_factor");
    }
    p_glLinkProgram(p);
    p_glDeleteShader(v); p_glDeleteShader(f);
    GLint ok = 0; p_glGetProgramiv(p, PSXGL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; log[0]=0; p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stdout, "psxrecomp: GL program link failed: %s\n", log); return 0; }
    return p;
}
static GLuint build_program(const char *vs, const char *fs) {
    return build_program_ex(vs, fs, 0);
}

/* ---- pixel conversion (PS1 1555: bit15=mask, B[14:10] G[9:5] R[4:0]) ---- */
static inline uint32_t conv_1555_to_rgba8(uint16_t p) {
    uint32_t r = (p & 0x1F) << 3, g = ((p >> 5) & 0x1F) << 3, b = ((p >> 10) & 0x1F) << 3;
    uint32_t a = (p >> 15) & 1 ? 0xFF : 0;
    return r | (g << 8) | (b << 16) | (a << 24);   /* RGBA8 little-endian */
}

/* ---- mask-bit stencil --------------------------------------------------- *
 * Stencil bit0 mirrors bit15 of every pixel.
 *   check off: test ALWAYS, op REPLACE with ref = write value.
 *   check on:  test EQUAL 0 (pass iff dest unmasked). GL couples the REPLACE
 *              value to the test reference, so write a 1 via INVERT (the
 *              stored value is known to be 0 when the test passed) and a 0
 *              via KEEP. */
static void mask_stencil_ex(int write_val, int check) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    if (check) {
        glStencilFunc(GL_EQUAL, 0, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, write_val ? GL_INVERT : GL_KEEP);
    } else {
        glStencilFunc(GL_ALWAYS, write_val ? 1 : 0, 0x01);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
}
static void mask_stencil(int write_val) { mask_stencil_ex(write_val, s_mask_check); }
/* Like mask_stencil but never checks (uploads: gpu.c already applied mask). */
static void plain_stencil(int write_val) {
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    glStencilFunc(GL_ALWAYS, write_val ? 1 : 0, 0x01);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

/* PS1 semi-transparency as fixed-function blending, RGB only — the alpha
 * channel (mask bit) is always replaced by the source fragment's alpha.
 *   0: B/2 + F/2   1: B + F   2: B - F   3: B + F/4 */
static void apply_psx_blend(int mode) {
    glEnable(GL_BLEND);
    p_glBlendEquationSeparate((mode & 3) == 2 ? PSXGL_FUNC_REVERSE_SUBTRACT
                                              : PSXGL_FUNC_ADD,
                              PSXGL_FUNC_ADD);
    switch (mode & 3) {
    case 0:
        p_glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
        p_glBlendFuncSeparate(PSXGL_CONSTANT_ALPHA, PSXGL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
        break;
    case 1:
    case 2:
        p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        break;
    case 3:
        p_glBlendColor(0.25f, 0.25f, 0.25f, 0.25f);
        p_glBlendFuncSeparate(PSXGL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        break;
    }
}

/* ---- hr FBO render-state bracket ---------------------------------------- */
static void hr_begin(int clip_to_draw_area) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glViewport(0, 0, VRAM_W * s_hr_scale, VRAM_H * s_hr_scale);
    glEnable(GL_SCISSOR_TEST);
    if (clip_to_draw_area) {
        int ax1 = s_area_x1, ay1 = s_area_y1, ax2 = s_area_x2, ay2 = s_area_y2;
        if (s_pass_active) {
            if (ax1 < s_pass_x) ax1 = s_pass_x;
            if (ay1 < s_pass_y) ay1 = s_pass_y;
            if (ax2 > s_pass_x + s_pass_w - 1) ax2 = s_pass_x + s_pass_w - 1;
            if (ay2 > s_pass_y + s_pass_h - 1) ay2 = s_pass_y + s_pass_h - 1;
        }
        int sw = ax2 - ax1 + 1, sh = ay2 - ay1 + 1;
        if (sw < 0) sw = 0; if (sh < 0) sh = 0;
        glScissor(ax1 * s_hr_scale, ay1 * s_hr_scale,
                  sw * s_hr_scale, sh * s_hr_scale);
    }
}
static void hr_end(void) {
    glDisable(GL_BLEND);
    /* apply_psx_blend mode 2 leaves REVERSE_SUBTRACT armed; reset so later
     * host draws (OSD) that re-enable blend do not inherit B-F math. */
    if (p_glBlendEquationSeparate)
        p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* PGXP depth state for one draw (G1.14). 0: no depth (the default GL state
 * everywhere else), 1: opaque 3D polygon, 2: depth clear draw (always pass,
 * write the far clear depth, no colour or stencil).
 *
 * Mode 1 is two passes so that near-coplanar surfaces keep the PS1's
 * painter order (R4's lane markings and decals are separate polygons drawn
 * after the road, a hair off its plane once SZ is quantised per vertex): the
 * colour pass tests LEQUAL with its depth pulled toward the camera by a
 * relative tolerance (u_zbias, s_pgxp_depth_tol of the distance) and writes
 * no depth; depth_restore then lays down the true depth in a colourless
 * pass. A later polygon within the tolerance of what is already there wins,
 * as in painter order; anything clearly behind is still occluded.
 * DuckStation's depth buffer is a plain LEQUAL with no bias and has this
 * decal problem; this tolerance is our own. Every caller that applies 1 or 2
 * calls depth_restore after its draw (tex: textured program bound). */
static GLint s_geo_uZbias = -1, s_tex_uZbias = -1;
static float s_pgxp_depth_tol = -1.0f;
static float pgxp_depth_tol(void) {
    if (s_pgxp_depth_tol < 0.0f) {
        const char *e = getenv("PSX_PGXP_DEPTH_TOL");
        s_pgxp_depth_tol = e && *e ? (float)atof(e) : 0.02f;
        if (s_pgxp_depth_tol < 0.0f) s_pgxp_depth_tol = 0.0f;
        if (s_pgxp_depth_tol > 0.5f) s_pgxp_depth_tol = 0.5f;
    }
    return s_pgxp_depth_tol;
}
static void depth_apply_ex(int mode, int tex) {
    if (mode == 0) return;
    glEnable(GL_DEPTH_TEST);
    if (mode == 2) {
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_ALWAYS);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    } else {
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_LEQUAL);
        p_glUniform1f(tex ? s_tex_uZbias : s_geo_uZbias, pgxp_depth_tol());
    }
}
/* After the colour pass of mode 1: the depth-only pass (n vertices of the
 * bound buffer), then back to the default state. */
static void depth_restore_ex(int mode, int tex, int n) {
    if (mode == 0) return;
    if (mode == 1) {
        p_glUniform1f(tex ? s_tex_uZbias : s_geo_uZbias, 0.0f);
        GLboolean st = glIsEnabled(GL_STENCIL_TEST), bl = glIsEnabled(GL_BLEND);
        glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_TRUE);
        if (tex) p_glUniform1i(s_uSemipass, 0);
        glDrawArrays(GL_TRIANGLES, 0, n);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (st) glEnable(GL_STENCIL_TEST);
        if (bl) glEnable(GL_BLEND);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    if (mode == 2) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* ---- coherency: CPU -> GPU upload flush --------------------------------- */
/* CPU-side VRAM writes (GP0 A0 transfers, DMA, single pixel pokes) land in
 * the CPU array immediately and accumulate s_up_rects. Flushing before the
 * next GPU op (or readback/present) preserves PS1 command order. */
static void flush_cpu_upload(void) {
    if (!s_raster_ok || s_up_nrects == 0) return;
    const int diag = runtime_upload_diag_enabled();
    if (diag) { s_rt_up_diag[0]++; s_rt_up_diag[1] += (uint64_t)s_up_nrects; }
    flush_flat_batch();  /* queued flat GEO before upload mutates VRAM */
    flush_tex_batch();   /* queued textured draws before this upload writes VRAM */
    hiw_flush_queue();   /* ...and their high-resolution-window replay */
    /* Snapshot + clear first (re-entrancy safe; up_add overflow calls back in). */
    DirtyRect rects[UP_RECTS_MAX];
    int nrects = s_up_nrects;
    memcpy(rects, s_up_rects, (size_t)nrects * sizeof(DirtyRect));
    s_up_nrects = 0;

    /* Stage every rect's CPU data first (texture uploads outside the FBO
     * bracket), then draw all the quads in one bracket. Only the exact
     * uploaded rects are painted — never the union bounding box (stale-CPU
     * flicker class bug, see s_up_rects). */
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        if (diag) s_rt_up_diag[2] += (uint64_t)w * (uint64_t)h;
        coh_record(GL_COH_FLUSH, x, y, x + w - 1, y + h - 1);

        /* RGBA8 staging for the hr quad draw. */
        uint64_t t0 = diag ? SDL_GetPerformanceCounter() : 0;
        for (int row = 0; row < h; row++) {
            const uint16_t *src = s_vram + (size_t)(y + row) * VRAM_W + x;
            uint32_t *dst = s_conv + (size_t)row * w;
            for (int col = 0; col < w; col++) dst[col] = conv_1555_to_rgba8(src[col]);
        }
        if (diag) s_rt_up_diag[3] += SDL_GetPerformanceCounter() - t0;
        t0 = diag ? SDL_GetPerformanceCounter() : 0;
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_conv);

        /* Raw mirror takes the CPU data directly — current for this rect, so no
         * pack is needed for uploaded content. */
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, VRAM_W);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                        PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT,
                        s_vram + (size_t)y * VRAM_W + x);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, 0);
        if (diag) s_rt_up_diag[4] += SDL_GetPerformanceCounter() - t0;
    }

    /* Quads into the hr FBO; two passes split by bit15 so the stencil mirror
     * stays exact. gpu.c applied mask set/check per pixel already — no check
     * here, the data is final. up_tex is VRAM-aligned: src texel = frag/S. */
    uint64_t draw_t0 = diag ? SDL_GetPerformanceCounter() : 0;
    hr_begin(0);
    glDisable(GL_BLEND);
    p_glUseProgram(s_blit_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_up_tex);
    p_glUniform1i(s_uBlitSrc, 0);
    p_glUniform1i(s_uBlitMaskset, 0);
    p_glUniform1i(s_uBlitSrcDiv, s_hr_scale);
    p_glUniform2i(s_uBlitSrcOff, 0, 0);
    p_glBindVertexArray(s_blit_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    for (int i = 0; i < nrects; i++) {
        int x = rects[i].x0, y = rects[i].y0;
        int w = rects[i].x1 - rects[i].x0 + 1;
        int h = rects[i].y1 - rects[i].y0 + 1;
        glScissor(x * s_hr_scale, y * s_hr_scale, w * s_hr_scale, h * s_hr_scale);
        float fx0 = (float)x, fy0 = (float)y, fx1 = (float)(x + w), fy1 = (float)(y + h);
        float verts[6 * 2] = {
            fx0, fy0,  fx1, fy0,  fx0, fy1,
            fx1, fy0,  fx0, fy1,  fx1, fy1,
        };
        p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
        plain_stencil(0); p_glUniform1i(s_uBlitPass, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
        plain_stencil(1); p_glUniform1i(s_uBlitPass, 2); glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    hr_end();
    hiw_mirror_uploads(rects, nrects);
    if (diag) s_rt_up_diag[5] += SDL_GetPerformanceCounter() - draw_t0;
}

/* Recreate one target's stencil mask from its authoritative alpha channel.
 * Sampling an attached render target is undefined, so copy color to the shared
 * scratch texture first. Uncapped Fit can exceed the native VRAM width. */
static void rebuild_target_stencil(GLuint target_fbo, int target_w, int target_h) {
    if (target_w > s_scratch_w || target_h > s_scratch_h) {
        if (target_w > s_scratch_w) s_scratch_w = target_w;
        if (target_h > s_scratch_h) s_scratch_h = target_h;
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_scratch_w, s_scratch_h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, target_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    p_glBlitFramebuffer(0, 0, target_w, target_h, 0, 0, target_w, target_h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, target_w, target_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x01);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0x01);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    p_glUseProgram(s_stencil_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    p_glUniform1i(s_uStencilSrc, 0);
    p_glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* Grow a staging texture to at least w x h (never shrinks). The recorded
 * size is committed only after the driver accepted the new storage: a size
 * past the GPU limit is refused, and a failed allocation puts the old storage
 * back, so the recorded size always matches the texture. Logs the first
 * refusal. Returns 1 when the texture holds at least w x h. */
static int stage_tex_grow(GLuint tex, int *cur_w, int *cur_h, int w, int h,
                          const char *what, int *logged) {
    if (w <= *cur_w && h <= *cur_h) return 1;
    int nw = w > *cur_w ? w : *cur_w, nh = h > *cur_h ? h : *cur_h;
    int ok = !(s_gl_max_dim > 0 && (nw > s_gl_max_dim || nh > s_gl_max_dim)) &&
             nw > 0 && nh > 0;
    if (ok) {
        while (glGetError() != GL_NO_ERROR) {}
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        if (glGetError() != GL_NO_ERROR) {
            ok = 0;
            if (*cur_w > 0 && *cur_h > 0)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, *cur_w, *cur_h, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, NULL);
            while (glGetError() != GL_NO_ERROR) {}
        }
    }
    if (!ok) {
        if (!*logged) {
            fprintf(stdout, "psxrecomp: GL %s %dx%d refused (GPU limit %d); kept at "
                    "%dx%d\n", what, nw, nh, s_gl_max_dim, *cur_w, *cur_h);
            *logged = 1;
        }
        return 0;
    }
    *cur_w = nw; *cur_h = nh;
    return 1;
}

/* Grow the shared scratch texture to at least w x h (never shrinks). The
 * scratch holds copy sources and stencil-rebuild tiles; at S > 1 it is sized
 * to what those operations actually need instead of the whole hr surface.
 * Returns 0 (scratch unchanged) when w x h cannot be allocated. */
static int s_scratch_refused_logged = 0;
static int scratch_ensure(int w, int h) {
    return stage_tex_grow(s_scratch_tex, &s_scratch_w, &s_scratch_h, w, h,
                          "copy/stencil scratch", &s_scratch_refused_logged);
}

/* Rebuild the stencil mask of [x, x+w) x [y, y+h) (target px) from alpha, in
 * tiles no larger than GL_SCRATCH_TILE so the scratch never has to match a
 * multi-gigapixel surface. Same result as rebuild_target_stencil over that
 * rect; used only at S > 1. */
static void rebuild_target_stencil_tiled(GLuint target_fbo, int target_w, int target_h,
                                         int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > target_w) w = target_w - x;
    if (y + h > target_h) h = target_h - y;
    if (w <= 0 || h <= 0) return;
    int tile_w = w < GL_SCRATCH_TILE ? w : GL_SCRATCH_TILE;
    int tile_h = h < GL_SCRATCH_TILE ? h : GL_SCRATCH_TILE;
    if (!scratch_ensure(tile_w, tile_h)) return;
    for (int ty = y; ty < y + h; ty += tile_h) {
        for (int tx = x; tx < x + w; tx += tile_w) {
            int cw = (x + w - tx) < tile_w ? (x + w - tx) : tile_w;
            int ch = (y + h - ty) < tile_h ? (y + h - ty) : tile_h;
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, target_fbo);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
            glDisable(GL_SCISSOR_TEST);
            p_glBlitFramebuffer(tx, ty, tx + cw, ty + ch, 0, 0, cw, ch,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, target_fbo);
            glViewport(0, 0, target_w, target_h);
            glEnable(GL_SCISSOR_TEST);
            glScissor(tx, ty, cw, ch);
            glDisable(GL_BLEND);
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0x01);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilFunc(GL_ALWAYS, 1, 0x01);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            p_glUseProgram(s_stencil_prog);
            p_glActiveTexture(PSXGL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
            p_glUniform1i(s_uStencilSrc, 0);
            p_glUniform2i(s_uStencilOff, tx, ty);
            p_glBindVertexArray(s_empty_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
    }
    p_glUniform2i(s_uStencilOff, 0, 0);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

static void rebuild_mask_stencils(void) {
    if (s_stencil_valid || !s_raster_ok) return;
    hiw_flush_queue();
    int hw = VRAM_W * s_hr_scale, hh = VRAM_H * s_hr_scale;
    if (s_out_scale <= 1) {
        /* Native: the historical whole-surface rebuild, unchanged. */
        rebuild_target_stencil(s_hr_fbo, hw, hh);
        for (int i = 0; i < WIDE_MAX_SURF; i++) {
            if (s_wide_fbo[i])
                rebuild_target_stencil(s_wide_fbo[i], g_wide_w * s_out_scale, hh);
            wst_clear(i);
        }
    } else {
        /* S > 1: only primitives can leave stencil behind alpha, so rebuild
         * their bbox union, tiled. Wide surfaces keep a full rebuild (their
         * margins are not in canonical coordinates), also tiled. In windowed
         * mode the hr surface is 1x and the hi window takes the same rect. */
        if (s_stencil_stale.set) {
            int S = s_hr_scale;
            rebuild_target_stencil_tiled(s_hr_fbo, hw, hh,
                s_stencil_stale.x0 * S, s_stencil_stale.y0 * S,
                (s_stencil_stale.x1 - s_stencil_stale.x0 + 1) * S,
                (s_stencil_stale.y1 - s_stencil_stale.y0 + 1) * S);
            for (int t = 0; hiw_on() && t < s_hiw_n; t++) {
                const HiwTile *T = &s_hiw_t[t];
                int x0 = s_stencil_stale.x0 > T->x0 ? s_stencil_stale.x0 : T->x0;
                int x1 = s_stencil_stale.x1 + 1 < T->x1 ? s_stencil_stale.x1 + 1 : T->x1;
                if (x1 > x0)
                    rebuild_target_stencil_tiled(T->fbo, (T->x1 - T->x0) * s_out_scale,
                        VRAM_H * s_out_scale, (x0 - T->x0) * s_out_scale,
                        s_stencil_stale.y0 * s_out_scale, (x1 - x0) * s_out_scale,
                        (s_stencil_stale.y1 - s_stencil_stale.y0 + 1) * s_out_scale);
            }
        }
        /* Wide surfaces: their stale rects (s_wst_*), the centre columns
         * left for later while the centre is spliced from hr. */
        int defer = wide_fast_center_valid() && !view_enabled;
        for (int i = 0; i < WIDE_MAX_SURF; i++) {
            if (!s_wide_fbo[i]) continue;
            if (wst_full()) wst_all(i);
            wst_rebuild(i, defer && !wst_full());
        }
    }
    rect_clear(&s_stencil_stale);
    s_stencil_valid = 1;
}

/* Rebuild surface i's stale stencil rect (S > 1). defer: leave the centre
 * columns [g_wide_off, g_wide_w - g_wide_off) stale (see s_wst_*). */
static void wst_rebuild_rect(int i, int x0, int x1, int y0, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    int S = s_out_scale;
    rebuild_target_stencil_tiled(s_wide_fbo[i], g_wide_w * S, VRAM_H * S,
                                 x0 * S, y0 * S, (x1 - x0) * S, (y1 - y0) * S);
    s_wst_rebuilds++;
    s_wst_px += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
}
static void wst_rebuild(int i, int defer) {
    int x0 = s_wst_x0[i], x1 = s_wst_x1[i], y0 = s_wst_y0[i], y1 = s_wst_y1[i];
    if (x1 <= x0 || y1 <= y0) return;
    int cl = g_wide_off, cr = g_wide_w - g_wide_off;
    if (!defer || cl <= 0 || cr <= cl) {
        wst_rebuild_rect(i, x0, x1, y0, y1);
        wst_clear(i);
        return;
    }
    wst_rebuild_rect(i, x0, x1 < cl ? x1 : cl, y0, y1);   /* left margin */
    wst_rebuild_rect(i, x0 > cr ? x0 : cr, x1, y0, y1);   /* right margin */
    if (x1 > cl && x0 < cr) {   /* the centre part stays stale */
        s_wst_x0[i] = x0 > cl ? x0 : cl;
        s_wst_x1[i] = x1 < cr ? x1 : cr;
        s_wst_deferred++;
    } else {
        wst_clear(i);
    }
}

/* Before a mirror draw into the current wide surface: under the mask check
 * with the centre splice off, the stencil it tests must be current
 * everywhere, including a centre part a rebuild left stale. Called before
 * any GL state for the draw is set (it binds its own). */
static void wide_stencil_ready(void) {
    if (!g_wide_cur || !s_mask_check || s_out_scale <= 1 || wst_full()) return;
    int i = wide_index(g_wide_cur);
    if (i < 0 || s_wst_x1[i] <= s_wst_x0[i] || s_wst_y1[i] <= s_wst_y0[i]) return;
    if (wide_fast_center_valid() && !view_enabled) return;
    hiw_flush_queue();
    wst_rebuild(i, 0);
}

/* ---- windowed high-resolution surface: mirror plumbing ------------------ */
static int hiw_on(void) { return s_hiw && s_hiw_n > 0; }

/* The first tile that holds all of [x0, x1), or NULL. */
static const HiwTile *hiw_tile_holding(int x0, int x1) {
    for (int t = 0; t < s_hiw_n; t++)
        if (x0 >= s_hiw_t[t].x0 && x1 <= s_hiw_t[t].x1) return &s_hiw_t[t];
    return NULL;
}

/* Scissored clear of [x, x+w) x [y, y+h) (native) in every tile it touches.
 * Leaves the scissor test enabled and a tile FBO bound; callers rebind. */
static void hiw_clear_rect(int x, int y, int w, int h, float r, float g, float b,
                           float a, int stencil) {
    if (!hiw_on() || w <= 0 || h <= 0) return;
    hiw_flush_queue();
    int S = s_out_scale;
    for (int t = 0; t < s_hiw_n; t++) {
        const HiwTile *T = &s_hiw_t[t];
        int x0 = x > T->x0 ? x : T->x0, x1 = x + w < T->x1 ? x + w : T->x1;
        if (x1 <= x0) continue;
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, T->fbo);
        glViewport(0, 0, (T->x1 - T->x0) * S, VRAM_H * S);
        glEnable(GL_SCISSOR_TEST);
        glScissor((x0 - T->x0) * S, y * S, (x1 - x0) * S, h * S);
        glClearColor(r, g, b, a);
        glClearStencil(stencil);
        glStencilMask(0xFF);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
}

/* Bind the hi blit program for tile T: projection and shift set, the source
 * texture on unit 0. */
static void hiw_blit_begin(const HiwTile *T, GLuint src_tex, int src_div, int off_x,
                           int off_y, int maskset) {
    int S = s_out_scale, ww = T->x1 - T->x0;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, T->fbo);
    glViewport(0, 0, ww * S, VRAM_H * S);
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    p_glUseProgram(s_blit_hi_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    p_glUniform1i(s_uBhSrc, 0);
    p_glUniform1i(s_uBhMaskset, maskset);
    p_glUniform1i(s_uBhSrcDiv, src_div);
    p_glUniform2i(s_uBhSrcOff, off_x, off_y);
    p_glUniform1f(s_uBhShift, s_shift_hi);
    p_glUniform1f(s_uBhXoff, (float)(-T->x0));
    p_glUniform1f(s_uBhXhalf, (float)ww / 2.0f);
    p_glBindVertexArray(s_blit_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
}

static void blit_quad_verts(int x, int y, int w, int h) {
    float fx0 = (float)x, fy0 = (float)y, fx1 = (float)(x + w), fy1 = (float)(y + h);
    float verts[6 * 2] = {
        fx0, fy0,  fx1, fy0,  fx0, fy1,
        fx1, fy0,  fx0, fy1,  fx1, fy1,
    };
    p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
}

/* CPU->VRAM uploads into the tiles: the same staged native texels (s_up_tex,
 * VRAM-aligned) drawn at S, texel = frag/S + tile origin. */
static void hiw_mirror_uploads(const DirtyRect *rects, int nrects) {
    if (!hiw_on() || nrects <= 0) return;
    hiw_flush_queue();
    int S = s_out_scale;
    for (int t = 0; t < s_hiw_n; t++) {
        const HiwTile *T = &s_hiw_t[t];
        int any = 0;
        for (int i = 0; i < nrects; i++) {
            int x0 = rects[i].x0 > T->x0 ? rects[i].x0 : T->x0;
            int x1 = rects[i].x1 + 1 < T->x1 ? rects[i].x1 + 1 : T->x1;
            if (x1 <= x0) continue;
            if (!any) { hiw_blit_begin(T, s_up_tex, S, T->x0, 0, 0); any = 1; }
            int y = rects[i].y0, h = rects[i].y1 - rects[i].y0 + 1;
            glScissor((x0 - T->x0) * S, y * S, (x1 - x0) * S, h * S);
            blit_quad_verts(x0, y, x1 - x0, h);
            plain_stencil(0); p_glUniform1i(s_uBhPass, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
            plain_stencil(1); p_glUniform1i(s_uBhPass, 2); glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        if (any) hr_end();
    }
}

/* One column chunk [a, b) (dest columns) of hiw_mirror_copy. Dest columns
 * whose source column lies in a tile take it at S: every tile holding part
 * of the source stages it into the window scratch at ((c - a) * S, 0) before
 * anything is drawn. The rest take the 1x source the canonical copy staged
 * at the shared scratch origin, upscaled nearest. Two passes by source bit15,
 * so each tile's stencil tracks the mask. */
static int s_hiw_scratch_refused_logged = 0;
static void hiw_mirror_copy_chunk(int a, int b, int shift, int sy, int dx, int dy,
                                  int h) {
    static uint8_t hi_src[VRAM_W];
    int S = s_out_scale, staged = 0;
    memset(hi_src + a, 0, (size_t)(b - a));
    for (int t = 0; t < s_hiw_n; t++) {
        const HiwTile *T = &s_hiw_t[t];
        int s0 = a + shift > T->x0 ? a + shift : T->x0;
        int s1 = b + shift < T->x1 ? b + shift : T->x1;
        if (s1 <= s0) continue;
        if (!staged) {
            if (!s_hiw_scratch_fbo ||
                !stage_tex_grow(s_hiw_scratch_tex, &s_hiw_scratch_w, &s_hiw_scratch_h,
                                (b - a) * S, h * S, "high-resolution copy scratch",
                                &s_hiw_scratch_refused_logged))
                break;   /* every column takes the 1x source */
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_hiw_scratch_fbo);
            glDisable(GL_SCISSOR_TEST);
            staged = 1;
        }
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, T->fbo);
        p_glBlitFramebuffer((s0 - T->x0) * S, sy * S, (s1 - T->x0) * S, (sy + h) * S,
                            (s0 - shift - a) * S, 0, (s1 - shift - a) * S, h * S,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        memset(hi_src + (s0 - shift), 1, (size_t)(s1 - s0));
    }
    if (staged) {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    }
    for (int t = 0; t < s_hiw_n; t++) {
        const HiwTile *T = &s_hiw_t[t];
        int d0 = a > T->x0 ? a : T->x0, d1 = b < T->x1 ? b : T->x1;
        if (d1 <= d0) continue;
        for (int from_hi = 0; from_hi < 2; from_hi++) {
            int began = 0;
            for (int c = d0; c < d1;) {
                int e = c + 1;
                while (e < d1 && hi_src[e] == hi_src[c]) e++;
                if (hi_src[c] == from_hi) {
                    if (!began) {
                        if (from_hi)   /* texel = frag - chunk origin in the tile */
                            hiw_blit_begin(T, s_hiw_scratch_tex, 1, -(a - T->x0) * S,
                                           -dy * S, s_mask_set);
                        else           /* texel = frag/S + tile origin - dest */
                            hiw_blit_begin(T, s_scratch_tex, S, T->x0 - dx, -dy,
                                           s_mask_set);
                        began = 1;
                    }
                    glScissor((c - T->x0) * S, dy * S, (e - c) * S, h * S);
                    blit_quad_verts(c, dy, e - c, h);
                    mask_stencil(s_mask_set); p_glUniform1i(s_uBhPass, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
                    mask_stencil(1);          p_glUniform1i(s_uBhPass, 2); glDrawArrays(GL_TRIANGLES, 0, 6);
                }
                c = e;
            }
            if (began) hr_end();
        }
    }
}

/* VRAM->VRAM copy into the tiles. The canonical copy (1x) has just staged its
 * source at the shared scratch origin. The dest columns any tile holds are
 * copied in chunks narrow enough for the window scratch at S (the GPU limit),
 * in memmove order: a source right of its dest walks the chunks left to
 * right, one left of it right to left, so no chunk overwrites a later chunk's
 * source and an overlapping copy reads pre-copy pixels like the canonical
 * one. */
static void hiw_mirror_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!hiw_on()) return;
    hiw_flush_queue();
    int S = s_out_scale, shift = sx - dx, lo = VRAM_W, hi = 0;
    for (int t = 0; t < s_hiw_n; t++) {
        int a = dx > s_hiw_t[t].x0 ? dx : s_hiw_t[t].x0;
        int b = dx + w < s_hiw_t[t].x1 ? dx + w : s_hiw_t[t].x1;
        if (b > a) { if (a < lo) lo = a; if (b > hi) hi = b; }
    }
    if (hi <= lo) return;
    int cols = s_gl_max_dim > 0 ? s_gl_max_dim / S : hi - lo;
    if (cols < 1) cols = 1;
    int n = (hi - lo + cols - 1) / cols;
    for (int k = 0; k < n; k++) {
        int i = shift < 0 ? n - 1 - k : k;
        int a = lo + i * cols, b = a + cols < hi ? a + cols : hi;
        hiw_mirror_copy_chunk(a, b, shift, sy, dx, dy, h);
    }
}

static uint64_t hiw_tile_bytes(int x0, int x1) {
    return psx_gl_window_bytes(s_out_scale, x1 - x0);
}

/* Allocate a tile over [nx0, nx1) (64-aligned) and release the tiles in
 * `drop` (a bitmask of indices). The new surface is seeded by upscaling the
 * 1x content (nearest), then every existing tile's overlapping columns are
 * carried over at S, and the stencil is rebuilt from alpha. Returns NULL,
 * leaving the tiles as they were, when the new tile would exceed the driver
 * limit, the memory budget (all tiles kept plus the new one) or
 * HIW_MAX_TILES, or fails to allocate. */
static const HiwTile *hiw_alloc_tile(int nx0, int nx1, unsigned drop) {
    int S = s_out_scale, W = (nx1 - nx0) * S, H = VRAM_H * S;
    uint64_t bytes = hiw_tile_bytes(nx0, nx1);
    int kept = 0;
    for (int t = 0; t < s_hiw_n; t++)
        if (!(drop & (1u << t))) { kept++; bytes += hiw_tile_bytes(s_hiw_t[t].x0, s_hiw_t[t].x1); }
    if (kept >= HIW_MAX_TILES) return NULL;
    if ((s_gl_max_dim > 0 && (W > s_gl_max_dim || H > s_gl_max_dim)) ||
        (s_vram_budget && bytes > s_vram_budget))
        return NULL;
    while (glGetError() != GL_NO_ERROR) {}
    GLuint tex = make_tex(GL_RGBA8, W, H, GL_RGBA, GL_UNSIGNED_BYTE), rb = 0, fbo = 0;
    p_glGenRenderbuffers(1, &rb);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, W, H);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, 0);
    int ok = glGetError() == GL_NO_ERROR && make_fbo(&fbo, tex, rb);
    if (!ok) {
        if (fbo) p_glDeleteFramebuffers(1, &fbo);
        p_glDeleteRenderbuffers(1, &rb);
        glDeleteTextures(1, &tex);
        if (!s_hiw_refused_logged) {
            fprintf(stdout, "psxrecomp: GL high-resolution window %dx%d failed to "
                    "allocate; that display presents at 1x\n", W, H);
            s_hiw_refused_logged = 1;
        }
        return NULL;
    }
    /* Seed from the authoritative 1x surface, then carry the tiles over. */
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(nx0 * s_hr_scale, 0, nx1 * s_hr_scale, VRAM_H * s_hr_scale,
                        0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    for (int t = 0; t < s_hiw_n; t++) {
        const HiwTile *T = &s_hiw_t[t];
        int i0 = nx0 > T->x0 ? nx0 : T->x0, i1 = nx1 < T->x1 ? nx1 : T->x1;
        if (i1 <= i0) continue;
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, T->fbo);
        p_glBlitFramebuffer((i0 - T->x0) * S, 0, (i1 - T->x0) * S, H,
                            (i0 - nx0) * S, 0, (i1 - nx0) * S, H,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    int n = 0;
    for (int t = 0; t < s_hiw_n; t++) {
        if (drop & (1u << t)) {
            p_glDeleteFramebuffers(1, &s_hiw_t[t].fbo);
            p_glDeleteRenderbuffers(1, &s_hiw_t[t].rb);
            glDeleteTextures(1, &s_hiw_t[t].tex);
        } else {
            s_hiw_t[n++] = s_hiw_t[t];
        }
    }
    HiwTile *nt = &s_hiw_t[n];
    nt->x0 = nx0; nt->x1 = nx1; nt->tex = tex; nt->rb = rb; nt->fbo = fbo;
    s_hiw_n = n + 1;
    s_hiw_grows++;
    rebuild_target_stencil_tiled(fbo, W, H, 0, 0, W, H);
    fprintf(stdout, "psxrecomp: GL high-resolution window x=[%d,%d) at %dx: %dx%d "
            "(%llu MiB; %d tile%s)\n", nx0, nx1, S, W, H,
            (unsigned long long)(hiw_tile_bytes(nx0, nx1) >> 20), s_hiw_n,
            s_hiw_n == 1 ? "" : "s");
    return nt;
}

/* The tile holding columns [x0, x1), created or grown on first sight:
 * 1. one surface over the union of every display shown so far (a single
 *    window growing with the layout, as long as it fits);
 * 2. else this display merged with the tiles it overlaps;
 * 3. else a tile of its own (side-by-side buffers too wide together).
 * Returns NULL (logged once) when none fits the driver limit, the memory
 * budget or HIW_MAX_TILES; that display then presents from the 1x surface. */
static const HiwTile *hiw_ensure(int x0, int x1) {
    if (!s_hiw || !s_raster_ok) return NULL;
    if (x0 < 0) x0 = 0;
    if (x1 > VRAM_W) x1 = VRAM_W;
    if (x1 <= x0) return NULL;
    hiw_flush_queue();   /* every caller is about to read the window */
    const HiwTile *T = hiw_tile_holding(x0, x1);
    if (T) return T;
    int a0 = x0 / HIW_ALIGN * HIW_ALIGN;
    int a1 = (x1 + HIW_ALIGN - 1) / HIW_ALIGN * HIW_ALIGN;
    if (a1 > VRAM_W) a1 = VRAM_W;
    int u0 = a0, u1 = a1, m0 = a0, m1 = a1;
    unsigned all = 0, over = 0;
    for (int t = 0; t < s_hiw_n; t++) {
        all |= 1u << t;
        if (s_hiw_t[t].x0 < u0) u0 = s_hiw_t[t].x0;
        if (s_hiw_t[t].x1 > u1) u1 = s_hiw_t[t].x1;
        if (s_hiw_t[t].x1 > a0 && s_hiw_t[t].x0 < a1) {
            over |= 1u << t;
            if (s_hiw_t[t].x0 < m0) m0 = s_hiw_t[t].x0;
            if (s_hiw_t[t].x1 > m1) m1 = s_hiw_t[t].x1;
        }
    }
    T = hiw_alloc_tile(u0, u1, all);
    if (!T && over && over != all) T = hiw_alloc_tile(m0, m1, over);
    if (!T && s_hiw_n) T = hiw_alloc_tile(a0, a1, 0);
    if (T) return T;
    if (!s_hiw_refused_logged) {
        fprintf(stdout, "psxrecomp: GL high-resolution window [%d,%d) at %dx does not "
                "fit beside %d tile%s (GPU limit %d, %llu MiB budget); that display "
                "presents at 1x\n", a0, a1, s_out_scale, s_hiw_n, s_hiw_n == 1 ? "" : "s",
                s_gl_max_dim, (unsigned long long)(s_vram_budget >> 20));
        s_hiw_refused_logged = 1;
    }
    return NULL;
}

/* ---- coherency: hr FBO -> raw mirror (pack) ------------------------------ */
static void pack_flush(void) {
    if (!s_raster_ok || !s_pack_dirty.set) return;
    /* A generated frame draws into its own surfaces: the raw mirror keeps
     * the real VRAM (its pack dirt is restored afterwards). */
    if (s_fg_drawing) return;
    hiw_flush_queue();   /* queued window draws sample the raw mirror as it is now */
    int x = s_pack_dirty.x0, y = s_pack_dirty.y0;
    int w = s_pack_dirty.x1 - s_pack_dirty.x0 + 1;
    int h = s_pack_dirty.y1 - s_pack_dirty.y0 + 1;
    rect_clear(&s_pack_dirty);
    coh_record(GL_COH_PACK, x, y, x + w - 1, y + h - 1);

    if (s_hd_native_authority) {
        /* All pending draws consumed the previous raw version before this
         * refresh. Never encode the HD presentation image into native words. */
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, VRAM_W);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, PSXGL_RED_INTEGER,
                       GL_UNSIGNED_SHORT, s_vram + (size_t)y * VRAM_W + x);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, 0);
        return;
    }

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_raw_fbo);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    p_glUseProgram(s_pack_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_hr_tex);
    p_glUniform1i(s_uPackHr, 0);
    p_glUniform1i(s_uPackScale, s_hr_scale);
    p_glBindVertexArray(s_empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* Make sure the raw mirror is current for a textured draw that samples the
 * given texture page / CLUT. */
static void flush_pack_if_sampling(int tpage_x, int tpage_y, int depth,
                                   int clut_x, int clut_y) {
    if (!s_pack_dirty.set) return;
    int page_w = depth == 0 ? 64 : depth == 1 ? 128 : 256;  /* VRAM columns */
    if (tpage_x >= 0 && rect_intersects(&s_pack_dirty, tpage_x, tpage_y,
                        tpage_x + page_w - 1, tpage_y + 255)) {
        flush_flat_batch();
        flush_tex_batch();   /* queued draws are part of s_pack_dirty — realise them before packing */
        pack_flush(); return;
    }
    if (depth <= 1) {
        int n = depth == 0 ? 16 : 256;
        if (rect_intersects(&s_pack_dirty, clut_x, clut_y, clut_x + n - 1, clut_y)) {
            flush_flat_batch();
            flush_tex_batch();
            pack_flush();
        }
    }
}

/* ---- coherency: GPU -> CPU readback -------------------------------------- */
/* Defined with the depth24 policy below; needed here for the readback guard. */
static int s_depth24_skip_up = 0;
static DirtyRect s_d24_skip_fb; /* union of skipped MDEC FB rects (VRAM halfwords) */
static int cpu_vram_authoritative(void) {
    return s_cpu_auth_dual || s_hd_native_authority || s_depth24_skip_up;
}

static void ensure_cpu(void) {
    extern int psx_netplay_active(void);
    if (!s_raster_ok || !s_gpu_dirty) return;
    /* Guest-visible readbacks run on the emulation thread under a sync point
     * and land in gpu.c's array. On the render thread s_vram is the private
     * upload source: reading back there would mark the CPU side current
     * without the guest's array ever seeing the pixels. */
    if (rth_replaying()) return;
    /* Dual-raster / netplay and depth24: CPU VRAM is written on every GP0.
     * Packed RGB888 movie uploads are deliberately absent from the FBO;
     * reading it back would overwrite the movie with stale 1555 words. */
    if (cpu_vram_authoritative() || psx_netplay_active()) {
        s_gpu_dirty = 0;
        rect_clear(&s_cpu_dirty);
        return;
    }
    flush_flat_batch();
    flush_tex_batch();   /* realise queued textured draws before reading the FBO back */
    flush_cpu_upload();
    pack_flush();
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    /* CPU uploads have already landed; raw packing preserves command order.
     * A union is safe for readback (GPU -> CPU), unlike CPU upload unions.
     * Do not reuse s_pack_dirty: texture sampling can consume it before CPU reads. */
    int rx = s_cpu_dirty.set ? s_cpu_dirty.x0 : 0;
    int ry = s_cpu_dirty.set ? s_cpu_dirty.y0 : 0;
    int rw = s_cpu_dirty.set ? s_cpu_dirty.x1 - rx + 1 : VRAM_W;
    int rh = s_cpu_dirty.set ? s_cpu_dirty.y1 - ry + 1 : VRAM_H;
    glPixelStorei(PSXGL_PACK_ROW_LENGTH, VRAM_W);
    glReadPixels(rx, ry, rw, rh, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT,
                 s_vram + (size_t)ry * VRAM_W + rx);
    glPixelStorei(PSXGL_PACK_ROW_LENGTH, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    s_gpu_dirty = 0;
    rect_clear(&s_cpu_dirty);
    coh_record(GL_COH_ENSURE, rx, ry, rx + rw - 1, ry + rh - 1);

}

/* ---- GPU primitives ------------------------------------------------------ */

static uint64_t s_scene_prims = 0;     /* frame_perf: scene primitives submitted (pre double-draw) */
static uint64_t s_scene_prims_tex = 0; /* frame_perf: of which textured (vs flat geometry)         */
static void flush_tex_batch(void);     /* fwd: drained at the backdrop-phase boundary below */
static void mark_prim_dirty(const int *xs, const int *ys, int n, int textured) {
    s_scene_prims++;
    int x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] < x0) x0 = xs[i]; if (xs[i] > x1) x1 = xs[i];
        if (ys[i] < y0) y0 = ys[i]; if (ys[i] > y1) y1 = ys[i];
    }
    /* A sub-pixel-corrected vertex lies in [int, int+1) of the integer position
     * it was rounded from, so the corrected prim can touch one more pixel on
     * each max edge than the integer bbox covers. Widen before clipping so the
     * pack/readback dirty rect never trails the drawn area. */
    if (s_pc_valid) { x1 += 1; y1 += 1; }
    s_bdg_prims++;   /* dbg: prims seen this frame (gate is now per-prim, see bd_prim_gate) */
    if (s_ptrace_n < PTRACE_CAP) {
        PrimRec *p = &s_ptrace[s_ptrace_n++];
        p->x0 = (short)x0; p->x1 = (short)x1; p->y0 = (short)y0; p->y1 = (short)y1;
        p->tex = (unsigned char)textured;
    }
    if (x0 < s_area_x1) x0 = s_area_x1;
    if (y0 < s_area_y1) y0 = s_area_y1;
    if (x1 > s_area_x2) x1 = s_area_x2;
    if (y1 > s_area_y2) y1 = s_area_y2;
    rect_add(&s_pack_dirty, x0, y0, x1, y1);
    if (s_out_scale > 1) rect_add(&s_stencil_stale, x0, y0, x1, y1);
    if (!cpu_vram_authoritative()) rect_add(&s_cpu_dirty, x0, y0, x1, y1);
    /* Dual-raster keeps CPU current via SW writes — do not mark GPU-ahead. */
    if (!cpu_vram_authoritative())
        s_gpu_dirty = 1;
    coh_record(GL_COH_DRAW, x0, y0, x1, y1);
}

/* ---- native-wide mirror pass plumbing ----------------------------------- *
 * Re-issue an already-set-up draw (program/VAO/VBO/blend/stencil bound) into
 * the active wide surface. The geometry positions are identical on the host
 * side; the x translation (wide_dx) and the wider clip are applied entirely in
 * the vertex shader via u_xoff / u_xhalf, so the SAME glDrawArrays produces the
 * shifted copy. wide_target_begin binds the wide FBO + viewport + scissor and
 * sets the projection uniforms; wide_target_end restores u_xoff=0/u_xhalf=512
 * on the active program so the next canonical pass is bit-identical. The
 * caller stays inside hr_begin/hr_end; hr_end unbinds and the next hr_begin
 * resets the viewport, so no viewport restore is needed mid-function.
 *
 * The scissor X is the FULL wide surface (NOT the draw area): the SW reference
 * (rt_wide()) deliberately lets the shifted geometry fill the revealed 16:9
 * margins that lie OUTSIDE the game's 4:3 draw-area x-range; scissoring X to
 * the (translated) draw area would crop exactly the margin content native-wide
 * exists to reveal. The scissor Y stays clamped to the DRAW AREA, exactly like
 * rt_wide() (t.cy1/cy2 = g_clip_y1/y2): native-wide only widens X. A full-height
 * Y scissor let draws that canonically clip at a vertical double-buffer band
 * boundary (MMX6: draw area alternates y=0/y=240, both bands in ONE wide
 * surface) bleed into the OTHER band's rows — presented one frame later as
 * top/bottom edge flicker (16:9 GL only). */
/* Wide-surface scissor: full wide width, draw-area rows (see above). Every
 * draw into a wide surface uses it, including the full-screen-overlay pass. */
static void wide_band_rows(int *out_sy, int *out_sh) {
    int y1 = s_area_y1, y2 = s_area_y2;
    if (s_pass_active) {   /* render pass: stay inside the backed-up band */
        if (y1 < s_pass_y) y1 = s_pass_y;
        if (y2 > s_pass_y + s_pass_h - 1) y2 = s_pass_y + s_pass_h - 1;
    }
    int sy = y1, sh = y2 - y1 + 1;
    if (sy < 0) { sh += sy; sy = 0; }
    if (sy + sh > VRAM_H) sh = VRAM_H - sy;
    if (sh < 0) sh = 0;
    *out_sy = sy; *out_sh = sh;
}
static void wide_band_scissor_x(int x, int w) {
    int sy, sh;
    wide_band_rows(&sy, &sh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * s_out_scale, sy * s_out_scale, w * s_out_scale, sh * s_out_scale);
}
static void wide_band_scissor(void) { wide_band_scissor_x(0, g_wide_w); }
static void wide_target_begin(int dx, GLint uXoff, GLint uXhalf) {
    if (s_ws_ablate != 3)   /* ablate 3: no FBO rebind (draws land in hr — perf probe) */
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, g_wide_cur);
    /* Windowed high-resolution mode: the wide surfaces are at s_out_scale while
     * the canonical pass ran at 1x, so they take the S sample-grid shift. */
    if (s_hiw)
        p_glUniform1f(uXoff == s_tex_uXoff ? s_tex_uShift : s_geo_uShift, s_shift_hi);
    glViewport(0, 0, g_wide_w * s_out_scale, VRAM_H * s_out_scale);
    wide_band_scissor_x(view_pad_left, g_wide_w - view_pad_left - view_pad_right);
    p_glUniform1f(uXoff, (float)dx);
    p_glUniform1f(uXhalf, (float)g_wide_w / 2.0f);
}
static void wide_target_end(GLint uXoff, GLint uXhalf) {
    if (s_hiw)
        p_glUniform1f(uXoff == s_tex_uXoff ? s_tex_uShift : s_geo_uShift, s_shift_hr);
    p_glUniform1f(uXoff, 0.0f);
    p_glUniform1f(uXhalf, 512.0f);
    if (s_ws_ablate != 3)
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
}

/* Per-prim gate: stretch this prim iff native-wide + feature on AND the prim's
 * source address is inside the flower-field backdrop data structure (precise —
 * excludes the 3D rock/foreground, which is untagged AND has narrow prims so the
 * earlier tag/narrow heuristic tore it). mode!=0 falls back to the old
 * tag+narrow heuristic for A/B. */
static int bd_prim_gate(const int *xs, int n, int textured) {
    if (g_wide_w <= 0 || !g_ws_bd_stretch_on) return 0;
    /* Some games draw their authored 4:3 sky/water as flat-colour polygons.
     * Stretch those only in the native-wide mirror: the canonical framebuffer
     * remains byte-for-byte 4:3, while the flat backdrop reaches the reveal
     * margins. Opt-in because flat foreground geometry is title-dependent. */
    if (!textured && ctx_flat_backdrop()) return 1;
    if (g_ws_bd_phase_mode != 0) return ctx_prim_in_backdrop();  /* default: precise address gate */
    /* mode 0: legacy tag+narrow heuristic (kept for comparison) */
    int native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return 0;
    if (ctx_prim_tagged()) return 0;
    int base = g_wide_cur_base, lo = xs[0], hi = xs[0];
    for (int i = 1; i < n; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
    if (lo < base - g_ws_bd_phase_thresh) return 0;             /* into left margin -> GTE-wide */
    if (hi > base + native_w + g_ws_bd_phase_thresh) return 0;  /* into right margin -> GTE-wide */
    return 1;
}

/* ---- native-wide FAST path (skip redundant center mirror) ----------------- *
 * The wide surface's CENTRE columns [g_wide_off, g_wide_off+native_w) are, by
 * construction, identical to the canonical 4:3 framebuffer. So instead of
 * re-rasterizing every primitive into the wide surface (the "mirror" pass — the
 * dominant native-wide GPU cost, ~2x scene fill), we copy the canonical centre
 * into the wide surface once at present (blit_wide_center_from_canonical), and
 * the per-prim mirror only needs to produce the reveal MARGINS. Any prim/batch
 * whose x-range is fully inside the 4:3 frame contributes nothing to the margins,
 * so its mirror is skipped entirely. Correctness does not depend on the skip
 * being precise: the centre is authoritatively overwritten by the blit, so the
 * ONLY requirement is that a margin-reaching prim is NOT skipped — hence the
 * conservative strict-inside test. 4:3 never runs any of this (g_wide_cur == 0).
 * Toggle via gl_wide_fast for A/B; default ON. */
static int s_wide_fast = 1;
void gl_renderer_set_wide_fast(int on) {
    GL_RT_SYNC("set_wide_fast"); s_wide_fast = on ? 1 : 0; }
int  gl_renderer_get_wide_fast(void) { return s_wide_fast; }

/* PGXP renderer features (G1.14). Flags read at append time, so they can
 * flip between any two primitives. Like their neighbours the setters sync
 * with a live render thread first (GL_RT_SYNC), so a change never lands in
 * the middle of a replayed frame; they are safe at session_reboot. */
static int s_pgxp_render_wanted = 0;   /* any PGXP renderer feature on */
static void pgxp_render_wanted_update(void) {
    __atomic_store_n(&s_pgxp_render_wanted, (s_pgxp_depth || s_pgxp_cpersp || s_pgxp_seam) ? 1 : 0, __ATOMIC_RELEASE);
}
int gl_renderer_pgxp_render_wanted(void) {
    return __atomic_load_n(&s_pgxp_render_wanted, __ATOMIC_ACQUIRE);
}
void gl_renderer_set_pgxp_depth(int on) {
    GL_RT_SYNC("set_pgxp_depth");
    s_pgxp_depth = on ? 1 : 0;
    s_depth_need_clear = 1;
    s_pz_valid = 0;
    pgxp_render_wanted_update();
}
int  gl_renderer_get_pgxp_depth(void) { return s_pgxp_depth; }
void gl_renderer_set_pgxp_color_perspective(int on) {
    GL_RT_SYNC("set_pgxp_color_perspective");
    s_pgxp_cpersp = on ? 1 : 0;
    pgxp_render_wanted_update();
}
int  gl_renderer_get_pgxp_color_perspective(void) { return s_pgxp_cpersp; }
void gl_renderer_set_pgxp_seam(int mode) {
    GL_RT_SYNC("set_pgxp_seam");
    s_pgxp_seam = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    pgxp_render_wanted_update();
}
int  gl_renderer_get_pgxp_seam(void) { return s_pgxp_seam; }
void gl_renderer_set_pgxp_depth_threshold(float sz) {
    GL_RT_SYNC("set_pgxp_depth_threshold"); s_pgxp_depth_threshold = sz; }
void gl_renderer_pgxp_render_stats(uint64_t *depth_tris, uint64_t *depth_clears,
                                   uint64_t *seam_tris) {
    GL_RT_SYNC("pgxp_render_stats");
    if (depth_tris) *depth_tris = s_depth_tris;
    if (depth_clears) *depth_clears = s_depth_clears;
    if (seam_tris) *seam_tris = s_seam_tris;
}
static int wide_fast_center_valid(void) {
    /* An explicitly stretched sky differs inside the canonical viewport too.
     * Keep the full composite for those scenes, including later foreground
     * draws. Tags are installed before DMA, so no earlier center draws skip. */
    // The native split seam occupies canonical edge columns that the expanded
    // camera renders as world. Copying those columns reinstates the divider.
    return s_wide_fast && ctx_vp_width() <= 0 && !ctx_bg_full();
}
/* The one predicate for "the wide surface's centre columns [g_wide_off,
 * g_wide_off+native_w) are the canonical framebuffer, unshifted". A camera view
 * (glb_wide_set_view: anchored or shifted reveal) translates every world draw by
 * view_shift, so the canonical frame no longer lands at g_wide_off; the mirror
 * then draws the whole surface and no site may skip it or copy the canonical
 * frame over it. Every centre skip and every centre copy (present AND render
 * pass capture) must ask this, never wide_fast_center_valid() alone. */
static int wide_center_is_canonical(void) {
    return wide_fast_center_valid() && !view_enabled;
}
static void wide_blit_center(GLuint wide_fbo, int base_x, int disp_y, int disp_h); /* def below */
/* True if [lo,hi] (canonical draw-x) lies strictly inside the 4:3 frame, so the
 * prim adds nothing to either reveal margin and its mirror can be skipped. */
static int mirror_x_center_only(int lo, int hi) {
    if (!wide_center_is_canonical()) return 0;
    int base = g_wide_cur_base, native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return 0;
    return (lo >= base) && (hi < base + native_w);
}
static int mirror_geo_center_only(const int *xs, int n) {
    if (!wide_center_is_canonical()) return 0;
    int lo = xs[0], hi = xs[0];
    for (int i = 1; i < n; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
    return mirror_x_center_only(lo, hi);
}
/* mirror_batch_center_only (textured-batch variant) is defined after s_tb below. */

/* The 2D-backdrop x-stretch for a wide-mirror draw under gate `gate`. */
static void wide_bd_scale(int gate, float *out_scale, float *out_center) {
    extern int g_ws_tex_edge_pct;
    float scale = 1.0f, center = 0.0f;
    if (gate && g_ws_bd_stretch_on && g_wide_w > 0) {
        int native_w = g_wide_w - 2 * g_wide_off;
        if (native_w > 0) {
            scale  = g_ws_bd_stretch_pct > 0 ? (float)g_ws_bd_stretch_pct / 100.0f
                                             : (float)g_wide_w / (float)native_w;
            if (gate == 2) {
                scale = g_ws_tex_edge_pct > 0
                      ? (float)g_ws_tex_edge_pct / 100.0f : scale;
                scale = -scale; /* shader: expand only beyond canonical edges */
            }
            center = (float)g_wide_cur_base + (float)native_w / 2.0f;
        }
    }
    if (scale != 1.0f) s_bdg_applied++;
    *out_scale = scale;
    *out_center = center;
}

/* Set / clear the 2D-backdrop x-stretch for a wide-mirror draw, per the current
 * gate (s_bd_gate, set by the caller from bd_prim_gate / the batch gate). */
static void wide_set_bd_scale(GLint uScale, GLint uCenter) {
    float scale, center;
    wide_bd_scale(s_bd_gate, &scale, &center);
    p_glUniform1f(uScale, scale);
    p_glUniform1f(uCenter, center);
}
static void wide_clear_bd_scale(GLint uScale, GLint uCenter) {
    p_glUniform1f(uScale, 1.0f);
    p_glUniform1f(uCenter, 0.0f);
}

/* ---- textured-prim batching -------------------------------------------- *
 * Consecutive textured prims sharing blend/mask/texwindow/filter coalesce into
 * one draw. Per-prim texture state (texpage/clut/depth/raw/uv-limits/texture
 * window) rides in the vertex (TEXV flat attributes), so only `semi` (blend)
 * and the global mask/filter are batch keys, plus the texture window unless
 * texture-window batching is on (below). flush_tex_batch() draws the queued verts; it
 * is called before any op that reads VRAM, writes it outside the batch, or
 * changes batch state (see callers: flush_cpu_upload, flush_pack_if_sampling,
 * every non-textured glb_ wrapper, and the present path). Drawing reads only the
 * already-coherent texture (per-prim coherency was ensured at append time), so
 * flush_tex_batch never re-enters those helpers. */
#define TEXBATCH_MAXV 8190                 /* multiple of 3; ~2730 tris */
static float s_tb[TEXBATCH_MAXV * TEXV];
static int   s_tb_n = 0;                    /* verts queued */
static int   s_tb_semi = -2;
static int   s_tb_depth = 0;                /* PGXP depth mode (batch key) */
static int   s_tb_mask = 0, s_tb_filter = 0;
static GLuint s_tb_bank_tex;
static GLuint s_tb_hd_tex;

#define HD_GL_CACHE_CAP 64
#define HD_GL_CACHE_BUDGET (128u * 1024u * 1024u)
typedef struct HdGlTexture {
    uint64_t generation, key;
    uint64_t last_use;
    GLuint texture;
    size_t bytes;
} HdGlTexture;
static HdGlTexture s_hd_gl_cache[HD_GL_CACHE_CAP];
static size_t s_hd_gl_cache_bytes;
static uint64_t s_hd_gl_cache_clock;

void gl_renderer_clear_hd_texture_cache(void) {
    /* Pack (re)configuration: the render thread must be idle before the
     * session and its replacement textures change. */
    GL_RT_SYNC("clear_hd_texture_cache");
    flush_flat_batch(); flush_tex_batch(); hiw_flush_queue();
    for (int i = 0; i < HD_GL_CACHE_CAP; ++i) {
        if (s_ctx && s_hd_gl_cache[i].texture)
            glDeleteTextures(1, &s_hd_gl_cache[i].texture);
    }
    memset(s_hd_gl_cache, 0, sizeof(s_hd_gl_cache));
    s_hd_gl_cache_bytes = 0; s_tb_hd_tex = 0;
}
static GLuint hd_gl_texture(const GpuHdTextureImage* image) {
    if (!image->rgba || !image->width || !image->height || !image->source_width ||
        !image->source_height || image->stride != (uint64_t)image->width * 4u ||
        image->width > (uint32_t)s_gl_max_dim || image->height > (uint32_t)s_gl_max_dim)
        return 0;
    size_t bytes = (size_t)image->stride * image->height;
    if (bytes > HD_GL_CACHE_BUDGET) return 0;
    int free_slot = -1;
    for (int i = 0; i < HD_GL_CACHE_CAP; ++i) {
        HdGlTexture* texture = &s_hd_gl_cache[i];
        if (texture->texture && texture->key == image->cache_key &&
            texture->generation == image->generation) {
            texture->last_use = ++s_hd_gl_cache_clock; return texture->texture;
        }
        if (!texture->texture && free_slot < 0) free_slot = i;
    }
    while (free_slot < 0 || s_hd_gl_cache_bytes + bytes > HD_GL_CACHE_BUDGET) {
        int oldest = -1;
        for (int i = 0; i < HD_GL_CACHE_CAP; ++i)
            if (s_hd_gl_cache[i].texture && (oldest < 0 ||
                s_hd_gl_cache[i].last_use < s_hd_gl_cache[oldest].last_use)) oldest = i;
        if (oldest < 0) return 0;
        flush_flat_batch(); flush_tex_batch(); hiw_flush_queue();
        HdGlTexture* victim = &s_hd_gl_cache[oldest];
        glDeleteTextures(1, &victim->texture);
        s_hd_gl_cache_bytes -= victim->bytes;
        memset(victim, 0, sizeof(*victim)); free_slot = oldest;
    }
    /* A newly allocated image cannot affect pending draws, but TextureImage
     * changes GL bindings; their flush always binds its own complete state. */
    HdGlTexture* texture = &s_hd_gl_cache[free_slot];
    glGenTextures(1, &texture->texture);
    p_glActiveTexture(PSXGL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, texture->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->width, image->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, image->rgba);
    p_glActiveTexture(PSXGL_TEXTURE0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture->texture); texture->texture = 0; return 0;
    }
    texture->key = image->cache_key; texture->generation = image->generation;
    texture->last_use = ++s_hd_gl_cache_clock;
    texture->bytes = bytes; s_hd_gl_cache_bytes += bytes;
    return texture->texture;
}

static void bind_textured_resources(GLuint source, GLuint palette, GLuint hd) {
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source); p_glUniform1i(s_uVram, 0);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, palette); p_glUniform1i(s_uPalette, 1);
    p_glActiveTexture(PSXGL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, hd ? hd : s_up_tex); p_glUniform1i(s_uHdTexture, 2);
    p_glActiveTexture(PSXGL_TEXTURE0);
}
static int   s_tb_twin[4] = {0, 0, 0, 0};
static uint64_t s_batch_total = 0, s_batch_reason[8];

/* Texture-window batching ([video] texture_window_batching, OpenGL; off by
 * default). Off: a GP0(E2h) texture-window change ends the open textured batch
 * (batch reason 5), as before. On: prims with different windows share a batch,
 * since each vertex carries its own window. Games that tile far textures with
 * per-prim windows (R4's split screen: ~515 E2 changes and ~180 batches a game
 * frame, ~17 with this on) then draw in a tenth of the batches, which matters
 * most on the native-wide path, where a batch reaching the margins is drawn
 * again into the wide surface.
 * The result is pixel-identical: an opaque batch draws its colour in one
 * painter-ordered pass and semi-transparent prims are still drawn one per
 * batch. The one exception is mask checking (GP0(E6h) bit 1): an opaque batch
 * fixes the stencil for its STP=1 texels after its colour pass, so a later
 * prim in the same batch is not stopped by an earlier one's mask bit. The
 * window therefore stays a batch key while the check is on, and this mode
 * never makes a batch longer than the default mode would there. */
static int s_twin_batching = 0;
void gl_renderer_set_texture_window_batching(int on) {
    GL_RT_SYNC("set_texture_window_batching"); s_twin_batching = on ? 1 : 0; }
int  gl_renderer_get_texture_window_batching(void) { return s_twin_batching; }

void gl_renderer_batch_diag(uint64_t out[9]) {
    GL_RT_SYNC("batch_diag");
    out[0] = s_batch_total;
    for (int i = 0; i < 8; i++) out[i + 1] = s_batch_reason[i];
}

/* Draw the queued textured batch with correct PSX mask-bit handling AND correct
 * painter's order. The mask bit lives in both the colour-attachment alpha
 * (frag.a) and the stencil; STP=1 texels must always set it.
 *
 * OPAQUE batch (semi < 0): the STP bit does NOT gate COLOUR (every texel is
 * opaque), so colour is drawn in ONE ordered pass over all texels — frag.a still
 * carries the per-texel mask bit, so the alpha mask is correct — followed by a
 * COLOUR-MASKED pass that only fixes the STENCIL for STP=1 texels. The old code
 * split colour into STP=0 (pass 1) then STP=1 (pass 2) across the WHOLE batch,
 * which let a behind prim's STP=1 texels overwrite a front prim's STP=0 colour
 * (the Tomba character drew behind an AP-block's letters / a save post on GL
 * only). Same draw count, order preserved, mask preserved.
 *
 * SEMI batch (semi >= 0): genuine two-pass — STP=0 texels opaque, STP=1 texels
 * blended per the PSX mode. Cross-prim order is kept by isolating semi prims to
 * one per batch (see gpu_textured_triangle), so this batch holds a single prim
 * whose two passes do not self-overlap. */
/* tb_mask / check are the batch's set-mask and the mask-check state it was
 * drawn under; track_stencil lets the canonical pass record deferred stencil
 * (a replay into the high-resolution window must not touch that flag). */
static void tex_draw_passes_ex(int nverts, int semi, int tb_mask, int check,
                               int track_stencil) {
    p_glUniform1i(s_uSemimode, semi < 0 ? 0 : semi);
    if (semi < 0) {
        glDisable(GL_BLEND);
        mask_stencil_ex(tb_mask, check);
        p_glUniform1i(s_uSemipass, 0);                 /* all texels, one ordered colour pass */
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        if (check) {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);  /* stencil-only fixup */
            mask_stencil_ex(1, check);
            p_glUniform1i(s_uSemipass, 2);             /* STP=1 texels set the mask bit */
            glDrawArrays(GL_TRIANGLES, 0, nverts);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        } else if (!tb_mask) {
            /* Alpha is already exact; defer its duplicate stencil encoding
             * until a later GP0(E6h) actually enables destination masking. */
            if (track_stencil) s_stencil_valid = 0;
        }
    } else if (!check && semi == 4) {
        /* Modes 0/1/3 can select opaque-vs-semi behavior per fragment with
         * dual-source factors, so the whole painter-ordered batch is one draw.
         * Mode 2 needs a different blend equation and stays on the conservative
         * isolated path below. */
        glEnable(GL_BLEND);
        p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
        p_glBlendFuncSeparate(GL_ONE, PSXGL_SRC1_ALPHA, GL_ONE, GL_ZERO);
        if (tb_mask) mask_stencil_ex(1, check); else glDisable(GL_STENCIL_TEST);
        p_glUniform1i(s_uSemipass, 0);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        if (!tb_mask && track_stencil) s_stencil_valid = 0;
    } else {
        glDisable(GL_BLEND);                           /* Pass 1: STP=0 texels (opaque) */
        mask_stencil_ex(tb_mask, check);
        p_glUniform1i(s_uSemipass, 1);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        apply_psx_blend(semi);                         /* Pass 2: STP=1 texels (blended) */
        mask_stencil_ex(1, check);
        p_glUniform1i(s_uSemipass, 2);
        glDrawArrays(GL_TRIANGLES, 0, nverts);
    }
}
static void tex_batch_draw_passes(int nverts, int semi) {
    tex_draw_passes_ex(nverts, semi, s_tb_mask, s_mask_check, 1);
}

/* ---- frame_perf CPU attribution (native-wide wedge hunt) ----------------- *
 * Per-frame CPU wall time spent inside the GL submission paths (driver CPU
 * cost surfaces INSIDE our gl* calls) + counters for the wide plumbing, so a
 * CPU-bound wide frame (emu_cpu >> scene_gpu) can be attributed without a
 * sampling profiler. Reset at present enter; reported by frame_perf. */
static double cw_ms(void) {
    static double freq = 0.0;
    if (freq == 0.0) {
        uint64_t f = SDL_GetPerformanceFrequency();
        freq = f ? (double)f : 1.0;
    }
    return (double)SDL_GetPerformanceCounter() * 1000.0 / freq;
}
static double s_cw_flush_ms = 0.0;   /* CPU wall inside flush_tex_batch        */
static double s_cw_wide_ms  = 0.0;   /* CPU wall inside glb_wide_* entry points */
static int    s_cw_batches = 0, s_cw_wide_sets = 0, s_cw_wide_cfgs = 0,
              s_cw_wide_clears = 0, s_cw_fbo_creates = 0, s_cw_flush_depth = 0;

/* Textured-batch variant of mirror_x_center_only: scan the queued verts' x
 * (attr 0, stride TEXV). Defined here so s_tb / TEXV are in scope. */
static int mirror_batch_center_only(int nverts) {
    if (!wide_center_is_canonical() || nverts <= 0) return 0;
    float flo = s_tb[0], fhi = s_tb[0];
    for (int i = 1; i < nverts; i++) {
        float x = s_tb[i * TEXV];
        if (x < flo) flo = x; if (x > fhi) fhi = x;
    }
    /* floor/ceil, not a truncating cast: with geometry_correction these are
     * fractional, and (int) rounds toward zero — which WIDENS a negative x
     * toward the centre and could wrongly call a margin-touching batch
     * centre-only, dropping its wide-mirror draw. Whole values are unchanged. */
    return mirror_x_center_only((int)floorf(flo), (int)ceilf(fhi));
}

/* ---- windowed high-resolution surface: deferred mirror queue ------------
 * Mirroring each batch into the window as it is flushed switches framebuffers
 * twice per batch, and on Apple's GL (Metal underneath) every switch ends a
 * render pass: about 80 us each, ~10 ms per R4 frame. So the window's copies
 * of textured / flat / line draws are queued (vertices + the state they were
 * drawn with) and replayed into the window in ONE pass at the next sync
 * point: anything that changes what they sample (the raw mirror: pack,
 * uploads, the depth24 clear), any other write to the window (fill, copy,
 * upload, stencil rebuild), and every read of it (present, capture, growth).
 * Order within the window is preserved; its content equals an immediate
 * mirror at every point it is observed.
 * The native-wide mirror of the same draws switched to the wide surface and
 * back per batch as well; at 8K that surface is up to 15372x9216 and the
 * switches held a 21:9 race to ~30 frames/s. In windowed mode a draw's wide
 * mirror rides in its queue entry (wfbo: target surface, wdx: its x shift,
 * wscale/wcenter: its backdrop stretch) and is replayed in the same flush,
 * one pass per wide surface. Every other write to a wide surface (clears,
 * the full-screen overlay rect, reallocation) and every read of one
 * (present, capture, dumps) flushes the queue first. */
/* The same queue carries the native-wide mirrors when the window is not
 * engaged (wide_queue_live): at a 10x Match display allocation each per-batch
 * switch to a wide surface and back ended two render passes over surfaces
 * 4270x5120 (wide) and 10240x5120 (hr) and held a 16:9 R4 race to ~47
 * frames/s; queued, the mirrors land in one pass per wide surface at the same
 * sync points. Lines drawn as GL_LINES (1x) are not queued; they flush the
 * queue and mirror at once. PSX_GL_WIDE_QUEUE=0 turns the queue off. */
enum { HQ_TEX = 1, HQ_GEO = 2 };
typedef struct {
    uint8_t kind;
    int8_t  semi;            /* tex: batch semi key; geo: blend mode, -1 opaque */
    uint8_t mask;            /* tex: batch set-mask; geo: stencil write value */
    uint8_t check;           /* mask-check state at draw time */
    uint8_t filter;
    GLuint  tex;             /* sampled texture (bank or raw mirror) */
    GLuint  palette, hd_tex;
    int     ax0, ay0, ax1, ay1;  /* draw area, inclusive */
    size_t  vfirst;          /* float offset into s_hq_v */
    int     vcount;          /* vertices */
    GLuint  wfbo;            /* native-wide mirror target, 0: none */
    int     wdx;             /* its x shift (wide_dx()) */
    float   wscale, wcenter; /* its backdrop stretch (wide_bd_scale) */
    int     wsx, wsy, wsw, wsh;  /* its wide-surface scissor (native px) */
    uint8_t depth;           /* PGXP depth mode (depth_apply) */
} HiCmd;
static int s_hq_depth_cur = 0;   /* depth mode of the batch being queued */
static HiCmd  *s_hq = NULL;
static int     s_hq_n = 0, s_hq_cap = 0;
static float  *s_hq_v = NULL;
static size_t  s_hq_vn = 0, s_hq_vcap = 0;
static uint64_t s_hq_flushes = 0, s_hq_cmds = 0;

static HiCmd *hiw_enqueue(int kind, const float *verts, int nverts, int stride) {
    size_t nf = (size_t)nverts * (size_t)stride;
    if (s_hq_n == s_hq_cap) {
        int cap = s_hq_cap ? s_hq_cap * 2 : 256;
        HiCmd *q = (HiCmd *)realloc(s_hq, (size_t)cap * sizeof(HiCmd));
        if (!q) return NULL;
        s_hq = q; s_hq_cap = cap;
    }
    if (s_hq_vn + nf > s_hq_vcap) {
        size_t cap = s_hq_vcap ? s_hq_vcap : 65536;
        while (cap < s_hq_vn + nf) cap *= 2;
        float *v = (float *)realloc(s_hq_v, cap * sizeof(float));
        if (!v) return NULL;
        s_hq_v = v; s_hq_vcap = cap;
    }
    HiCmd *c = &s_hq[s_hq_n++];
    memset(c, 0, sizeof *c);
    c->kind = (uint8_t)kind;
    c->vfirst = s_hq_vn;
    c->vcount = nverts;
    memcpy(s_hq_v + s_hq_vn, verts, nf * sizeof(float));
    s_hq_vn += nf;
    c->check = (uint8_t)s_mask_check;
    c->depth = (uint8_t)s_hq_depth_cur;
    c->ax0 = s_area_x1; c->ay0 = s_area_y1; c->ax1 = s_area_x2; c->ay1 = s_area_y2;
    s_hq_cmds++;
    return c;
}

/* The draw area intersects a tile at all (else nothing to mirror). */
static int hiw_area_touches(void) {
    if (s_area_y2 < s_area_y1) return 0;
    for (int t = 0; t < s_hiw_n; t++)
        if (s_area_x2 >= s_hiw_t[t].x0 && s_area_x1 < s_hiw_t[t].x1) return 1;
    return 0;
}

/* A draw's native-wide mirror goes in its queue entry when the window's
 * queue is live and the mirror is not being ablated (a perf probe). */
static int hiw_wide_queue_ok(int mirror) {
    return mirror && g_wide_cur && s_ws_ablate == 0;
}
static int s_wide_queue = -1;   /* PSX_GL_WIDE_QUEUE (default on) */
static int wide_queue_live(void) {
    if (s_wide_queue < 0) {
        const char *e = getenv("PSX_GL_WIDE_QUEUE");
        s_wide_queue = !(e && e[0] == '0');
    }
    return s_wide_queue && !s_hiw && g_wide_cur && s_ws_ablate == 0;
}
static void wide_band_rows(int *sy, int *sh);
static void hiw_wide_set(HiCmd *c, int gate) {
    c->wfbo = g_wide_cur;
    c->wdx = wide_dx();
    wide_bd_scale(gate, &c->wscale, &c->wcenter);
    if (s_hiw) {   /* the window's replay: full width, the draw area's rows */
        int sy = c->ay0, sh = c->ay1 - c->ay0 + 1;
        if (sy < 0) { sh += sy; sy = 0; }
        if (sy + sh > VRAM_H) sh = VRAM_H - sy;
        if (sh < 0) sh = 0;
        c->wsx = 0; c->wsw = g_wide_w; c->wsy = sy; c->wsh = sh;
    } else {       /* what wide_target_begin sets */
        c->wsx = view_pad_left;
        c->wsw = g_wide_w - view_pad_left - view_pad_right;
        wide_band_rows(&c->wsy, &c->wsh);
    }
}

/* Queue a textured batch for the window, and its native-wide mirror when
 * `mirror` (gate: its backdrop-stretch gate). Returns 1 when the mirror was
 * queued (the caller then skips its immediate mirror). */
static int hiw_enqueue_tex(int nverts, int semi, int mirror, int gate) {
    int wq = hiw_wide_queue_ok(mirror);
    if (!wq && !hiw_area_touches()) return 0;
    HiCmd *c = hiw_enqueue(HQ_TEX, s_tb, nverts, TEXV);
    if (!c) { hiw_flush_queue(); return 0; }
    c->semi = (int8_t)semi;
    c->mask = (uint8_t)s_tb_mask;
    c->filter = (uint8_t)s_tb_filter;
    c->tex = s_tb_bank_tex ? s_tb_bank_tex : s_raw_tex;
    c->palette = s_tb_bank_tex && !s_tb_bank_live_clut ? s_tb_bank_tex : s_raw_tex;
    c->hd_tex = s_tb_hd_tex;
    if (wq) hiw_wide_set(c, gate);
    return wq;
}

static int hiw_enqueue_geo(const float *verts, int nverts, int semi, int mask,
                           int mirror, int gate) {
    int wq = hiw_wide_queue_ok(mirror);
    if (!wq && !hiw_area_touches()) return 0;
    HiCmd *c = hiw_enqueue(HQ_GEO, verts, nverts, 6);
    if (!c) { hiw_flush_queue(); return 0; }
    c->semi = (int8_t)semi;
    c->mask = (uint8_t)(mask ? 1 : 0);
    if (wq) hiw_wide_set(c, gate);
    return wq;
}

/* Replay the queued native-wide mirrors, in order, into their wide surfaces:
 * the state wide_target_begin sets (full-width scissor, the draw area's rows,
 * the S sample-grid shift) plus each entry's shift and backdrop stretch. */
static void hiw_replay_wide(void) {
    int S = s_out_scale, any = 0, cur = 0;
    GLuint bound = 0;
    for (int i = 0; i < s_hq_n; i++) {
        const HiCmd *c = &s_hq[i];
        if (!c->wfbo) continue;
        if (!any) { gl_perf_mirror_begin(); any = 1; }
        if (c->wfbo != bound) {
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, c->wfbo);
            glViewport(0, 0, g_wide_w * S, VRAM_H * S);
            glEnable(GL_SCISSOR_TEST);
            p_glUseProgram(s_geo_prog);
            p_glUniform1f(s_geo_uShift, s_shift_hi);
            p_glUniform1f(s_geo_uXhalf, (float)g_wide_w / 2.0f);
            p_glUseProgram(s_tex_prog);
            p_glUniform1f(s_tex_uShift, s_shift_hi);
            p_glUniform1f(s_tex_uXhalf, (float)g_wide_w / 2.0f);
            p_glUniform1i(s_uVram, 0);
            p_glUniform1i(s_uPalette, 1);
            p_glActiveTexture(PSXGL_TEXTURE0);
            bound = c->wfbo;
            cur = 0;
        }
        glScissor(c->wsx * S, c->wsy * S, c->wsw * S, c->wsh * S);
        if (c->kind == HQ_TEX) {
            if (cur != HQ_TEX) {
                p_glUseProgram(s_tex_prog);
                p_glBindVertexArray(s_tex_vao);
                p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
                cur = HQ_TEX;
            }
            p_glUniform1f(s_tex_uXoff, (float)c->wdx);
            p_glUniform1f(s_tex_uXscale, c->wscale);
            p_glUniform1f(s_tex_uXcenter, c->wcenter);
            bind_textured_resources(c->tex, c->palette, c->hd_tex);
            p_glUniform1i(s_uMaskset, c->mask);
            p_glUniform1i(s_uFilter, c->filter);
            p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)((size_t)c->vcount * TEXV * sizeof(float)),
                           s_hq_v + c->vfirst, PSXGL_STREAM_DRAW);
            depth_apply_ex(c->depth, 1);
            tex_draw_passes_ex(c->vcount, c->semi, c->mask, c->check, 0);
            depth_restore_ex(c->depth, 1, c->vcount);
        } else {
            if (cur != HQ_GEO) {
                p_glUseProgram(s_geo_prog);
                p_glBindVertexArray(s_geo_vao);
                p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
                cur = HQ_GEO;
            }
            p_glUniform1f(s_geo_uXoff, (float)c->wdx);
            p_glUniform1f(s_geo_uXscale, c->wscale);
            p_glUniform1f(s_geo_uXcenter, c->wcenter);
            if (c->semi >= 0) apply_psx_blend(c->semi); else glDisable(GL_BLEND);
            mask_stencil_ex(c->mask, c->check);
            p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)((size_t)c->vcount * 6 * sizeof(float)),
                           s_hq_v + c->vfirst, PSXGL_STREAM_DRAW);
            depth_apply_ex(c->depth, 0);
            glDrawArrays(GL_TRIANGLES, 0, c->vcount);
            depth_restore_ex(c->depth, 0, c->vcount);
        }
    }
    if (!any) return;
    p_glUseProgram(s_geo_prog);
    p_glUniform1f(s_geo_uXscale, 1.0f);
    p_glUniform1f(s_geo_uXcenter, 0.0f);
    p_glUseProgram(s_tex_prog);
    p_glUniform1f(s_tex_uXscale, 1.0f);
    p_glUniform1f(s_tex_uXcenter, 0.0f);
    gl_perf_mirror_end();
}

/* Replay the queued draws into every tile they touch: the window projection
 * and each command's draw area (clipped to the tile) as the scissor. A tile no
 * command touches is not bound. */
static void hiw_flush_tail(void);
static void hiw_flush_queue(void) {
    if (s_hq_n == 0) return;
    if (!hiw_on()) {   /* native-wide mirrors only (wide_queue_live) */
        s_hq_flushes++;
        hiw_flush_tail();
        return;
    }
    int S = s_out_scale;
    s_hq_flushes++;
    for (int t = 0; t < s_hiw_n; t++) {
        const HiwTile *T = &s_hiw_t[t];
        int ww = T->x1 - T->x0, bound = 0, cur = 0;
        for (int i = 0; i < s_hq_n; i++) {
            const HiCmd *c = &s_hq[i];
            int cx0 = c->ax0 > T->x0 ? c->ax0 : T->x0;
            int cx1 = c->ax1 + 1 < T->x1 ? c->ax1 + 1 : T->x1;
            int cy0 = c->ay0 < 0 ? 0 : c->ay0;
            int cy1 = c->ay1 + 1 > VRAM_H ? VRAM_H : c->ay1 + 1;
            if (cx1 <= cx0 || cy1 <= cy0) continue;   /* (a wide-only entry) */
            if (!bound) {
                p_glBindFramebuffer(PSXGL_FRAMEBUFFER, T->fbo);
                glViewport(0, 0, ww * S, VRAM_H * S);
                glEnable(GL_SCISSOR_TEST);
                /* The tile projection on both programs for its replay. */
                p_glUseProgram(s_geo_prog);
                p_glUniform1f(s_geo_uShift, s_shift_hi);
                p_glUniform1f(s_geo_uXoff, (float)(-T->x0));
                p_glUniform1f(s_geo_uXhalf, (float)ww / 2.0f);
                p_glUseProgram(s_tex_prog);
                p_glUniform1f(s_tex_uShift, s_shift_hi);
                p_glUniform1f(s_tex_uXoff, (float)(-T->x0));
                p_glUniform1f(s_tex_uXhalf, (float)ww / 2.0f);
                p_glUniform1i(s_uVram, 0);
                p_glActiveTexture(PSXGL_TEXTURE0);
                bound = 1;
            }
            glScissor((cx0 - T->x0) * S, cy0 * S, (cx1 - cx0) * S, (cy1 - cy0) * S);
            if (c->kind == HQ_TEX) {
                if (cur != HQ_TEX) {
                    p_glUseProgram(s_tex_prog);
                    p_glBindVertexArray(s_tex_vao);
                    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
                    cur = HQ_TEX;
                }
                bind_textured_resources(c->tex, c->palette, c->hd_tex);
                p_glUniform1i(s_uMaskset, c->mask);
                p_glUniform1i(s_uFilter, c->filter);
                p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)((size_t)c->vcount * TEXV * sizeof(float)),
                               s_hq_v + c->vfirst, PSXGL_STREAM_DRAW);
                depth_apply_ex(c->depth, 1);
                tex_draw_passes_ex(c->vcount, c->semi, c->mask, c->check, 0);
                depth_restore_ex(c->depth, 1, c->vcount);
            } else {
                if (cur != HQ_GEO) {
                    p_glUseProgram(s_geo_prog);
                    p_glBindVertexArray(s_geo_vao);
                    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
                    cur = HQ_GEO;
                }
                if (c->semi >= 0) apply_psx_blend(c->semi); else glDisable(GL_BLEND);
                mask_stencil_ex(c->mask, c->check);
                p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)((size_t)c->vcount * 6 * sizeof(float)),
                               s_hq_v + c->vfirst, PSXGL_STREAM_DRAW);
                depth_apply_ex(c->depth, 0);
                glDrawArrays(GL_TRIANGLES, 0, c->vcount);
                depth_restore_ex(c->depth, 0, c->vcount);
            }
        }
    }
    hiw_flush_tail();
}

/* The native-wide mirrors (both modes), then the canonical projection back
 * on both programs. */
static void hiw_flush_tail(void) {
    hiw_replay_wide();
    /* Canonical projection back on both programs. */
    p_glUseProgram(s_geo_prog);
    p_glUniform1f(s_geo_uShift, s_shift_hr);
    p_glUniform1f(s_geo_uXoff, 0.0f);
    p_glUniform1f(s_geo_uXhalf, 512.0f);
    p_glUseProgram(s_tex_prog);
    p_glUniform1f(s_tex_uShift, s_shift_hr);
    p_glUniform1f(s_tex_uXoff, 0.0f);
    p_glUniform1f(s_tex_uXhalf, 512.0f);
    hr_end();
    /* Native-wide only: leave the hr surface bound, as an immediate mirror
     * (wide_target_end) does. */
    if (!s_hiw) p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    s_hq_n = 0;
    s_hq_vn = 0;
}

static void flush_tex_batch(void) {
    if (s_tb_n == 0) return;
    wide_stencil_ready();   /* before any of this batch's GL state */
    int nverts = s_tb_n, semi = s_tb_semi, dmode = s_tb_depth;
    s_tb_n = 0;                             /* clear first: re-entrancy safe */
    double cw_t0 = cw_ms();
    s_cw_batches++; s_batch_total++; s_cw_flush_depth++;

    hr_begin(1);
    p_glUseProgram(s_tex_prog);
    bind_textured_resources(s_tb_bank_tex ? s_tb_bank_tex : s_raw_tex,
        s_tb_bank_tex && !s_tb_bank_live_clut ? s_tb_bank_tex : s_raw_tex, s_tb_hd_tex);
    p_glUniform1i(s_uMaskset, s_tb_mask);
    p_glUniform1i(s_uFilter, s_tb_filter);
    p_glBindVertexArray(s_tex_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(nverts * TEXV * sizeof(float)), s_tb, PSXGL_STREAM_DRAW);

    depth_apply_ex(dmode, 1);
    tex_batch_draw_passes(nverts, semi);
    depth_restore_ex(dmode, 1, nverts);

    /* Native-wide mirror — skipped for a batch fully inside the 4:3 frame (its
     * centre content comes from the present-time canonical blit; nothing to add
     * to the margins). A backdrop-stretched batch (s_tb_gate) widens past the
     * frame, so it is never treated as centre-only. */
    int mirror = g_wide_cur && s_ws_ablate != 1 &&
                 !(s_tb_gate == 0 && mirror_batch_center_only(nverts));
    if (mirror) {   /* the stencil it may leave behind alpha */
        float sc, ce;
        wide_bd_scale(s_tb_gate, &sc, &ce);
        wst_note_draw(g_wide_cur, s_tb, nverts, TEXV, wide_dx(), sc, ce, s_mask_check);
    }
    /* Windowed high-resolution surface: the same batch at S, queued and
     * replayed in one render pass at the next sync point (see s_hq), with its
     * native-wide mirror. No-op unless that mode is engaged. */
    s_hq_depth_cur = dmode;
    if ((hiw_on() || (mirror && wide_queue_live())) &&
        hiw_enqueue_tex(nverts, semi, mirror, s_tb_gate)) mirror = 0;
    s_hq_depth_cur = 0;

    if (mirror) {   /* native-wide mirror */
        int dx = wide_dx();
        s_bd_gate = s_tb_gate;              /* this batch is uniform-gate (flushed on change) */
        gl_perf_mirror_begin();
        wide_target_begin(dx, s_tex_uXoff, s_tex_uXhalf);
        wide_set_bd_scale(s_tex_uXscale, s_tex_uXcenter);
        depth_apply_ex(dmode, 1);
        if (s_ws_ablate != 2) tex_batch_draw_passes(nverts, semi);
        depth_restore_ex(dmode, 1, nverts);
        wide_clear_bd_scale(s_tex_uXscale, s_tex_uXcenter);
        wide_target_end(s_tex_uXoff, s_tex_uXhalf);
        gl_perf_mirror_end();
    }
    hr_end();
    if (--s_cw_flush_depth == 0) s_cw_flush_ms += cw_ms() - cw_t0;
}

/* Flat / gouraud GEO batch — MotK title/char-select starfields issue ~30k/s
 * GP0(68h) 1x1 dots; each was two immediate gpu_triangle draws (BufferData +
 * DrawArrays each). Coalesce opaque/semi-uniform tris into one draw. */
#define FLATBATCH_MAXV 8190                 /* multiple of 3 */
#define FLATBATCH_MAXL (FLATBATCH_MAXV / 6)   /* line quads the batch can hold */
/* The batch, then room for its windowed lines' GL_LINES vertices (s_fbl),
 * which flush_flat_batch uploads right after it. */
static float s_fb[(FLATBATCH_MAXV + 2 * FLATBATCH_MAXL) * 6];
static int   s_fb_n = 0;
static int   s_fb_semi = -2;
static int   s_fb_depth = 0;                /* PGXP depth mode (batch key) */
static int   s_fb_mask = -1;
/* GL_TRIANGLES, or GL_LINES for a batch of native-wide lines (gpu_geometry). */
static GLenum s_fb_mode = GL_TRIANGLES;
/* Windowed high-resolution mode (s_hiw): the hr surface is the 1x
 * authoritative VRAM, where a line stays GL_LINES, while the window and the
 * wide surfaces draw its one-native-pixel quad. A batched line therefore
 * carries two vertex sets: its quad in s_fb with the triangles (everything
 * the S surfaces draw) and its two GL_LINES vertices in s_fbl, s_fbl_at[i]
 * being the s_fb vertex line i's quad starts at, so the 1x draw
 * (flat_batch_draw_hr_lines) keeps painter order. Empty outside windowed
 * mode. */
static float s_fbl[FLATBATCH_MAXL * 2 * 6];
static int   s_fbl_at[FLATBATCH_MAXL];
static int   s_fbl_n = 0;
static int   s_fb_gate = 0;

static int mirror_flat_batch_center_only(int nverts) {
    if (!wide_center_is_canonical() || nverts <= 0) return 0;
    float flo = s_fb[0], fhi = s_fb[0];
    for (int i = 1; i < nverts; i++) {
        float x = s_fb[i * 6];
        if (x < flo) flo = x; if (x > fhi) fhi = x;
    }
    /* floor/ceil rather than a truncating cast — see mirror_batch_center_only. */
    return mirror_x_center_only((int)floorf(flo), (int)ceilf(fhi));
}

/* A batch holding windowed lines (nl > 0) on the 1x hr surface, in painter
 * order as runs: triangles from s_fb, lines as GL_LINES from the s_fbl copy
 * uploaded after the batch's nverts vertices. */
static void flat_batch_draw_hr_lines(int nverts, int nl) {
    glLineWidth((float)s_hr_scale);
    int pos = 0;
    for (int i = 0; i < nl;) {
        int j = i + 1;   /* lines i..j-1 are back to back in the batch */
        while (j < nl && s_fbl_at[j] == s_fbl_at[j - 1] + 6) j++;
        if (s_fbl_at[i] > pos) glDrawArrays(GL_TRIANGLES, pos, s_fbl_at[i] - pos);
        glDrawArrays(GL_LINES, nverts + 2 * i, 2 * (j - i));
        pos = s_fbl_at[j - 1] + 6;
        i = j;
    }
    if (nverts > pos) glDrawArrays(GL_TRIANGLES, pos, nverts - pos);
}

static void flush_flat_batch(void) {
    if (s_fb_n == 0) return;
    wide_stencil_ready();   /* before any of this batch's GL state */
    if (s_fb_mode != GL_TRIANGLES && !hiw_on())
        hiw_flush_queue();  /* GL_LINES mirror at once: queued mirrors first */
    int nverts = s_fb_n, semi = s_fb_semi, mask = s_fb_mask, nl = s_fbl_n;
    int dmode = s_fb_depth;
    GLenum fmode = s_fb_mode;
    s_fb_n = 0;
    s_fbl_n = 0;

    hr_begin(1);
    if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
    mask_stencil(mask);
    if (fmode == GL_LINES) glLineWidth((float)s_hr_scale);
    p_glUseProgram(s_geo_prog);
    p_glBindVertexArray(s_geo_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    if (nl) memcpy(&s_fb[nverts * 6], s_fbl, (size_t)nl * 2 * 6 * sizeof(float));
    p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)((nverts + 2 * nl) * 6 * sizeof(float)),
                   s_fb, PSXGL_STREAM_DRAW);
    depth_apply_ex(dmode, 0);
    if (nl) flat_batch_draw_hr_lines(nverts, nl);
    else glDrawArrays(fmode, 0, nverts);
    depth_restore_ex(dmode, 0, nverts);
    int mirror = g_wide_cur && !s_wide_suppress && s_ws_ablate != 1 &&
                 !(!g_ws_bd_stretch_on && mirror_flat_batch_center_only(nverts));
    if (mirror) {
        float sc, ce;
        wide_bd_scale(s_fb_gate, &sc, &ce);
        wst_note_draw(g_wide_cur, s_fb, nverts, 6, wide_dx(), sc, ce, s_mask_check);
    }
    s_hq_depth_cur = dmode;
    if ((hiw_on() || (mirror && fmode == GL_TRIANGLES && wide_queue_live())) &&
        hiw_enqueue_geo(s_fb, nverts, semi, mask, mirror, s_fb_gate)) mirror = 0;
    s_hq_depth_cur = 0;

    if (mirror) {
        int dx = wide_dx();
        s_bd_gate = s_fb_gate;
        gl_perf_mirror_begin();
        wide_target_begin(dx, s_geo_uXoff, s_geo_uXhalf);
        wide_set_bd_scale(s_geo_uXscale, s_geo_uXcenter);
        depth_apply_ex(dmode, 0);
        if (s_ws_ablate != 2) glDrawArrays(fmode, 0, nverts);
        depth_restore_ex(dmode, 0, nverts);
        wide_clear_bd_scale(s_geo_uXscale, s_geo_uXcenter);
        wide_target_end(s_geo_uXoff, s_geo_uXhalf);
        gl_perf_mirror_end();
    }
    hr_end();
}

/* A PS1 line as a quad one NATIVE pixel thick (used at S > 1).
 *
 * glLineWidth(S) was the old way to keep a line's thickness at internal scale,
 * but core-profile contexts only guarantee width 1 (macOS reports
 * GL_ALIASED_LINE_WIDTH_RANGE 1..1 and rejects anything wider), so every line
 * came out one HR pixel thick: 1/9 of native at S=9. The quad follows the PS1
 * rule instead: one pixel per step along the major axis, one pixel across the
 * minor axis. It spans pixel centres p0..p1 extended half a pixel along the
 * major axis at both ends (so both endpoint pixels are covered) and offset
 * +-0.5 px across the minor axis. Positions are native VRAM px, the space the
 * GEO shader already takes. v = two (x, y, r, g, b, a) vertices; q = six. */
static void line_to_quad(const float *v, float *q) {
    float x0 = v[0] + 0.5f, y0 = v[1] + 0.5f;
    float x1 = v[6] + 0.5f, y1 = v[7] + 0.5f;
    float dx = x1 - x0, dy = y1 - y0;
    float ax = fabsf(dx), ay = fabsf(dy);
    float ex, ey, nx, ny;      /* half-pixel extension along, offset across */
    if (ax >= ay) {            /* X-major (also the single-pixel case) */
        float sx = dx < 0.0f ? -1.0f : 1.0f;
        ex = 0.5f * sx; ey = ax > 0.0f ? 0.5f * sx * dy / dx : 0.0f;
        nx = 0.0f; ny = 0.5f;
    } else {                   /* Y-major */
        float sy = dy < 0.0f ? -1.0f : 1.0f;
        ey = 0.5f * sy; ex = 0.5f * sy * dx / dy;
        nx = 0.5f; ny = 0.0f;
    }
    const float a[2] = { x0 - ex, y0 - ey }, b[2] = { x1 + ex, y1 + ey };
    /* corners: a-, b-, a+ / b-, a+, b+ ; colour follows the nearer endpoint */
    const float cx[6] = { a[0]-nx, b[0]-nx, a[0]+nx, b[0]-nx, a[0]+nx, b[0]+nx };
    const float cy[6] = { a[1]-ny, b[1]-ny, a[1]+ny, b[1]-ny, a[1]+ny, b[1]+ny };
    const int   end[6] = { 0, 1, 0, 1, 0, 1 };
    for (int i = 0; i < 6; i++) {
        const float *src = v + end[i] * 6;
        q[i*6+0] = cx[i]; q[i*6+1] = cy[i];
        q[i*6+2] = src[2]; q[i*6+3] = src[3]; q[i*6+4] = src[4]; q[i*6+5] = src[5];
    }
}

/* Flat / gouraud triangles and lines share the GEO program. mode: GL_TRIANGLES
 * or GL_LINES; verts are (x, y, r, g, b, a) tuples with colors as 1555. */

/* ---- PGXP depth buffer, perspective colour, seam expansion (G1.14) -------
 * A triangle is "3D" here when gpu.c proved all three vertices from GTE
 * dataflow shadows: sub-pixel positions (s_pc_valid) and their SZ
 * (s_pz_valid). Everything else (2D, HUD, sprites, CPU-built or unproven
 * polygons) draws exactly as before: no depth test, no depth write. */
static int pgxp_tri_is_3d(void) { return s_pc_valid && s_pz_valid; }
/* PSX_PGXP_TRI_LOG=<file> (diagnostic): while <file>.on exists, every
 * triangle is logged (kind, depth mode, x y sz per vertex) and every depth
 * clear as "C". */
static FILE *pgxp_tri_log(void) {
    static int init = 0; static char path[400]; static FILE *f = NULL;
    if (!init) { init = 1; const char *e = getenv("PSX_PGXP_TRI_LOG"); if (e && *e) snprintf(path, sizeof path, "%s", e); }
    if (!path[0]) return NULL;
    char on[420]; snprintf(on, sizeof on, "%s.on", path);
    FILE *t = fopen(on, "r"); if (!t) { if (f) { fclose(f); f = NULL; } return NULL; } fclose(t);
    if (!f) f = fopen(path, "a");
    return f;
}
static void pgxp_tri_log_tri(char kind, int dmode, int semi, const float *v, int stride) {
    FILE *f = pgxp_tri_log(); if (!f) return;
    fprintf(f, "%c %d %d %d", kind, dmode, semi, s_pz_valid);
    for (int i = 0; i < 3; i++)
        fprintf(f, " %.3f %.3f %.1f", v[i * stride], v[i * stride + 1], s_pz_valid ? s_pz[i] : 0.0f);
    fprintf(f, " area %d %d %d %d\n", s_area_x1, s_area_y1, s_area_x2, s_area_y2);
}
/* Depth mode of the next triangle: opaque 3D polygons only (DuckStation's
 * default; semi-transparent polygons neither test nor write). */
/* Near-camera triangles (any vertex SZ below s_pgxp_depth_near) stay out of
 * the depth buffer: there SZ quantisation and R4's near subdivision make a
 * road decal's depth disagree with its road by more than any sane tolerance
 * (the lane dash cut off at the bottom of the screen), and painter order is
 * what the game was built for. PSX_PGXP_DEPTH_NEAR tunes it (0 = off). */
static float s_pgxp_depth_near = -1.0f;
static int pgxp_tri_depth_mode(int semi) {
    if (!(s_pgxp_depth && semi < 0 && pgxp_tri_is_3d())) return 0;
    if (s_pgxp_depth_near < 0.0f) {
        const char *e = getenv("PSX_PGXP_DEPTH_NEAR");
        s_pgxp_depth_near = e && *e ? (float)atof(e) : 1024.0f;
    }
    const float zmin = fminf(s_pz[0], fminf(s_pz[1], s_pz[2]));
    return zmin >= s_pgxp_depth_near ? 1 : 0;
}
/* Per-vertex code the shaders decode: SZ (0 = none) plus 65536 when the
 * vertex's Gouraud colour interpolates perspective-correct. */
static float pgxp_vertex_code(float sz) {
    if (!s_pz_valid || sz <= 0.0f) return 0.0f;
    return sz + (s_pgxp_cpersp ? 65536.0f : 0.0f);
}

/* The depth buffer is cleared (beyond every SZ, over the drawing area and
 * the native-wide margins) by a colourless depth-only draw through the flat
 * batch, so every surface a draw reaches (the hr surface, the high-resolution
 * window tiles, the wide surfaces) clears in painter order with it. */
static void depth_clear_now(void) {
    flush_flat_batch();
    flush_tex_batch();
    const float x0 = (float)s_area_x1 - 1024.0f, x1 = (float)s_area_x2 + 1.0f + 1024.0f;
    const float y0 = (float)s_area_y1, y1 = (float)s_area_y2 + 1.0f;
    const float q[6][2] = { {x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1} };
    s_fb_mode = GL_TRIANGLES; s_fb_semi = -1; s_fb_mask = (int)s_mask_set;
    s_fb_gate = 0; s_fb_depth = 2;
    for (int i = 0; i < 6; i++) {
        float *v = &s_fb[i * 6];
        v[0] = q[i][0]; v[1] = q[i][1]; v[2] = v[3] = v[4] = 0.0f; v[5] = -1.0f;
    }
    s_fb_n = 6;
    flush_flat_batch();
    s_depth_clears++;
    { FILE *f = pgxp_tri_log(); if (f) fprintf(f, "C\n"); }
}
/* Before a depth-tested triangle: clear when the drawing area changed since
 * the last one (a new frame buffer) or after a fill, and, like DuckStation's
 * PGXP depth clear threshold, when the average SZ jumps back by the
 * threshold or more (the game started a new 3D pass over the same area). */
static void depth_before_tri(void) {
    const float avg = (s_pz[0] + s_pz[1] + s_pz[2]) * (1.0f / 3.0f);
    if (s_depth_used && s_pgxp_depth_threshold > 0.0f &&
        avg - s_depth_last_avg >= s_pgxp_depth_threshold)
        s_depth_need_clear = 1;
    s_depth_last_avg = avg;
    if (s_depth_need_clear) {
        s_depth_need_clear = 0;
        depth_clear_now();
    }
    s_depth_used = 1;
    s_depth_tris++;
}

/* Seam expansion of an opaque 3D triangle. At internal scale > 1 a vertex
 * that sits on a neighbour's edge only to within the PS1's precision (R4's
 * subdivided near polygons, T-junctions) leaves a hairline crack onto the
 * background. Each edge moves outward by e native px (mitred corners,
 * limited at sharp angles); B[k][i] are the barycentric coordinates of new
 * corner k in the original triangle, so attributes extrapolate exactly:
 * affine ones as sum B a, perspective ones as sum B q a / sum B q, depth as
 * 1/sz' = sum B / sz. The depth buffer resolves the overlap. */
static float pgxp_seam_width(void) {
    static float fine_px = -1.0f;   /* PSX_PGXP_SEAM_PX: "fine" width in output px */
    if (fine_px < 0.0f) {
        const char *e = getenv("PSX_PGXP_SEAM_PX");
        fine_px = e && *e ? (float)atof(e) : 1.0f;
        if (fine_px < 0.0f) fine_px = 0.0f;
    }
    if (s_pgxp_seam == 1) return s_out_scale > 1 ? fine_px / (float)s_out_scale : 0.0f;
    if (s_pgxp_seam == 2) return s_out_scale > 1 ? 0.5f : 0.0f;   /* above 1x only, as fine */
    return 0.0f;
}
static int seam_expand(float x[3], float y[3], float e, float B[3][3]) {
    if (e <= 0.0f) return 0;
    const float area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
    if (fabsf(area) < 1e-4f) return 0;
    const float sgn = area > 0.0f ? 1.0f : -1.0f;
    float nx[3], ny[3];   /* outward unit normal of edge k (k -> k+1) */
    for (int k = 0; k < 3; k++) {
        const int k1 = (k + 1) % 3;
        const float ex = x[k1] - x[k], ey = y[k1] - y[k], l = sqrtf(ex * ex + ey * ey);
        if (l < 1e-5f) return 0;
        nx[k] = sgn * ey / l; ny[k] = -sgn * ex / l;
    }
    float ox[3], oy[3];
    for (int k = 0; k < 3; k++) {
        const int kp = (k + 2) % 3;   /* edges kp and k meet at corner k */
        float mx = nx[kp] + nx[k], my = ny[kp] + ny[k];
        const float d = 1.0f + nx[kp] * nx[k] + ny[kp] * ny[k];
        if (d < 0.25f) {              /* sharp corner: limit the mitre to 2e */
            const float l = sqrtf(mx * mx + my * my);
            if (l > 1e-6f) { mx *= 2.0f / l; my *= 2.0f / l; }
        } else { mx /= d; my /= d; }
        ox[k] = x[k] + e * mx; oy[k] = y[k] + e * my;
    }
    const float inv = 1.0f / area;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) {
            const int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
            B[k][i] = ((x[i2] - x[i1]) * (oy[k] - y[i1]) - (y[i2] - y[i1]) * (ox[k] - x[i1])) * inv;
        }
    }
    for (int k = 0; k < 3; k++) { x[k] = ox[k]; y[k] = oy[k]; }
    s_seam_tris++;
    return 1;
}
/* Extrapolate n attributes (stride apart) of 3 vertices with B. q != NULL:
 * perspective-correct with weights q (left unchanged; see seam_q). */
static void seam_attr(const float B[3][3], float *a, int stride, int n, const float *q) {
    float src[3][8];
    for (int i = 0; i < 3; i++) for (int j = 0; j < n; j++) src[i][j] = a[i * stride + j];
    for (int k = 0; k < 3; k++) {
        float Q = 0.0f;
        if (q) for (int i = 0; i < 3; i++) Q += B[k][i] * q[i];
        for (int j = 0; j < n; j++) {
            float v = 0.0f;
            if (q && Q > 1e-12f) {
                for (int i = 0; i < 3; i++) v += B[k][i] * q[i] * src[i][j];
                v /= Q;
            } else {
                for (int i = 0; i < 3; i++) v += B[k][i] * src[i][j];
            }
            a[k * stride + j] = v;
        }
    }
}
static void seam_q(const float B[3][3], float q[3]) {
    float o[3];
    for (int k = 0; k < 3; k++) { o[k] = 0.0f; for (int i = 0; i < 3; i++) o[k] += B[k][i] * q[i]; }
    for (int k = 0; k < 3; k++) q[k] = o[k] > 1e-9f ? o[k] : q[k];
}
static void seam_sz(const float B[3][3], float z[3]) {
    float o[3];
    for (int k = 0; k < 3; k++) {
        float iz = 0.0f;
        for (int i = 0; i < 3; i++) iz += B[k][i] / z[i];
        o[k] = iz > 1e-9f ? 1.0f / iz : z[k];
        if (o[k] < 1.0f) o[k] = 1.0f;
        if (o[k] > 65535.0f) o[k] = 65535.0f;
    }
    for (int k = 0; k < 3; k++) z[k] = o[k];
}

static void gpu_geometry(GLenum mode, const int *xs, const int *ys,
                         const uint16_t *cs, int n, int semi) {
    flush_tex_batch();   /* flat prim: drain textured draws first (order + program) */
    flush_cpu_upload();  /* also drains flat batch if an upload was pending */
    mark_prim_dirty(xs, ys, n, 0 /* flat */);
    /* Sub-pixel positions only describe a 3-vertex projected triangle. */
    const int precise = s_pc_valid && mode == GL_TRIANGLES && n == 3;

    /* Native-wide: while the wide mirror is live (g_wide_cur), a line joins
     * the flat batch as a GL_LINES batch: same vertices, program, line width
     * and painter order, keyed on mode, semi and mask-set, and drained by
     * flush_line_batch() before the mask-check, mirror-suppress or wide
     * target changes, so every line is drawn and mirrored under the state it
     * arrived with. Drawn one by one, every line flushed the textured batch
     * and rebound the hr and wide surfaces for its mirror; R4 draws hundreds
     * of lines per race frame, which held its 21:9 race at 1x to about 53
     * frames/s. A line the backdrop-stretch gate would widen keeps the
     * immediate path (the flat batch mirrors unstretched). With no live
     * mirror (native-wide off, back at 4:3, or an offscreen draw) nothing
     * changes. 1x only: above 1x a line is drawn as its own shape (the
     * internal-resolution work draws it as a one-native-pixel quad and
     * batches that itself), so it is left to that path. */
    if (mode == GL_LINES && n == 2 && s_hr_scale == 1 && !s_hiw && g_wide_cur &&
        !bd_prim_gate(xs, n, 0)) {
        if (s_fb_n > 0 && (s_fb_mode != GL_LINES || s_fb_semi != semi ||
                           s_fb_mask != (int)s_mask_set || s_fb_gate != 0 || s_fb_depth != 0))
            flush_flat_batch();
        if (s_fb_n + 2 > FLATBATCH_MAXV)
            flush_flat_batch();
        s_fb_mode = GL_LINES;
        s_fb_gate = 0;
        s_fb_depth = 0;   /* lines are 2D: never depth-tested (G1.14) */
        s_fb_semi = semi;
        s_fb_mask = (int)s_mask_set;
        float mask_a = s_mask_set ? 1.0f : 0.0f;
        for (int i = 0; i < 2; i++) {
            float *v = &s_fb[s_fb_n * 6];
            v[0] = (float)xs[i];
            v[1] = (float)ys[i];
            v[2] = ((cs[i] & 0x1F) << 3) / 255.0f;
            v[3] = (((cs[i] >> 5) & 0x1F) << 3) / 255.0f;
            v[4] = (((cs[i] >> 10) & 0x1F) << 3) / 255.0f;
            v[5] = mask_a;
            s_fb_n++;
        }
        return;
    }
    /* Above 1x a line is a quad of two triangles (line_to_quad), so it joins
     * the flat batch like any gouraud triangle: same vertices, same program,
     * same painter order and batch keys, so the same pixels. Drawn one by
     * one, every line flushed the textured batch and, with native-wide,
     * rebound the hr and wide surfaces twice; R4 draws hundreds of lines per
     * race frame, and at 9x those render-pass switches held its 21:9 race to
     * 22-30 frames/s (at 8K in windowed mode, ~10). In windowed mode the 1x
     * hr surface still draws the line as GL_LINES, from the second vertex
     * set kept in s_fbl; the window and the wide surfaces take the quad. A
     * line the backdrop-stretch gate would widen keeps the immediate path
     * (the flat batch mirrors unstretched). 1x lines take the GL_LINES
     * batch above. */
    if (mode == GL_LINES && n == 2 && s_out_scale > 1 && !bd_prim_gate(xs, n, 0)) {
        float lv[2 * 6], quad[6 * 6];
        float mask_a = s_mask_set ? 1.0f : 0.0f;
        for (int i = 0; i < 2; i++) {
            lv[i*6+0] = (float)xs[i];
            lv[i*6+1] = (float)ys[i];
            lv[i*6+2] = ((cs[i] & 0x1F) << 3) / 255.0f;
            lv[i*6+3] = (((cs[i] >> 5) & 0x1F) << 3) / 255.0f;
            lv[i*6+4] = (((cs[i] >> 10) & 0x1F) << 3) / 255.0f;
            lv[i*6+5] = mask_a;
        }
        line_to_quad(lv, quad);
        if (s_fb_n > 0 && (s_fb_mode != GL_TRIANGLES || s_fb_semi != semi ||
                           s_fb_mask != (int)s_mask_set || s_fb_depth != 0))
            flush_flat_batch();
        if (s_fb_n + 6 > FLATBATCH_MAXV)
            flush_flat_batch();
        s_fb_mode = GL_TRIANGLES;
        s_fb_semi = semi;
        s_fb_mask = (int)s_mask_set;
        s_fb_depth = 0;   /* lines are 2D: never depth-tested (G1.14) */
        if (s_hiw) {   /* the 1x hr surface draws the line itself */
            s_fbl_at[s_fbl_n] = s_fb_n;
            memcpy(&s_fbl[s_fbl_n * 2 * 6], lv, sizeof lv);
            s_fbl_n++;
        }
        memcpy(&s_fb[s_fb_n * 6], quad, sizeof quad);
        s_fb_n += 6;
        return;
    }
    /* Backdrop-stretched lines and 1x lines with no live mirror stay
     * immediate; tris batch for MotK 0x68 starfields. */
    if (mode != GL_TRIANGLES || n < 3) {
        flush_flat_batch();
        float verts[3 * 6];
        float mask_a = s_mask_set ? 1.0f : 0.0f;
        for (int i = 0; i < n; i++) {
            verts[i*6+0] = precise ? s_pc_x[i] : (float)xs[i];
            verts[i*6+1] = precise ? s_pc_y[i] : (float)ys[i];
            verts[i*6+2] = ((cs[i] & 0x1F) << 3) / 255.0f;
            verts[i*6+3] = (((cs[i] >> 5) & 0x1F) << 3) / 255.0f;
            verts[i*6+4] = (((cs[i] >> 10) & 0x1F) << 3) / 255.0f;
            verts[i*6+5] = mask_a;
        }
        /* S > 1: a line becomes a one-native-pixel quad (see line_to_quad);
         * native keeps GL_LINES exactly as before. The hr surface draws at
         * s_hr_scale; the presentation surfaces (the high-resolution window,
         * the wide surfaces) at s_out_scale, which differ only in windowed mode. */
        float quad[6 * 6];
        const int is_line = (mode == GL_LINES && n == 2);
        if (is_line && s_out_scale > 1) line_to_quad(verts, quad);
        GLenum draw_mode = mode;
        const float *draw_verts = verts;
        int draw_n = n;
        if (is_line && s_hr_scale > 1) {
            draw_mode = GL_TRIANGLES; draw_verts = quad; draw_n = 6;
        }
        wide_stencil_ready();   /* before any of this draw's GL state */
        if (draw_mode != GL_TRIANGLES && !hiw_on())
            hiw_flush_queue();  /* GL_LINES mirror at once: queued mirrors first */
        hr_begin(1);
        if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
        mask_stencil(s_mask_set);
        if (draw_mode == GL_LINES) glLineWidth((float)s_hr_scale);
        p_glUseProgram(s_geo_prog);
        p_glBindVertexArray(s_geo_vao);
        p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
        p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(draw_n * 6 * sizeof(float)),
                       draw_verts, PSXGL_STREAM_DRAW);
        glDrawArrays(draw_mode, 0, draw_n);
        if (is_line && s_out_scale > 1 && draw_mode == GL_LINES) {
            /* windowed: hr drew a 1x line; the S surfaces take the quad */
            p_glBufferData(PSXGL_ARRAY_BUFFER, (ptrdiff_t)(6 * 6 * sizeof(float)),
                           quad, PSXGL_STREAM_DRAW);
            draw_mode = GL_TRIANGLES; draw_n = 6;
        }
        int mirror = g_wide_cur && !s_wide_suppress && s_ws_ablate != 1 &&
                     !(!g_ws_bd_stretch_on && mirror_geo_center_only(xs, n));
        if (mirror) {
            float sc, ce;
            wide_bd_scale(bd_prim_gate(xs, n, 0), &sc, &ce);
            wst_note_draw(g_wide_cur, verts, n, 6, wide_dx(), sc, ce, s_mask_check);
        }
        if ((hiw_on() || (mirror && draw_mode == GL_TRIANGLES && wide_queue_live())) &&
            hiw_enqueue_geo(draw_mode == GL_TRIANGLES && is_line ? quad : verts,
                            draw_n, semi, s_mask_set, mirror, bd_prim_gate(xs, n, 0)))
            mirror = 0;
        if (mirror) {
            int dx = wide_dx();
            s_bd_gate = bd_prim_gate(xs, n, 0);
            gl_perf_mirror_begin();
            wide_target_begin(dx, s_geo_uXoff, s_geo_uXhalf);
            wide_set_bd_scale(s_geo_uXscale, s_geo_uXcenter);
            if (s_ws_ablate != 2) glDrawArrays(draw_mode, 0, draw_n);
            wide_clear_bd_scale(s_geo_uXscale, s_geo_uXcenter);
            wide_target_end(s_geo_uXoff, s_geo_uXhalf);
            gl_perf_mirror_end();
        }
        hr_end();
        return;
    }

    int gate = bd_prim_gate(xs, n, 0);
    const int dmode = n == 3 ? pgxp_tri_depth_mode(semi) : 0;
    if (dmode) depth_before_tri();
    if (s_fb_n > 0 && (s_fb_mode != GL_TRIANGLES || s_fb_semi != semi ||
                       s_fb_mask != (int)s_mask_set || s_fb_gate != gate ||
                       s_fb_depth != dmode))
        flush_flat_batch();
    if (s_fb_n + n > FLATBATCH_MAXV)
        flush_flat_batch();
    s_fb_mode = GL_TRIANGLES;
    s_fb_semi = semi;
    s_fb_mask = (int)s_mask_set;
    s_fb_gate = gate;
    s_fb_depth = dmode;

    float mask_a = s_mask_set ? 1.0f : 0.0f;
    float *v0 = &s_fb[s_fb_n * 6];
    for (int i = 0; i < n; i++) {
        float *v = &s_fb[s_fb_n * 6];
        v[0] = precise ? s_pc_x[i] : (float)xs[i];
        v[1] = precise ? s_pc_y[i] : (float)ys[i];
        v[2] = ((cs[i] & 0x1F) << 3) / 255.0f;
        v[3] = (((cs[i] >> 5) & 0x1F) << 3) / 255.0f;
        v[4] = (((cs[i] >> 10) & 0x1F) << 3) / 255.0f;
        v[5] = mask_a;
        s_fb_n++;
    }
    if (n == 3 && pgxp_tri_is_3d()) {
        float z[3] = { s_pz[0], s_pz[1], s_pz[2] };
        /* Only a triangle the depth buffer tests (mode 1) expands: without
         * depth, or near the camera where triangles stay painter-ordered, a
         * widened edge would draw over its neighbour. */
        if (dmode == 1 && s_pgxp_seam) {
            float x[3], y[3], B[3][3];
            for (int i = 0; i < 3; i++) { x[i] = v0[i * 6]; y[i] = v0[i * 6 + 1]; }
            if (seam_expand(x, y, pgxp_seam_width(), B)) {
                float qc[3] = { 1.0f / z[0], 1.0f / z[1], 1.0f / z[2] };
                seam_attr(B, v0 + 2, 6, 3, s_pgxp_cpersp ? qc : NULL);
                seam_sz(B, z);
                for (int i = 0; i < 3; i++) { v0[i * 6] = x[i]; v0[i * 6 + 1] = y[i]; }
            }
        }
        for (int i = 0; i < 3; i++)
            v0[i * 6 + 5] = mask_a + 2.0f * pgxp_vertex_code(z[i]);
    }
    if (n == 3) pgxp_tri_log_tri('G', dmode, semi, v0, 6);
}

/* Drain a pending native-wide line batch (gpu_geometry). flush_flat_batch()
 * reads the mask-check, mirror-suppress and wide-target state when it runs,
 * not when a primitive was queued, so a line batch is drained before any of
 * them changes: each line is then drawn and mirrored under the state it
 * arrived with, as on the immediate path. A pending triangle batch keeps its
 * existing behaviour at these points. */
static void flush_line_batch(void) {
    if (s_fb_n > 0 && s_fb_mode == GL_LINES) flush_flat_batch();
}

static void gpu_triangle(int x0,int y0,uint16_t c0, int x1,int y1,uint16_t c1,
                         int x2,int y2,uint16_t c2, int semi) {
    int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    uint16_t cs[3] = {c0, c1, c2};
    gpu_geometry(GL_TRIANGLES, xs, ys, cs, 3, semi);
}

static void gpu_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int semi) {
    int xs[2] = {x0, x1}, ys[2] = {y0, y1};
    uint16_t cs[2] = {c0, c1};
    gpu_geometry(GL_LINES, xs, ys, cs, 2, semi);
}

/* Shared PS1 uv-sampling model (limits + mirrored-2D compensation) — one
 * implementation for GL/VK/SW, see gpu_uv.h. */
#include "gpu_uv.h"

/* Textured triangle. Always two passes split by the per-texel STP bit so the
 * stencil (mask) write value is constant within each pass; the semi pass is
 * also where PS1 blending applies. lim = uv sampling bounds (see
 * tri_uv_limits); NULL computes them from the vertices. */
static void gpu_textured_triangle(const int *xs, const int *ys,
                                  const int *us, const int *vs,
                                  const float *col, uint16_t texpage,
                                  uint16_t clut_x, uint16_t clut_y, int rawtex,
                                  int semi, const int *lim) {
    // Mode 2 needs proven world geometry. Sprites, HUD and untracked packets
    // retain point sampling; their cutout pixels must stay sharp.
    const int untracked=(lim && !s_projected_uv_valid) || (!s_pc_valid && !s_pq_valid);
    /* [video] texture_lod rides in the filter batch key (bit 4) for proven
     * world geometry only; sprites and HUD keep their exact texels. */
    const int filter=(s_tex_filter==2 && untracked ? 0 : s_tex_filter) |
                     (s_tex_lod && !untracked ? 16 : 0);
    int lim_buf[4];
    int uv_buf[6];
    if (!lim) {
        /* World polygons keep authored UVs and inclusive atlas bounds.
         * Affine primitives keep the PS1 center-sampling compensation (rects
         * arrive with their own precomputed bounds and compensated UVs). */
        int *mu = uv_buf, *mv = uv_buf + 3;
        for (int i = 0; i < 3; i++) { mu[i] = us[i]; mv[i] = vs[i]; }
        psx_uv_tri_center_sample(xs, ys, mu, mv, s_pq_valid, lim_buf);
        us = mu; vs = mv;
        lim = lim_buf;
    }
    s_scene_prims_tex++;
    int base_x = (texpage & 0xF) * 64;
    int base_y = ((texpage >> 4) & 1) * 256;
    int depth  = (texpage >> 7) & 3; if (depth > 2) depth = 2;

    flush_cpu_upload();   /* if a CPU->VRAM upload is pending it flushes the batch first */
    if (!s_selected_bank_tex)
        flush_pack_if_sampling(base_x, base_y, depth, clut_x, clut_y);
    else if (s_selected_bank_live_clut)
        flush_pack_if_sampling(-1, 0, depth, clut_x, clut_y);
    GpuHdTextureImage hd = {0};
    GLuint hd_tex = 0;
    if (s_hd_native_authority && !s_selected_bank_tex) {
        uint32_t window = (uint32_t)(s_tw_mask_x | (s_tw_mask_y << 5) |
                                    (s_tw_off_x << 10) | (s_tw_off_y << 15));
        if (gpu_hd_textures_acquire_draw(texpage, clut_x, clut_y, lim, window,
                                        semi >= 0, &hd)) {
            hd_tex = hd_gl_texture(&hd);
            if (!hd_tex) hd.alpha_mode = 0;
            else gpu_hd_textures_note_applied();
        }
    }
    mark_prim_dirty(xs, ys, 3, 1 /* textured */);

    /* Append to the textured batch. Flush first if this prim's blend/mask/twin/
     * filter differ from the open batch, or the buffer is full. Per-prim texture
     * state goes in the vertex; only these keys force a new draw (the window
     * not at all under texture-window batching, see s_twin_batching). */
    {
        flush_flat_batch();   /* painter order: flat GEO before textured */
        int twx = s_tw_mask_x, twy = s_tw_mask_y, tox = s_tw_off_x, toy = s_tw_off_y;
        int gate = bd_prim_gate(xs, 3, 1); /* backdrop-stretch gate is also a batch key */
        /* Batch key: keep opaque as -1. Dual-source (4) is only for semi modes
         * 0/1/3 when mask-check is off — never coalesce opaque into that key.
         * Mixing opaque+semi under u_semimode==4 made additive particle/glow
         * batches (CTR Naughty Dog intro binary funnel) paint over later
         * opaque crate flaps whenever submission order and STP bits disagreed
         * with the dual-source path's assumptions. */
        int batch_semi;
        if (semi < 0)
            batch_semi = -1;
        else if (!s_mask_check && semi != 2)
            batch_semi = 4;
        else
            batch_semi = semi;
        /* STP draw-ORDER correctness. flush_tex_batch's conservative two-pass
         * path draws pass 1 = every prim's STP=0 texels then pass 2 = every
         * prim's STP=1 texels — a behind prim's semi texels then overwrite a
         * front prim's opaque texels (Tomba AP-block / CTR intro flaps). The
         * dual-source single-pass path avoids that WITHIN one prim, but
         * batching many overlapping semi quads (digit particles + glow) still
         * mis-orders against neighbouring opaque geometry. Isolate EVERY
         * semi-transparent textured prim: drain the open batch, draw this
         * prim alone (composited fully before the next), let opaque prims
         * keep batching. Cost is one draw per semi prim. A separately opted-in
         * immutable bank may batch the single-pass dual-source cases: it
         * cannot alias a render target, keeps painter order, and still splits
         * on opaque transitions, bank/state changes, masking or subtraction. */
        int isolate = semi >= 0 &&
            !mod_texture_bank_batchable(s_selected_bank_tex != 0, s_mask_check, semi);
        const int tdmode = pgxp_tri_depth_mode(semi);
        if (tdmode) depth_before_tri();
        int reason = -1;
        if (s_tb_n > 0) {
            if (s_tb_bank_tex != s_selected_bank_tex || s_tb_bank_live_clut != s_selected_bank_live_clut || s_tb_hd_tex != hd_tex) reason = 0;
            else if (isolate) reason = 0;
            else if (batch_semi != s_tb_semi) reason = 1;
            else if (s_mask_set != s_tb_mask) reason = 2;
            else if (filter != s_tb_filter) reason = 3;
            else if (gate != s_tb_gate) reason = 4;
            else if ((!s_twin_batching || s_mask_check) &&
                     (twx != s_tb_twin[0] || twy != s_tb_twin[1] ||
                      tox != s_tb_twin[2] || toy != s_tb_twin[3])) reason = 5;
            else if (tdmode != s_tb_depth) reason = 7;
        }
        if (reason >= 0) {
            s_batch_reason[reason]++;
            flush_tex_batch();
        }
        if (s_tb_n + 3 > TEXBATCH_MAXV) { s_batch_reason[6]++; flush_tex_batch(); }
        if (s_tb_n == 0) {            /* opening a batch: capture its keyed state */
            s_tb_semi = batch_semi; s_tb_mask = s_mask_set; s_tb_filter = filter; s_tb_gate = gate;
            s_tb_depth = tdmode;
            s_tb_bank_tex = s_selected_bank_tex;
            s_tb_bank_live_clut = s_selected_bank_live_clut;
            s_tb_hd_tex = hd_tex;
            s_tb_twin[0] = twx; s_tb_twin[1] = twy; s_tb_twin[2] = tox; s_tb_twin[3] = toy;
        }
        /* The prim's window, exact as a float (20 bits). */
        float twin = (float)(twx | (twy << 5) | (tox << 10) | (toy << 15));
        float *vp = &s_tb[s_tb_n * TEXV];
        for (int i = 0; i < 3; i++, vp += TEXV) {
            vp[0] = s_pc_valid ? s_pc_x[i] : (float)xs[i];
            vp[1] = s_pc_valid ? s_pc_y[i] : (float)ys[i];
            vp[2] = s_projected_uv_valid ? s_projected_u[i] : (float)us[i];
            vp[3] = s_projected_uv_valid ? s_projected_v[i] : (float)vs[i];
            vp[4] = col[i*3+0];     vp[5] = col[i*3+1];     vp[6] = col[i*3+2];   vp[7] = 1.0f;
            vp[8]  = (float)base_x;  vp[9]  = (float)base_y;        /* a_tpage  */
            vp[10] = (float)clut_x;  vp[11] = (float)clut_y;        /* a_clut   */
            vp[12] = (float)depth;   vp[13] = (float)rawtex;        /* a_depth, a_raw */
            vp[14] = (float)lim[0];  vp[15] = (float)lim[1];        /* a_limits */
            vp[16] = (float)lim[2];  vp[17] = (float)lim[3];
            vp[18] = semi >= 0 ? (float)(semi + 1) : 0.0f;          /* a_semi code */
            vp[19] = s_pq_valid ? s_pq[i] : 0.0f;                   /* a_q; 0 = affine */
            vp[20] = twin;                                          /* a_twin   */
            vp[21] = (float)hd.origin_u; vp[22] = (float)hd.origin_v;
            vp[23] = (float)hd.source_width; vp[24] = (float)hd.source_height;
            vp[25] = (float)hd.alpha_mode;
        }
        if (pgxp_tri_is_3d()) {
            float *t0 = &s_tb[s_tb_n * TEXV];
            float z[3] = { s_pz[0], s_pz[1], s_pz[2] };
            if (tdmode == 1 && s_pgxp_seam) {   /* depth-tested only (gpu_geometry) */
                float x[3], y[3], B[3][3];
                for (int i = 0; i < 3; i++) { x[i] = t0[i * TEXV]; y[i] = t0[i * TEXV + 1]; }
                if (seam_expand(x, y, pgxp_seam_width(), B)) {
                    float q[3] = { t0[19], t0[TEXV + 19], t0[2 * TEXV + 19] };
                    float qc[3] = { 1.0f / z[0], 1.0f / z[1], 1.0f / z[2] };
                    seam_attr(B, t0 + 2, TEXV, 2, s_pq_valid ? q : NULL);          /* uv  */
                    seam_attr(B, t0 + 4, TEXV, 3, s_pgxp_cpersp ? (s_pq_valid ? q : qc) : NULL);
                    if (s_pq_valid) {
                        seam_q(B, q);
                        for (int i = 0; i < 3; i++) t0[i * TEXV + 19] = q[i];
                    }
                    seam_sz(B, z);
                    for (int i = 0; i < 3; i++) { t0[i * TEXV] = x[i]; t0[i * TEXV + 1] = y[i]; }
                }
            }
            for (int i = 0; i < 3; i++) {
                const float code = pgxp_vertex_code(z[i]);
                if (code > 0.0f) t0[i * TEXV + 7] = -code;   /* a_col.a (TEX_VS) */
            }
        }
        pgxp_tri_log_tri('T', tdmode, semi, &s_tb[s_tb_n * TEXV], TEXV);
        s_tb_n += 3;
        if (isolate) flush_tex_batch();   /* draw this semi prim alone, in submission order */
    }
    gpu_hd_textures_release_image(&hd);
}

/* Draw a flat-colored rect (GEO program) DIRECTLY into the active wide surface
 * at wide-space coords [wx, wx+ww) × [y, y+h). Used only by the full-screen-
 * overlay path; positions are already in wide space so u_xoff stays 0. Mirrors
 * raster_flat_rect(&wt, ...) in sw_draw_flat_rect, including its draw-area row
 * clip. Caller stays inside
 * hr_begin/hr_end. */
static void wide_flat_rect_direct(int wx, int y, int ww, int h, uint16_t c, int semi) {
    if (ww <= 0 || h <= 0) return;
    float r = ((c & 0x1F) << 3) / 255.0f;
    float g = (((c >> 5) & 0x1F) << 3) / 255.0f;
    float b = (((c >> 10) & 0x1F) << 3) / 255.0f;
    float a = s_mask_set ? 1.0f : 0.0f;
    float fx0 = (float)wx, fy0 = (float)y, fx1 = (float)(wx + ww), fy1 = (float)(y + h);
    float verts[6 * 6] = {
        fx0,fy0,r,g,b,a,  fx1,fy0,r,g,b,a,  fx0,fy1,r,g,b,a,
        fx1,fy0,r,g,b,a,  fx0,fy1,r,g,b,a,  fx1,fy1,r,g,b,a,
    };
    /* Wide target: positions already wide-space so u_xoff = 0; full-width,
     * draw-area-rows scissor like rt_wide() — a full-height scissor let an
     * overlay-width rect that canonically clips at a vertical double-buffer
     * band boundary paint the OTHER band's rows; u_xhalf = g_wide_w/2. */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, g_wide_cur);
    glViewport(0, 0, g_wide_w * s_out_scale, VRAM_H * s_out_scale);
    wide_band_scissor();
    if (semi >= 0) apply_psx_blend(semi); else glDisable(GL_BLEND);
    mask_stencil(s_mask_set);
    p_glUseProgram(s_geo_prog);
    if (s_hiw) p_glUniform1f(s_geo_uShift, s_shift_hi);
    p_glUniform1f(s_geo_uXoff, 0.0f);
    p_glUniform1f(s_geo_uXhalf, (float)g_wide_w / 2.0f);
    p_glBindVertexArray(s_geo_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (s_hiw) p_glUniform1f(s_geo_uShift, s_shift_hr);
    p_glUniform1f(s_geo_uXoff, 0.0f);
    p_glUniform1f(s_geo_uXhalf, 512.0f);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
}

static void gpu_flat_rect(int x,int y,int w,int h,uint16_t c,int semi) {
    if (w <= 0 || h <= 0) return;
    /* Full-screen 2D overlay (pause gray-filter / load fade): a flat rect
     * spanning the whole 4:3 framebuffer must cover the whole wide surface too,
     * else the revealed 16:9 margins are left undimmed/unfaded. Same detection
     * as sw_draw_flat_rect: native_w = the 4:3 framebuffer width (g_wide_w less
     * the per-side reveal on both sides); the rect's native screen-X span
     * (x - base) must cover [0, native_w). When it does, the two canonical
     * triangles are drawn WITHOUT the per-triangle wide mirror (suppressed) and
     * a single full-width rect is drawn into the wide surface instead; every
     * other rect mirrors 1:1 via the generic gpu_geometry path. Only runs in
     * native-wide (g_wide_cur != 0), so 4:3 is unaffected. */
    int overlay = 0;
    if (g_wide_cur) {
        int native_w = g_wide_w - 2 * g_wide_off;
        int lx = x - g_wide_cur_base, rx = x + w - g_wide_cur_base;
        overlay = (native_w > 0 && lx <= 0 && rx >= native_w);
    }
    if (overlay) {
        flush_line_batch();  /* queued lines mirror as they arrived */
        /* Earlier world geometry still needs its ordinary wide mirror. Drain
         * it before suppressing only this overlay's canonical triangles. */
        flush_flat_batch();
        flush_tex_batch();
        s_wide_suppress = 1;
    }
    gpu_triangle(x,   y,   c, x+w, y,   c, x,   y+h, c, semi);
    gpu_triangle(x+w, y,   c, x,   y+h, c, x+w, y+h, c, semi);
    if (overlay) {
        /* gpu_triangle queues flat geometry. Finish these two triangles while
         * their mirror remains suppressed, before the direct wide overlay. */
        flush_flat_batch();
        s_wide_suppress = 0;
        /* Re-open the bracket for the full-width wide pass so
         * blend/scissor/program state is clean. */
        if (s_ws_ablate != 1) {
            wide_stencil_ready();
            hiw_flush_queue();   /* windowed: queued wide mirrors land first */
            if (!s_mask_check) wst_add(wide_index(g_wide_cur), 0, y, g_wide_w, y + h);
            hr_begin(0);
            gl_perf_mirror_begin();
            wide_flat_rect_direct(0, y, g_wide_w, h, c, semi);
            gl_perf_mirror_end();
            hr_end();
        }
    }
}

static void gpu_textured_rect(int x,int y,int w,int h,
                              int u0,int v0,int u1,int v1,
                              uint16_t clut_x,uint16_t clut_y,uint16_t tp,int semi) {
    if (w <= 0 || h <= 0) return;
    float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
    float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
    /* gpu.c routes axis-aligned MIRRORED quads (X/Y-flipped 2D sprites,
     * e.g. right-facing MMX entities) through THIS path as scaled rects
     * with u0>u1 / v0>v1 — they never reach the poly path. Exact bounds
     * from the original corners, then the mirror bump (see gpu_uv.h). */
    int lim[4];
    psx_uv_rect_limits(u0, v0, u1, v1, lim);
    psx_uv_rect_mirror_offset(&u0, &v0, &u1, &v1);
    int xs1[3]={x, x+w, x},    ys1[3]={y, y, y+h};
    int us1[3]={u0,u1,u0},     vs1[3]={v0,v0,v1};
    gpu_textured_triangle(xs1,ys1,us1,vs1,col,tp,clut_x,clut_y,s_mod_raw,semi,lim);
    int xs2[3]={x+w, x, x+w},  ys2[3]={y, y+h, y+h};
    int us2[3]={u1,u0,u1},     vs2[3]={v0,v1,v1};
    gpu_textured_triangle(xs2,ys2,us2,vs2,col,tp,clut_x,clut_y,s_mod_raw,semi,lim);
}

/* GP0(02h) fill: writes color with bit15=0, ignoring draw area, mask and
 * offset; coordinates wrap. A scissored clear (color + stencil) per wrapped
 * segment is exactly this. */
static void fill_segment(int x, int y, int w, int h, float r, float g, float b) {
    if (w <= 0 || h <= 0) return;
    glScissor(x * s_hr_scale, y * s_hr_scale, w * s_hr_scale, h * s_hr_scale);
    glClearColor(r, g, b, 0.0f);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
    if (!cpu_vram_authoritative()) rect_add(&s_cpu_dirty, x, y, x + w - 1, y + h - 1);
}

static void gpu_fill(int x,int y,int w,int h,uint16_t c) {
    if (w <= 0 || h <= 0) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    float r=(c&0x1F)/31.0f, g=((c>>5)&0x1F)/31.0f, b=((c>>10)&0x1F)/31.0f;
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    int w1 = w, w2 = 0, h1 = h, h2 = 0;
    if (x + w > VRAM_W) { w1 = VRAM_W - x; w2 = w - w1; }
    if (y + h > VRAM_H) { h1 = VRAM_H - y; h2 = h - h1; }

    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glViewport(0, 0, VRAM_W * s_hr_scale, VRAM_H * s_hr_scale);
    glEnable(GL_SCISSOR_TEST);
    fill_segment(x, y, w1, h1, r, g, b);
    if (w2)       fill_segment(0, y, w2, h1, r, g, b);
    if (h2)       fill_segment(x, 0, w1, h2, r, g, b);
    if (w2 && h2) fill_segment(0, 0, w2, h2, r, g, b);
    if (hiw_on()) {
        hiw_clear_rect(x, y, w1, h1, r, g, b, 0.0f, 0);
        if (w2)       hiw_clear_rect(0, y, w2, h1, r, g, b, 0.0f, 0);
        if (h2)       hiw_clear_rect(x, 0, w1, h2, r, g, b, 0.0f, 0);
        if (w2 && h2) hiw_clear_rect(0, 0, w2, h2, r, g, b, 0.0f, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    if (!cpu_vram_authoritative())
        s_gpu_dirty = 1;
    coh_record(GL_COH_FILL, x, y, x + w - 1, y + h - 1);
}

/* VRAM->VRAM copy: blit the source region to the scratch texture (resolves
 * overlap), then draw it back at the destination through the BLIT program so
 * mask set/check and the stencil mirror apply, exactly like sw_copy_rect. */
static void gpu_copy_rect(int sx,int sy,int dx,int dy,int w,int h) {
    if (w <= 0 || h <= 0) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    /* Clamp to bounds (the software path wraps; wrapping copies are unused
     * in practice — see the file header). */
    if (sx < 0) sx = 0; if (sy < 0) sy = 0;
    if (dx < 0) dx = 0; if (dy < 0) dy = 0;
    if (sx + w > VRAM_W) w = VRAM_W - sx;
    if (dx + w > VRAM_W) w = VRAM_W - dx;
    if (sy + h > VRAM_H) h = VRAM_H - sy;
    if (dy + h > VRAM_H) h = VRAM_H - dy;
    if (w <= 0 || h <= 0) return;

    int S = s_hr_scale;
    /* Stage the source at the scratch ORIGIN (not at its own hr coords), so
     * the scratch only has to be as large as the biggest copy. The integer
     * texelFetch in BLIT_FS makes the placement invisible in the result.
     * w*S and h*S fit the GPU limit whenever the hr surface does; the
     * high-resolution window stages its S-scaled source in a scratch of its
     * own (hiw_mirror_copy), so this one never grows past the hr surface. */
    if (!scratch_ensure(w * S, h * S)) return;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(sx*S, sy*S, (sx+w)*S, (sy+h)*S,
                        0, 0, w*S, h*S,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);

    hr_begin(0);   /* copies ignore the draw area */
    glScissor(dx*S, dy*S, w*S, h*S);
    glDisable(GL_BLEND);
    p_glUseProgram(s_blit_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    p_glUniform1i(s_uBlitSrc, 0);
    p_glUniform1i(s_uBlitMaskset, s_mask_set);
    /* Scratch holds the source at its origin: texel = frag - dst*S. */
    p_glUniform1i(s_uBlitSrcDiv, 1);
    p_glUniform2i(s_uBlitSrcOff, -dx * S, -dy * S);
    float fx0 = (float)dx, fy0 = (float)dy, fx1 = (float)(dx + w), fy1 = (float)(dy + h);
    float verts[6 * 2] = {
        fx0, fy0,  fx1, fy0,  fx0, fy1,
        fx1, fy0,  fx0, fy1,  fx1, fy1,
    };
    p_glBindVertexArray(s_blit_vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    p_glBufferData(PSXGL_ARRAY_BUFFER, sizeof verts, verts, PSXGL_STREAM_DRAW);
    /* Two passes split by source bit15 so stencil tracks the copied mask. */
    mask_stencil(s_mask_set); p_glUniform1i(s_uBlitPass, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
    mask_stencil(1);          p_glUniform1i(s_uBlitPass, 2); glDrawArrays(GL_TRIANGLES, 0, 6);
    hr_end();
    hiw_mirror_copy(sx, sy, dx, dy, w, h);   /* scratch still holds the 1x source */

    rect_add(&s_pack_dirty, dx, dy, dx + w - 1, dy + h - 1);
    if (!cpu_vram_authoritative()) rect_add(&s_cpu_dirty, dx, dy, dx + w - 1, dy + h - 1);
    if (!cpu_vram_authoritative())
        s_gpu_dirty = 1;
    coh_record(GL_COH_COPY_SRC, sx, sy, sx + w - 1, sy + h - 1);
    coh_record(GL_COH_COPY,     dx, dy, dx + w - 1, dy + h - 1);
}

/* ---- backend vtable wrappers ------------------------------------------- */
static void glb_init(uint16_t *vram) { s_vram = vram; sw_renderer_init(vram); }

/* Under GL the internal-resolution scale lives in the hr FBO; the CPU-side
 * (software mirror, readbacks, screenshots) stays native, so the reported
 * scale is 1 and the software hi-res mirror stays off. */
static void glb_set_scale(int s) {
    /* Only the compile-time ceiling here: the driver and memory limits are
     * known at context init (init_gpu_raster), which clamps further. */
    if (s < 1) s = 1;
    if (s > GL_MAX_INTERNAL_SCALE) s = GL_MAX_INTERNAL_SCALE;
    s_req_scale = s;
    sw_renderer_set_scale(1);
}
static int  glb_scale(void) { return s_out_scale; }   /* real internal SSAA scale (was a stub 1; the
                                                      native-wide CPU present path + gr_scale() callers
                                                      need the true scale — the FBO-direct present is
                                                      unaffected since it never reads gr_scale()) */
/* Dither uniforms live in the geometry and textured programs. The effective
 * value only changes between batches (glb_set_dither drains them first). */
static GLint s_geo_uDither = -1, s_tex_uDither = -1;
static void dither_apply_uniforms(void) {
    if (!s_geo_prog || !s_tex_prog) return;
    GLint prev = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prev);
    if (s_geo_uDither < 0) s_geo_uDither = p_glGetUniformLocation(s_geo_prog, "u_dither");
    if (s_tex_uDither < 0) s_tex_uDither = p_glGetUniformLocation(s_tex_prog, "u_dither");
    p_glUseProgram(s_geo_prog); p_glUniform1i(s_geo_uDither, s_dither_live);
    p_glUseProgram(s_tex_prog); p_glUniform1i(s_tex_uDither, s_dither_live);
    p_glUseProgram((GLuint)prev);
}
static void dither_update(void) {
    const int live = s_dither_bit ? s_dither_mode : 0;
    if (live == s_dither_live) return;
    /* Queued draws keep the state they were submitted under. */
    flush_flat_batch(); flush_tex_batch(); hiw_flush_queue();
    s_dither_live = live;
    dither_apply_uniforms();
}
/* GP0(E1h) bit 9. With [video] dithering off (the default) this only records
 * the bit; nothing is flushed and no uniform changes. */
static void glb_set_dither(int on) {
    s_dither_bit = on ? 1 : 0;
    if (s_dither_mode || s_dither_live) dither_update();
}
void gl_renderer_set_dithering(int mode) {
    GL_RT_SYNC("dithering");
    s_dither_mode = (mode == 1 || mode == 2) ? mode : 0;
    if (s_ctx) dither_update();
}
int gl_renderer_dithering(void) { return s_dither_mode; }
static GLint s_uAniso = -1;
static void lod_apply_uniforms(void) {
    if (!s_tex_prog) return;
    GLint prev = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prev);
    if (s_uAniso < 0) s_uAniso = p_glGetUniformLocation(s_tex_prog, "u_aniso");
    p_glUseProgram(s_tex_prog); p_glUniform1i(s_uAniso, s_tex_aniso);
    p_glUseProgram((GLuint)prev);
}
void gl_renderer_set_texture_lod(int mode, int aniso) {
    GL_RT_SYNC("texture_lod");
    if (aniso < 1) aniso = 1;
    if (aniso > 16) aniso = 16;
    if (s_ctx) { flush_flat_batch(); flush_tex_batch(); hiw_flush_queue(); }
    s_tex_lod = mode ? 1 : 0;
    s_tex_aniso = aniso;
    if (s_ctx) lod_apply_uniforms();
}
int gl_renderer_texture_lod(void) { return s_tex_lod; }
int gl_renderer_anisotropy(void) { return s_tex_aniso; }
static void glb_set_texture_filter(int b) { s_tex_filter = b >= 0 && b <= 2 ? b : 0; sw_set_texture_filter(b != 0); }
static int  glb_texture_filter(void) { return s_tex_filter; }

static void glb_set_semi_transparency(int e, int m) { s_semi_en = e; s_semi_mode = m & 3; sw_set_semi_transparency(e, m); }
static void glb_set_mask_bits(int s, int c) {
    int next_check = c ? 1 : 0;
    if (next_check != s_mask_check) {
        /* The flat and textured batches take the mask check that is current
         * when they are drawn (mask_stencil reads s_mask_check), so draw what
         * was queued under the old check bit before it changes. */
        flush_flat_batch();
        flush_tex_batch();
    }
    if (next_check && !s_mask_check) {
        /* Land all alpha-authoritative work before deriving stencil from it. */
        flush_cpu_upload();
        rebuild_mask_stencils();
    }
    s_mask_set = s ? 1 : 0;
    s_mask_check = next_check;
    sw_set_mask_bits(s, c);
}
static void glb_set_texture_window(uint32_t r) {
    s_tw_mask_x = (int)(r & 0x1F);
    s_tw_mask_y = (int)((r >> 5) & 0x1F);
    s_tw_off_x  = (int)((r >> 10) & 0x1F);
    s_tw_off_y  = (int)((r >> 15) & 0x1F);
    sw_set_texture_window(r);
}
static void glb_set_color_modulation(int r,int g,int b,int raw) { s_mod_r=r; s_mod_g=g; s_mod_b=b; s_mod_raw=raw; sw_set_color_modulation(r,g,b,raw); }
/* Sub-pixel / perspective overrides for the next triangle. Forwarded to the
 * software rasterizer too: pre-context draws (s_raster_ok == 0) still go there,
 * and the GL backend delegates whole prim classes to it. */
static void glb_set_precise_triangle(int enabled,
                                     int32_t x0,int32_t y0, int32_t x1,int32_t y1,
                                     int32_t x2,int32_t y2) {
    s_pc_valid = enabled ? 1 : 0;
    if (s_pc_valid) {
        /* 16.16 native VRAM px -> float. The GL pipeline is float end-to-end,
         * so the fraction survives to the rasterizer at any internal scale. */
        s_pc_x[0] = (float)x0 / 65536.0f;  s_pc_y[0] = (float)y0 / 65536.0f;
        s_pc_x[1] = (float)x1 / 65536.0f;  s_pc_y[1] = (float)y1 / 65536.0f;
        s_pc_x[2] = (float)x2 / 65536.0f;  s_pc_y[2] = (float)y2 / 65536.0f;
    }
    sw_set_precise_triangle(enabled, x0,y0, x1,y1, x2,y2);
}
static void glb_set_perspective_triangle(int enabled, float q0, float q1, float q2) {
    s_pq_valid = (enabled && q0 > 0.0f && q1 > 0.0f && q2 > 0.0f) ? 1 : 0;
    s_pq[0] = q0; s_pq[1] = q1; s_pq[2] = q2;
    sw_set_perspective_triangle(enabled, q0, q1, q2);
}
static void glb_set_depth_triangle(int enabled, float z0, float z1, float z2) {
    s_pz_valid = (enabled && z0 > 0.0f && z1 > 0.0f && z2 > 0.0f) ? 1 : 0;
    s_pz[0] = z0; s_pz[1] = z1; s_pz[2] = z2;
}
static void glb_set_draw_area(int x1,int y1,int x2,int y2) {
    if (s_depth_used && (x1 != s_area_x1 || y1 != s_area_y1 || x2 != s_area_x2 || y2 != s_area_y2))
        s_depth_need_clear = 1;   /* PGXP depth: a new drawing area starts clean */
    flush_flat_batch(); flush_tex_batch(); s_area_x1=x1; s_area_y1=y1; s_area_x2=x2; s_area_y2=y2; sw_set_draw_area(x1,y1,x2,y2); }
static void glb_get_draw_area(int *x1,int *y1,int *x2,int *y2) { sw_get_draw_area(x1,y1,x2,y2); }
static void glb_set_draw_offset(int x,int y) { flush_flat_batch(); flush_tex_batch(); s_off_x=x; s_off_y=y; sw_set_draw_offset(x,y); }

/* Pre-context draws (s_raster_ok == 0) fall back to the software rasterizer
 * over CPU VRAM; the initial full-VRAM upload at context init folds them in.
 * Offline 15-bit rendering is GPU-only (FBO-auth). Netplay dual-raster and
 * depth24 scanout write SW @ 1× for authority, then GPU @ s_out_scale. Movie
 * players can clear with tiles between movies without leaving depth24; those
 * clears must reach the CPU mirror the RGB888 presenter reads. */
static void depth24_upload_policy(void);
static int cpu_raster_required(void) {
    depth24_upload_policy();
    /* A render pass admitted under dual-raster netplay (title opt-in,
     * render_pass_netplay_enabled) draws a presentation-only image into the
     * GPU surface. The CPU mirror stays the authoritative record of the real
     * frame, so the pass never rasterizes into it. depth24 scanout still
     * presents from the CPU mirror and keeps its write-through. */
    /* A generated (in-between) frame is presentation only, like a pass. */
    return !s_raster_ok || s_depth24_skip_up ||
           ((s_cpu_auth_dual || s_hd_native_authority) && !s_pass_active && !s_fg_drawing);
}
/* Bracket each authoritative draw so a later netplay/session policy change
 * cannot accidentally disable the faithful floor for HD replacement mode. */
static int native_draw_begin(void) {
    int previous = sw_faithful_authority();
    if (s_hd_native_authority) sw_set_faithful_authority(1);
    return previous;
}
static void native_draw_end(int previous) { sw_set_faithful_authority(previous); }
/* The sub-pixel / perspective override describes exactly one triangle; drop it
 * once that triangle has been submitted so a later prim can never inherit it. */
static inline void precise_consumed(void) { s_pc_valid = 0; s_pq_valid = 0; s_projected_uv_valid = 0; s_pz_valid = 0; }

int gl_renderer_projective_supported(void) {
    if (s_rth_on && !rt_on_render_thread() && !rt_held())
        return s_raster_ok && s_rthm_cur && !cpu_vram_authoritative();
    return s_raster_ok && g_wide_cur && !cpu_vram_authoritative();
}
void gl_renderer_draw_projected_triangle(const PSXProjectedVertex vertices[3],
    uint16_t texpage, uint16_t cx, uint16_t cy, int raw, int semi, int perspective) {
    if (rth_record_mode()) {
        uint8_t *p = (uint8_t *)rt_cmd_begin(RTH_PROJ_TRI, rth_prim_flags(),
                                             3u * sizeof(PSXProjectedVertex) + 24u);
        if (p) {
            const int32_t a[6] = { texpage, cx, cy, raw, semi, perspective };
            memcpy(p, vertices, 3u * sizeof(PSXProjectedVertex));
            memcpy(p + 3u * sizeof(PSXProjectedVertex), a, sizeof a);
            rt_cmd_commit();
            return;
        }
        gl_rth_acquire("projected_oversize");
    }
    PSXProjectedVertex polygon[12];
    const double left=fmin(s_area_x1, -wide_dx())-1.0;
    const double right=fmax(s_area_x2+1, g_wide_w-wide_dx())+1.0;
    const int n=psx_projective_clip_triangle(vertices,polygon,
        left,right,s_area_y1-1.0,s_area_y2+1.0);
    int limits[4]={(int)vertices[0].u,(int)vertices[0].v,
                   (int)vertices[0].u,(int)vertices[0].v};
    for (int i=1;i<3;++i) {
        if (vertices[i].u<limits[0]) limits[0]=(int)vertices[i].u;
        if (vertices[i].v<limits[1]) limits[1]=(int)vertices[i].v;
        if (vertices[i].u>limits[2]) limits[2]=(int)vertices[i].u;
        if (vertices[i].v>limits[3]) limits[3]=(int)vertices[i].v;
    }
    for (int k=1;k+1<n;++k) {
        const PSXProjectedVertex v[3]={polygon[0],polygon[k],polygon[k+1]};
        int xs[3],ys[3],us[3],vs[3]; float col[9];
        s_pc_valid=1; s_pq_valid=perspective; s_projected_uv_valid=1;
        for (int i=0;i<3;++i) {
            s_pc_x[i]=(float)(v[i].x/v[i].z);
            s_pc_y[i]=(float)(v[i].y/v[i].z);
            xs[i]=(int)floorf(s_pc_x[i]); ys[i]=(int)floorf(s_pc_y[i]);
            s_pq[i]=(float)(1.0/v[i].z);
            s_projected_u[i]=(float)v[i].u; s_projected_v[i]=(float)v[i].v;
            us[i]=(int)v[i].u; vs[i]=(int)v[i].v;
            col[i*3]=(float)v[i].r; col[i*3+1]=(float)v[i].g; col[i*3+2]=(float)v[i].b;
        }
        gpu_textured_triangle(xs,ys,us,vs,col,texpage,cx,cy,raw,semi,limits);
        precise_consumed();
    }
    precise_consumed();
}

static void glb_draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t col) {
    if (cpu_raster_required()) {
        int faithful = native_draw_begin();
        sw_draw_flat_triangle(x0,y0,x1,y1,x2,y2,col);
        native_draw_end(faithful);
    }
    if (!s_raster_ok) return;
    gpu_triangle(x0,y0,col, x1,y1,col, x2,y2,col, s_semi_en?s_semi_mode:-1);
    precise_consumed();
}
static void glb_draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2) {
    if (cpu_raster_required()) {
        int faithful = native_draw_begin();
        sw_draw_gouraud_triangle(x0,y0,c0,x1,y1,c1,x2,y2,c2);
        native_draw_end(faithful);
    }
    if (!s_raster_ok) return;
    gpu_triangle(x0,y0,c0, x1,y1,c1, x2,y2,c2, s_semi_en?s_semi_mode:-1);
    precise_consumed();
}
static void glb_fill_rect(int x,int y,int w,int h,uint16_t c){
    if (pass_refuse_write("fill", x, y, w, h)) return;
    if (s_depth_used) s_depth_need_clear = 1;   /* PGXP depth: the image under it is gone */
    if (cpu_raster_required()) {
        int faithful = native_draw_begin();
        sw_fill_rect(x,y,w,h,c);
        native_draw_end(faithful);
    }
    if (!s_raster_ok) return;
    gpu_fill(x,y,w,h,c);
}
static void glb_copy_rect(int sx,int sy,int dx,int dy,int w,int h){
    if (pass_refuse_write("copy", dx, dy, w, h)) return;
    if (cpu_raster_required()) {
        int faithful = native_draw_begin();
        sw_copy_rect(sx,sy,dx,dy,w,h);
        native_draw_end(faithful);
    }
    if (!s_raster_ok) return;
    gpu_copy_rect(sx,sy,dx,dy,w,h);
}
static void glb_draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,int x2,int y2,int u2,int v2,uint16_t cx,uint16_t cy,uint16_t tp){
    const int native_required = cpu_raster_required();
    if (native_required && !s_hd_native_authority)
        sw_draw_textured_triangle(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,cx,cy,tp);
    if (s_raster_ok) {
        int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
        float mr=s_mod_r/255.0f, mg=s_mod_g/255.0f, mb=s_mod_b/255.0f;
        float col[9]={mr,mg,mb, mr,mg,mb, mr,mg,mb};
        gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,s_mod_raw, s_semi_en?s_semi_mode:-1, NULL);
    }
    if (native_required && s_hd_native_authority) {
        int faithful = native_draw_begin();
        sw_draw_textured_triangle(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,cx,cy,tp);
        native_draw_end(faithful);
    }
    precise_consumed();
}
static void glb_draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,int x1,int y1,int u1,int v1,uint32_t c1,int x2,int y2,int u2,int v2,uint32_t c2,uint16_t cx,uint16_t cy,uint16_t tp,int raw){
    const int native_required = cpu_raster_required();
    if (native_required && !s_hd_native_authority)
        sw_draw_shaded_textured_triangle(x0,y0,u0,v0,c0,x1,y1,u1,v1,c1,x2,y2,u2,v2,c2,cx,cy,tp,raw);
    if (s_raster_ok) {
        int xs[3]={x0,x1,x2}, ys[3]={y0,y1,y2}, us[3]={u0,u1,u2}, vs[3]={v0,v1,v2};
        uint32_t cc[3]={c0,c1,c2}; float col[9];
        for (int i=0;i<3;i++){ col[i*3+0]=(cc[i]&0xFF)/255.0f; col[i*3+1]=((cc[i]>>8)&0xFF)/255.0f; col[i*3+2]=((cc[i]>>16)&0xFF)/255.0f; }
        gpu_textured_triangle(xs,ys,us,vs,col,tp,cx,cy,raw, s_semi_en?s_semi_mode:-1, NULL);
    }
    if (native_required && s_hd_native_authority) {
        int faithful = native_draw_begin();
        sw_draw_shaded_textured_triangle(x0,y0,u0,v0,c0,x1,y1,u1,v1,c1,x2,y2,u2,v2,c2,cx,cy,tp,raw);
        native_draw_end(faithful);
    }
    precise_consumed();
}
static void glb_draw_flat_rect(int x,int y,int w,int h,uint16_t c){
    if (cpu_raster_required()) {
        int faithful = native_draw_begin();
        sw_draw_flat_rect(x,y,w,h,c);
        native_draw_end(faithful);
    }
    if (!s_raster_ok) return;
    gpu_flat_rect(x,y,w,h,c, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t cx,uint16_t cy,uint16_t tp){
    const int native_required = cpu_raster_required();
    if (native_required && !s_hd_native_authority)
        sw_draw_textured_rect(x,y,w,h,u,v,cx,cy,tp);
    if (s_raster_ok)
        gpu_textured_rect(x,y,w,h, u,v, u+w,v+h, cx,cy,tp, s_semi_en?s_semi_mode:-1);
    if (native_required && s_hd_native_authority) {
        int faithful = native_draw_begin();
        sw_draw_textured_rect(x,y,w,h,u,v,cx,cy,tp);
        native_draw_end(faithful);
    }
}
static void glb_draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t cx,uint16_t cy,uint16_t tp){
    const int native_required = cpu_raster_required();
    if (native_required && !s_hd_native_authority)
        sw_draw_textured_rect_scaled(x,y,w,h,u0,v0,u1,v1,cx,cy,tp);
    if (s_raster_ok)
        gpu_textured_rect(x,y,w,h, u0,v0, u1,v1, cx,cy,tp, s_semi_en?s_semi_mode:-1);
    if (native_required && s_hd_native_authority) {
        int faithful = native_draw_begin();
        sw_draw_textured_rect_scaled(x,y,w,h,u0,v0,u1,v1,cx,cy,tp);
        native_draw_end(faithful);
    }
}
static void glb_draw_line(int x0,int y0,int x1,int y1,uint16_t c){
    if (cpu_raster_required()) {
        int faithful = native_draw_begin();
        sw_draw_line(x0,y0,x1,y1,c);
        native_draw_end(faithful);
    }
    if (!s_raster_ok) return;
    gpu_line(x0,y0,c, x1,y1,c, s_semi_en?s_semi_mode:-1);
}
static void glb_draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1){
    if (cpu_raster_required()) {
        int faithful = native_draw_begin();
        sw_draw_shaded_line(x0,y0,c0,x1,y1,c1);
        native_draw_end(faithful);
    }
    if (!s_raster_ok) return;
    gpu_line(x0,y0,c0, x1,y1,c1, s_semi_en?s_semi_mode:-1);
}
static int gl_read_display_argb(int x, int y, int w, int h, uint32_t *out,
                                int pitch_bytes, int cap_px, int *ow, int *oh);
static int  glb_render_display(uint32_t *o,int p,int dx,int dy,int dw,int dh){ depth24_upload_policy(); ensure_cpu(); return sw_render_display(o,p,dx,dy,dw,dh); }
static int  glb_render_display_hires(uint32_t *o,int p,int dx,int dy,int dw,int dh){ depth24_upload_policy(); ensure_cpu(); return sw_render_display_hires(o,p,dx,dy,dw,dh); }
/* Capture the presented surface at internal resolution (--headless-opengl
 * presentation ring). Read it straight from the GL surface (hr FBO, or the
 * high-resolution window tile) without ever writing CPU VRAM: in dual-raster
 * netplay ensure_cpu deliberately keeps the software mirror, which cannot
 * validate the OpenGL image shown to the player. depth24 scanout presents the
 * CPU mirror (packed RGB888 never reaches the FBO), so that mode, and any rect
 * the GL surface cannot serve, keeps the mirror resolve. The backend's
 * render_display_hires (present fallback, screenshot_hires) is unchanged. */
int gl_renderer_capture_display_hires(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    GL_RT_SYNC("capture_display_hires");
    /* A pitch shorter than one scaled row would make either path below write
     * rows over each other and past a pitch*height buffer. */
    if (!o || dw <= 0 || dh <= 0 || (int64_t)p < (int64_t)dw * s_out_scale * 4)
        return 0;
    depth24_upload_policy();
    if (s_raster_ok && !s_depth24_skip_up && p > 0 && p % 4 == 0) {
        int n = gl_read_display_argb(dx, dy, dw, dh, o, p, INT_MAX, NULL, NULL);
        if (n > 0) return n;
    }
    ensure_cpu();
    return sw_render_display_hires(o, p, dx, dy, dw, dh);
}
/* While GP1 depth24 is on, packed RGB888 lives in the CPU mirror and is
 * presented via gl_renderer_present — never as 1555 FBO texels. Queuing those
 * MDEC A0 rects hits UP_RECTS_MAX (16) and force-flushes mid-movie (MotK intro
 * ~50→~30 FPS). Skip ONLY framebuffer-sized transfers (RGB888); still upload
 * smaller texture A0s so post-FMV menus keep VRAM pages coherent.
 * On leave: clear the skipped FB union in the FBO — do NOT restage CPU RGB888
 * as 1555 (that painted MotK title rainbow/static). */
static int depth24_is_fb_transfer(int x, int y, int w, int h) {
    if (!ctx_depth24() || w <= 0 || h <= 0) return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    int fb_w = (int)((di.width * 3u + 1u) / 2u); /* RGB W → halfwords */
    int fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    /* Classic VRAM texture pages. The area heuristic below treats 256×256 as
     * "half of a 320-wide RGB888 FB" (480×240/2) and would skip staging them
     * during FMV — TM4 post-intro loading text then samples an empty FBO. */
    if (w <= 256 && h <= 256) return 0;
    /* Must target the CRTC scanout band (halfword coords). */
    {
        int dx = (int)(di.display_x & 1023u);
        int dy = (int)(di.display_y & 511u);
        int x0 = x & (VRAM_W - 1), y0 = y & (VRAM_H - 1);
        if (x0 + w <= dx || x0 >= dx + fb_w || y0 + h <= dy || y0 >= dy + fb_h)
            return 0;
    }
    /* MotK: 768×128 class blits; allow slack. */
    if (h >= fb_h - 8 && h <= fb_h + 16 && w >= (fb_w * 3) / 4) return 1;
    if ((int64_t)w * (int64_t)h >= ((int64_t)fb_w * fb_h) / 2) return 1;
    return 0;
}

/* Remember only the depth24 scanout band for clear-on-leave. Used after a
 * full-VRAM savestate restage so texture pages stay in the FBO while the
 * RGB888 movie band is still wiped when GP1 leaves 24-bit (avoids MotK
 * rainbow/static from treating packed RGB as 1555). */
static void depth24_mark_scanout_band(void) {
    GpuDisplayInfo di;
    int fb_w, fb_h, x0, y0, x1, y1;
    if (!ctx_depth24()) return;
    gpu_get_display_info(&di);
    fb_w = (int)((di.width * 3u + 1u) / 2u);
    fb_h = (int)di.height;
    if (fb_w < 8) fb_w = 8;
    if (fb_h < 1) fb_h = 1;
    x0 = (int)(di.display_x & 1023u);
    y0 = (int)(di.display_y & 511u);
    x1 = x0 + fb_w - 1;
    y1 = y0 + fb_h - 1;
    if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
    if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
    rect_add(&s_d24_skip_fb, x0, y0, x1, y1);
}

static void depth24_clear_skipped_fb(void) {
    if (!s_raster_ok || !s_d24_skip_fb.set) return;
    flush_flat_batch();
    flush_tex_batch();
    hiw_flush_queue();
    int x0 = s_d24_skip_fb.x0, y0 = s_d24_skip_fb.y0;
    int x1 = s_d24_skip_fb.x1, y1 = s_d24_skip_fb.y1;
    int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    int S = s_hr_scale;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, VRAM_W * S, VRAM_H * S);
    glScissor(x0 * S, y0 * S, rw * S, rh * S);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    hiw_clear_rect(x0, y0, rw, rh, 0.f, 0.f, 0.f, 0.f, 0);
    if (s_raw_fbo) {
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_raw_fbo);
        glViewport(0, 0, VRAM_W, VRAM_H);
        glScissor(x0, y0, rw, rh);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    rect_add(&s_pack_dirty, x0, y0, x1, y1);
    if (!cpu_vram_authoritative()) {
        rect_add(&s_cpu_dirty, x0, y0, x1, y1);
        s_gpu_dirty = 1; /* The clear must reach an immediate CPU read too. */
    }
    present_dirty_rect(x0, y0, x1, y1, 1);
    rect_clear(&s_d24_skip_fb);
}

static void depth24_upload_policy(void) {
    if (!s_raster_ok) return;
    int d24 = ctx_depth24();
    if (d24 && !s_depth24_skip_up) {
        /* Entering 24-bit: from here the frame is presented from the CPU
         * mirror, and MDEC only writes the movie's own rows. A letterboxed
         * movie leaves the bars untouched, so they scan out whatever the
         * mirror already held; pre-movie primitive clears live only in the FBO
         * until this sync.
         *
         * WipEout 3's intro is 320x192 written inside a 320x240 band. Without
         * syncing here, stale pre-FMV pixels can remain in the CPU mirror's
         * top/bottom bars and flicker between double-buffered movie frames.
         * Cost is one full-VRAM readback per 24-bit entry, not per frame, and
         * ensure_cpu is a no-op when the FBO is already clean. */
        ensure_cpu();
        s_up_nrects = 0;
        rect_clear(&s_d24_skip_fb);
    } else if (!d24 && s_depth24_skip_up) {
        /* GPU writes own the 15-bit image again, including the clear below. */
        s_depth24_skip_up = 0;
        /* Small texture uploads are still queued while depth24 scanout uses
         * the CPU mirror. Clear only the skipped movie band first, then land
         * those newer uploads so texture pages which overlap that band win in
         * PS1 command order. Dropping the queue here loses GT2's boot menu
         * atlas; flushing before the clear would wipe it again. */
        depth24_clear_skipped_fb();
        flush_cpu_upload();
        gpu_depth24_upload_span_reset();
        /* Force a fresh present after FMV→15-bit so menus/loading screens
         * are not held as a stale depth24 frame. */
        gl_renderer_invalidate_present();
    }
    s_depth24_skip_up = d24;
}

static void glb_vram_write(int x,int y,uint16_t px){
    if (pass_refuse_write("poke", x & (VRAM_W-1), y & (VRAM_H-1), 1, 1)) return;
    depth24_upload_policy();
    sw_vram_write(x,y,px);
    /* Point pokes are never MDEC frames — always stage to FBO. */
    up_add(x & (VRAM_W-1), y & (VRAM_H-1), x & (VRAM_W-1), y & (VRAM_H-1));
}
static uint16_t glb_vram_read(int x,int y){ depth24_upload_policy(); ensure_cpu(); return sw_vram_read(x,y); }
static void glb_vram_transfer_in(int x,int y,int w,int h,const uint16_t *d){
    if (pass_refuse_write("upload", x, y, w, h)) return;
    depth24_upload_policy();
    sw_vram_transfer_in(x,y,w,h,d);
    if (s_depth24_skip_up && depth24_is_fb_transfer(x, y, w, h)) {
        /* Full-VRAM restore (boot_state): must stage into the FBO or every
         * texture page outside the movie band is missing after FMV→menus.
         * Only the scanout band is remembered for clear-on-leave — do not
         * union every skipped rect (misclassified texture pages would be
         * wiped from the FBO when leaving depth24). */
        if (w >= VRAM_W && h >= VRAM_H) {
            up_add_transfer(x, y, w, h);
            rect_clear(&s_d24_skip_fb);
            depth24_mark_scanout_band();
            coh_record(GL_COH_UPLOAD, x, y, x + w - 1, y + h - 1);
            return;
        }
        depth24_mark_scanout_band();
        coh_record(GL_COH_UPLOAD, x, y, x+w-1, y+h-1);
        return;
    }
    up_add_transfer(x, y, w, h);   /* exact touched rects, incl. per-pixel wrap */
    coh_record(GL_COH_UPLOAD, x, y, x+w-1, y+h-1);
}
static void glb_vram_transfer_out(int x,int y,int w,int h,uint16_t *d){ depth24_upload_policy(); ensure_cpu(); sw_vram_transfer_out(x,y,w,h,d); }

/* ---- context init / present -------------------------------------------- */
static void upload_present_tex(const uint32_t *pixels, int w, int h, int linear) {
    glBindTexture(GL_TEXTURE_2D, s_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    /* Re-assert clamp every upload: a stale REPEAT wrap samples past the
     * right edge into garbage (MotK FMV right-strip flicker). */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (w != s_present_w || h != s_present_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        s_present_w = w; s_present_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
}

static int s_fmv_filter_cfg = 0;          /* VIDEO_FMV_FILTER_NEAREST */
/* [video] fmv_chroma_smoothing, and whether the frame being presented is a
 * 24-bit (MDEC) scanout (main.cpp notes it before gl_renderer_present). */
static int s_fmv_chroma = 0, s_present_d24 = 0;
static GLint s_present_uChroma = -1;
static uint64_t s_fmv_chroma_frames = 0;
void gl_renderer_set_fmv_chroma_smoothing(int on) {
    GL_RT_SYNC("fmv_chroma");
    s_fmv_chroma = on ? 1 : 0;
}
int gl_renderer_fmv_chroma_smoothing(void) { return s_fmv_chroma; }
uint64_t gl_renderer_fmv_chroma_frames(void) { GL_RT_SYNC("fmv_chroma_stats"); return s_fmv_chroma_frames; }
void gl_renderer_note_present_depth24(int d24) { GL_RT_SYNC("present_d24"); s_present_d24 = d24 ? 1 : 0; }

void gl_renderer_set_fmv_filter(int cfg_value) {
    GL_RT_SYNC("set_fmv_filter");
    if (cfg_value >= 0 && cfg_value <= 3) s_fmv_filter_cfg = cfg_value;
}

static int fmv_filter_mode(void) {
    return s_fmv_filter_cfg - 1;
}

/* Select the present-program sampling mode. s_present_prog is shared by the CPU
 * present and both VRAM/FBO quad paths, and program uniforms persist, so every
 * user states its choice rather than inheriting the last one's. sharp=0 keeps
 * the historical straight sample. */
static void present_set_sharp(int mode, int tex_w, int tex_h,
                              int out_w, int out_h) {
    if (s_present_uSharp < 0) return;
    if (mode <= 0 || tex_w <= 0 || tex_h <= 0) {
        p_glUniform1i(s_present_uSharp, 0);
        return;
    }
    p_glUniform1i(s_present_uSharp, mode);
    if (s_present_uTexSize >= 0)
        p_glUniform2f(s_present_uTexSize, (float)tex_w, (float)tex_h);
    if (s_present_uSharpScale >= 0)
        p_glUniform2f(s_present_uSharpScale,
                      (float)out_w / (float)tex_w,
                      (float)out_h / (float)tex_h);
}

/* The presentation shader is shared by game content, bezel art, host OSD, and
 * hold-last redraws. Uniform state persists across draws, so every draw must
 * explicitly choose whether it is raw game content or an already-composed
 * image. */
static void present_set_gamma(GLint uniform, int apply) {
    if (uniform >= 0)
        p_glUniform1f(uniform, apply ? s_present_gamma : 1.0f);
}

/* Push scanline uniforms into the currently-bound present or interpolation
 * program.
 *   pitch_lines = the height of the TEXTURE that v_uv is normalized against
 *                 (v_uv.y * pitch_lines is the texel-row coordinate, and one
 *                 source texel row is one PS1 scanline). For the VRAM/FBO quad
 *                 that is VRAM_H (v_uv spans only the display sub-range of the
 *                 512-row texture); for the CPU/interp textures, which hold
 *                 exactly the display rect, it equals the display height.
 *   disp_lines  = PS1 display lines actually shown, for the output-scale gate.
 *   out_h       = letterbox height in window px.
 * Pass pitch_lines/disp_lines <= 0 (OSD, bezel, or the already-composed
 * hold-last drawable) to force the effect off — program uniforms persist, so
 * every content draw must state its own choice. */
static void present_set_scanline(GLint uOn, GLint uStr, GLint uLines,
                                 GLint uScale, float pitch_lines, int disp_lines,
                                 int out_h) {
    if (uOn < 0) return;
    int on = (s_scanline_on && pitch_lines > 0.0f && disp_lines > 0 && out_h > 0)
                 ? 1 : 0;
    p_glUniform1i(uOn, on);
    if (!on) return;
    if (uStr >= 0)   p_glUniform1f(uStr, s_scanline_strength);
    if (uLines >= 0) p_glUniform1f(uLines, pitch_lines);
    if (uScale >= 0) p_glUniform1f(uScale, (float)out_h / (float)disp_lines);
}
#define PRESENT_SCANLINE(pitch, disp, oh)                                   \
    present_set_scanline(s_present_uScanline, s_present_uScanStrength,      \
                         s_present_uScanLines, s_present_uScanScale,        \
                         (pitch), (disp), (oh))
#define INTERP_SCANLINE(pitch, disp, oh)                                    \
    present_set_scanline(s_interp_uScanline, s_interp_uScanStrength,        \
                         s_interp_uScanLines, s_interp_uScanScale,          \
                         (pitch), (disp), (oh))

/* Display aspect for the present letterbox. Default 4:3 (native). When a wide
 * aspect is configured the 4:3 frame is stretched into it — paired with the
 * GTE X-squash (gte_set_display_aspect) this nets a wider field of view. */
static int s_aspect_num = 4, s_aspect_den = 3;

void gl_renderer_set_display_aspect(int num, int den) {
    GL_RT_SYNC("set_display_aspect");
    if (num <= 0 || den <= 0) { num = 4; den = 3; }
    s_aspect_num = num; s_aspect_den = den;
}

/* Scanline post-process toggle (host display setting). strength is the depth of
 * the dark gap between lines, 0..1 (0 = off-looking, 1 = fully black gap). The
 * actual darkening is applied per-draw in the present/interpolation shaders and
 * fades in with output scale — see PSX_SCANLINE_FUNC. */
void gl_renderer_set_scanlines(int on, float strength) {
    GL_RT_SYNC("set_scanlines");
    s_scanline_on = on ? 1 : 0;
    if (strength < 0.f) strength = 0.f;
    if (strength > 1.f) strength = 1.f;
    s_scanline_strength = strength;
}

int gl_renderer_get_scanlines(float *strength) {
    if (strength) *strength = s_scanline_strength;
    return s_scanline_on;
}

void gl_renderer_set_post_gamma(float gamma) {
    GL_RT_SYNC("set_post_gamma");
    if (!isfinite(gamma))
        gamma = 1.0f;
    if (gamma < 0.5f) gamma = 0.5f;
    if (gamma > 3.0f) gamma = 3.0f;
    if (fabsf(gamma - 1.0f) < 0.005f) gamma = 1.0f;
    if (gamma == s_present_gamma)
        return;
    s_present_gamma = gamma;

    /* Gamma is presentation state, not guest VRAM state. Invalidate the
     * presentation latches so a hot change is visible on an unchanged frame.
     * Force at least 2 frames so all backbuffers in the swapchain update. */
    for (int i = 0; i < PRES_ROWS; i++) s_present_dirty[i] = ~0ull;
    s_last_present_path = -1;
    s_force_present_remaining = 2;
    hold_invalidate();
}

float gl_renderer_get_post_gamma(void) {
    return s_present_gamma;
}

/* Letterbox: largest num:den rect centered in the drawable. */
static void letterbox_rect_aspect(int ww, int wh, int num, int den,
                                  int *x, int *y, int *w, int *h) {
    int dw = ww, dh = (ww * den) / num;
    if (dh > wh) { dh = wh; dw = (wh * num) / den; }
    *x = (ww - dw) / 2;
    *y = (wh - dh) / 2;
    *w = dw; *h = dh;
}
static void letterbox_rect(int ww, int wh, int *x, int *y, int *w, int *h) {
    letterbox_rect_aspect(ww, wh, s_aspect_num, s_aspect_den, x, y, w, h);
}

static GLuint make_tex(GLenum internal, int w, int h, GLenum fmt, GLenum type) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, fmt, type, NULL);
    return t;
}

static int make_fbo(GLuint *out_fbo, GLuint color_tex, GLuint stencil_rb) {
    p_glGenFramebuffers(1, out_fbo);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *out_fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    if (stencil_rb)
        p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                    PSXGL_RENDERBUFFER, stencil_rb);
    GLenum st = p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER);
    s_last_fbo_status = st;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    if (st != PSXGL_FRAMEBUFFER_COMPLETE) {
        fprintf(stdout, "psxrecomp: GL FBO incomplete (0x%X)\n", st);
        return 0;
    }
    return 1;
}

/* A new hr surface (colour texture, depth-stencil RB, FBO) at scale S for a
 * dynamic-resolution step, depth and stencil cleared. 0 on failure (nothing
 * left allocated). */
static int hr_alloc_surface(int S, GLuint *tex, GLuint *rb, GLuint *fbo) {
    while (glGetError() != GL_NO_ERROR) {}
    int hw = VRAM_W * S, hh = VRAM_H * S;
    *tex = make_tex(GL_RGBA8, hw, hh, GL_RGBA, GL_UNSIGNED_BYTE);
    p_glGenRenderbuffers(1, rb);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, *rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, hw, hh);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, 0);
    int ok = glGetError() == GL_NO_ERROR;
    if (ok) ok = make_fbo(fbo, *tex, *rb);
    if (ok) {
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *fbo);
        glDisable(GL_SCISSOR_TEST);
        glClearStencil(0);
        glClearDepth(1.0);
        glStencilMask(0xFF);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
        ok = glGetError() == GL_NO_ERROR;
    }
    if (!ok) {
        if (*fbo) p_glDeleteFramebuffers(1, fbo);
        if (*tex) glDeleteTextures(1, tex);
        if (*rb) p_glDeleteRenderbuffers(1, rb);
        *fbo = *tex = *rb = 0;
    }
    return ok;
}

/* Release the scale-dependent render targets (hr colour + depth-stencil +
 * copy scratch). Used when an allocation fails and init steps S down. */
static void free_hr_targets(void) {
    if (s_hr_fbo)      { p_glDeleteFramebuffers(1, &s_hr_fbo); s_hr_fbo = 0; }
    if (s_scratch_fbo) { p_glDeleteFramebuffers(1, &s_scratch_fbo); s_scratch_fbo = 0; }
    if (s_hr_rb)       { p_glDeleteRenderbuffers(1, &s_hr_rb); s_hr_rb = 0; }
    if (s_hr_tex)      { glDeleteTextures(1, &s_hr_tex); s_hr_tex = 0; }
    if (s_scratch_tex) { glDeleteTextures(1, &s_scratch_tex); s_scratch_tex = 0; }
    s_scratch_w = s_scratch_h = 0;
}

/* Allocate the authoritative hr surface at scale S. Returns 1 when every
 * target is complete and the driver reported no error (a too-large texture
 * surfaces as GL_INVALID_VALUE or GL_OUT_OF_MEMORY, not always as an
 * incomplete FBO). */
static int alloc_hr_targets(int S) {
    while (glGetError() != GL_NO_ERROR) {}
    int hw = VRAM_W * S, hh = VRAM_H * S;
    s_hr_tex = make_tex(GL_RGBA8, hw, hh, GL_RGBA, GL_UNSIGNED_BYTE);
    /* Copy/stencil scratch. Up to 2x it covers the whole surface as before
     * (<= 8 MiB); above that it starts at one tile and grows to the largest
     * copy on demand (scratch_ensure), instead of costing 4 MiB*S^2 up front. */
    if (S <= 2) { s_scratch_w = hw; s_scratch_h = hh; }
    else {
        s_scratch_w = hw < GL_SCRATCH_TILE ? hw : GL_SCRATCH_TILE;
        s_scratch_h = hh < GL_SCRATCH_TILE ? hh : GL_SCRATCH_TILE;
    }
    s_scratch_tex = make_tex(GL_RGBA8, s_scratch_w, s_scratch_h, GL_RGBA, GL_UNSIGNED_BYTE);
    p_glGenRenderbuffers(1, &s_hr_rb);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_hr_rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, hw, hh);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, 0);
    int ok = glGetError() == GL_NO_ERROR;
    if (ok) ok = make_fbo(&s_hr_fbo, s_hr_tex, s_hr_rb);
    if (ok) ok = make_fbo(&s_scratch_fbo, s_scratch_tex, 0);
    if (ok) ok = glGetError() == GL_NO_ERROR;
    if (!ok) free_hr_targets();
    s_hr_alloc = ok ? S : 0;
    return ok;
}

static int init_gpu_raster(void) {
    /* Effective scale: the request, clamped to what the driver can allocate
     * for the full-VRAM surface and to the memory budget. Logged whenever it
     * differs from the request so a preset that cannot be honoured says so. */
    {
        GLint max_tex = 0, max_rb = 0, max_vp[2] = { 0, 0 };
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        glGetIntegerv(PSXGL_MAX_RENDERBUFFER_SIZE, &max_rb);
        glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_vp);
        while (glGetError() != GL_NO_ERROR) {}
        int lim = max_tex > 0 ? max_tex : 0;
        if (max_rb > 0 && (lim == 0 || max_rb < lim)) lim = max_rb;
        if (max_vp[0] > 0 && (lim == 0 || max_vp[0] < lim)) lim = max_vp[0];
        if (max_vp[1] > 0 && (lim == 0 || max_vp[1] < lim)) lim = max_vp[1];
        s_gl_max_dim = lim;
        {
            /* PSX_GL_MAX_DIM lowers the limit, never raises it: it stands in
             * for a smaller GPU (tests, and checking a layout's fallback). */
            const char *md = getenv("PSX_GL_MAX_DIM");
            int v = md ? atoi(md) : 0;
            if (v > 0 && (s_gl_max_dim == 0 || v < s_gl_max_dim)) s_gl_max_dim = v;
        }
        s_vram_budget = psx_gl_budget_bytes_from_env(getenv("PSX_GL_VRAM_BUDGET_MB"));
        s_out_scale = psx_gl_clamp_full_vram_scale(s_req_scale, GL_MAX_INTERNAL_SCALE,
                                               s_gl_max_dim, s_vram_budget,
                                               &s_scale_clamp_reason);
        s_hr_scale = s_out_scale;
        /* Beyond the full-VRAM limit, keep the full request for the displayed
         * area only: a 1x authoritative surface plus the high-resolution
         * window (see s_hiw). PSX_GL_HIRES_WINDOW=0 disables it (clamp as
         * above); =1 forces it at any scale above 1 (testing). */
        s_hiw = 0;
        memset(s_hiw_t, 0, sizeof s_hiw_t);   /* the old context took the surfaces */
        s_hiw_n = 0;
        s_hiw_last_x0 = s_hiw_last_x1 = 0;
        s_hiw_scratch_tex = s_hiw_scratch_fbo = 0;
        s_hiw_scratch_w = s_hiw_scratch_h = 0;
        s_hiw_refused_logged = 0;
        s_hiw_scratch_refused_logged = 0;
        s_scratch_refused_logged = 0;
        s_hiw_grows = 0;
        {
            const char *env = getenv("PSX_GL_HIRES_WINDOW");
            int force = env && env[0] == '1', allow = !(env && env[0] == '0');
            /* Sized for the smallest framebuffer a window must hold (320). */
            int want = psx_gl_clamp_window_scale(s_req_scale, GL_MAX_INTERNAL_SCALE,
                                                 s_gl_max_dim, s_vram_budget, 320);
            if (allow && want > 1 && (force || want > s_out_scale)) {
                fprintf(stdout, "psxrecomp: GL internal scale %dx: the full-VRAM surface "
                        "stops at %dx here (GPU max texture %d, budget %llu MiB); keeping "
                        "VRAM at 1x and the displayed area at %dx (high-resolution window)\n",
                        s_req_scale, s_out_scale, s_gl_max_dim,
                        (unsigned long long)(s_vram_budget >> 20), want);
                s_hiw = 1;
                s_out_scale = want;
                s_hr_scale = 1;
                s_scale_clamp_reason = want < s_req_scale ? PSX_GL_SCALE_TEXTURE : 0;
            }
        }
        if (!s_hiw && s_out_scale != s_req_scale)
            fprintf(stdout, "psxrecomp: GL internal scale %dx clamped to %dx "
                    "(%s%s%s; GPU max texture %d, budget %llu MiB)\n",
                    s_req_scale, s_out_scale,
                    (s_scale_clamp_reason & PSX_GL_SCALE_TEXTURE) ? "texture limit " : "",
                    (s_scale_clamp_reason & PSX_GL_SCALE_BUDGET) ? "memory budget " : "",
                    (s_scale_clamp_reason & PSX_GL_SCALE_CEILING) ? "ceiling " : "",
                    s_gl_max_dim, (unsigned long long)(s_vram_budget >> 20));
        s_scale_alloc_retries = 0;
    }

    s_geo_prog  = build_program(GEO_VS, GEO_FS);
    s_tex_prog  = build_program_ex(TEX_VS, TEX_FS, 1);
    s_blit_prog = build_program(BLIT_VS, BLIT_FS);
    s_blit_hi_prog = build_program(BLIT_VS_HI, BLIT_FS);
    s_pack_prog = build_program(PACK_VS, PACK_FS);
    s_stencil_prog = build_program(PACK_VS, STENCIL_FS);
    if (!s_geo_prog || !s_tex_prog || !s_blit_prog || !s_pack_prog || !s_stencil_prog ||
        !s_blit_hi_prog) return 0;

    s_conv = (uint32_t *)malloc((size_t)VRAM_W * VRAM_H * sizeof(uint32_t));
    if (!s_conv) return 0;

    s_up_tex      = make_tex(GL_RGBA8, VRAM_W, VRAM_H, GL_RGBA, GL_UNSIGNED_BYTE);
    s_raw_tex     = make_tex(PSXGL_R16UI, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT);
    /* Force the driver's first texture-upload allocation while the renderer is
     * initializing. NVIDIA otherwise defers it until the first MDEC frame,
     * producing a measured ~33 ms glTexSubImage hitch and an audible underrun. */
    {
        const uint32_t zero_rgba = 0;
        const uint16_t zero_raw = 0;
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, &zero_rgba);
        glBindTexture(GL_TEXTURE_2D, s_raw_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1,
                        PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, &zero_raw);
        glFinish();
    }

    /* The hr surface: step the scale down inside GL when the driver cannot
     * allocate it (out of memory, or a limit it under-reported). Only a
     * failure at 1x takes the whole backend down, as before. */
    while (!alloc_hr_targets(s_hr_scale)) {
        if (s_hr_scale <= 1) return 0;
        fprintf(stdout, "psxrecomp: GL hr surface %dx%d (%dx) failed to allocate; "
                "retrying at %dx\n", VRAM_W * s_hr_scale, VRAM_H * s_hr_scale,
                s_hr_scale, s_hr_scale - 1);
        s_hr_scale--;
        s_out_scale = s_hr_scale;
        s_scale_alloc_retries++;
    }
    if (!make_fbo(&s_raw_fbo, s_raw_tex, 0)) return 0;
    if (s_hiw) {
        /* The window's copy staging; grown on demand (hiw_mirror_copy). */
        s_hiw_scratch_w = s_hiw_scratch_h = HIW_ALIGN;
        s_hiw_scratch_tex = make_tex(GL_RGBA8, s_hiw_scratch_w, s_hiw_scratch_h,
                                     GL_RGBA, GL_UNSIGNED_BYTE);
        if (!make_fbo(&s_hiw_scratch_fbo, s_hiw_scratch_tex, 0)) {
            /* Not fatal: window copies then take the 1x source. */
            if (s_hiw_scratch_fbo) p_glDeleteFramebuffers(1, &s_hiw_scratch_fbo);
            glDeleteTextures(1, &s_hiw_scratch_tex);
            s_hiw_scratch_tex = s_hiw_scratch_fbo = 0;
            s_hiw_scratch_w = s_hiw_scratch_h = 0;
        }
    }

    s_uVram  = p_glGetUniformLocation(s_tex_prog, "u_vram");
    s_uPalette = p_glGetUniformLocation(s_tex_prog, "u_palette");
    s_uTpage = p_glGetUniformLocation(s_tex_prog, "u_tpage");
    s_uClut  = p_glGetUniformLocation(s_tex_prog, "u_clut");
    s_uDepth = p_glGetUniformLocation(s_tex_prog, "u_depth");
    s_uRaw   = p_glGetUniformLocation(s_tex_prog, "u_raw");
    s_uSemipass = p_glGetUniformLocation(s_tex_prog, "u_semipass");
    s_uSemimode = p_glGetUniformLocation(s_tex_prog, "u_semimode");
    s_uMaskset  = p_glGetUniformLocation(s_tex_prog, "u_maskset");
    s_uFilter   = p_glGetUniformLocation(s_tex_prog, "u_filter");
    s_uAniso = -1;
    lod_apply_uniforms();
    s_uLimits   = p_glGetUniformLocation(s_tex_prog, "u_limits");
    s_uHdTexture = p_glGetUniformLocation(s_tex_prog, "u_hd_texture");
    s_uBlitSrc     = p_glGetUniformLocation(s_blit_prog, "u_src");
    s_uBlitPass    = p_glGetUniformLocation(s_blit_prog, "u_stp_pass");
    s_uBlitMaskset = p_glGetUniformLocation(s_blit_prog, "u_maskset");
    s_uBlitSrcDiv  = p_glGetUniformLocation(s_blit_prog, "u_src_div");
    s_uBlitSrcOff  = p_glGetUniformLocation(s_blit_prog, "u_src_off");
    s_uPackHr    = p_glGetUniformLocation(s_pack_prog, "u_hr");
    s_uPackScale = p_glGetUniformLocation(s_pack_prog, "u_scale");
    s_uStencilSrc = p_glGetUniformLocation(s_stencil_prog, "u_src");
    s_uStencilOff = p_glGetUniformLocation(s_stencil_prog, "u_off");
    s_uBhSrc     = p_glGetUniformLocation(s_blit_hi_prog, "u_src");
    s_uBhPass    = p_glGetUniformLocation(s_blit_hi_prog, "u_stp_pass");
    s_uBhMaskset = p_glGetUniformLocation(s_blit_hi_prog, "u_maskset");
    s_uBhSrcDiv  = p_glGetUniformLocation(s_blit_hi_prog, "u_src_div");
    s_uBhSrcOff  = p_glGetUniformLocation(s_blit_hi_prog, "u_src_off");
    s_uBhShift   = p_glGetUniformLocation(s_blit_hi_prog, "u_shift");
    s_uBhXoff    = p_glGetUniformLocation(s_blit_hi_prog, "u_xoff");
    s_uBhXhalf   = p_glGetUniformLocation(s_blit_hi_prog, "u_xhalf");
    s_geo_uXoff  = p_glGetUniformLocation(s_geo_prog, "u_xoff");
    s_geo_uXhalf = p_glGetUniformLocation(s_geo_prog, "u_xhalf");
    s_tex_uXoff  = p_glGetUniformLocation(s_tex_prog, "u_xoff");
    s_tex_uXhalf = p_glGetUniformLocation(s_tex_prog, "u_xhalf");
    s_geo_uXscale  = p_glGetUniformLocation(s_geo_prog, "u_xscale");
    s_geo_uXcenter = p_glGetUniformLocation(s_geo_prog, "u_xcenter");
    s_tex_uXscale  = p_glGetUniformLocation(s_tex_prog, "u_xscale");
    s_tex_uXcenter = p_glGetUniformLocation(s_tex_prog, "u_xcenter");
    /* Default the new uniforms to the no-op (1.0 scale, 0 centre) -- GLSL would
     * otherwise zero them, collapsing all x to 0. */
    p_glUseProgram(s_geo_prog);
    p_glUniform1f(s_geo_uXscale, 1.0f); p_glUniform1f(s_geo_uXcenter, 0.0f);
    p_glUseProgram(s_tex_prog);
    p_glUniform1f(s_tex_uXscale, 1.0f); p_glUniform1f(s_tex_uXcenter, 0.0f);

    /* Sample-grid alignment shift: half an HR pixel, set once (S is fixed
     * for the lifetime of the pipeline). Backed off by 1/64 native px so
     * primitive edges never land EXACTLY on sample centers — that float tie
     * dropped 1px columns at quad seams (e.g. the 256px texture-page seam in
     * Tomba's title background). The 1/64 bias keeps floor(uv) on the exact
     * PS1 texel for |uv slope| < 64; mirrored (negative-slope) mappings can
     * be off by one texel at exact-integer uv — accepted. */
    {
        /* hr at s_hr_scale; the high-resolution window and wide surfaces at
         * s_out_scale (the same value unless windowed). */
        float shift = 0.5f / (float)s_hr_scale - 1.0f / 64.0f;
        s_shift_hr = shift;
        s_shift_hi = 0.5f / (float)s_out_scale - 1.0f / 64.0f;
        s_geo_uShift = p_glGetUniformLocation(s_geo_prog, "u_shift");
        s_geo_uDither = s_tex_uDither = -1;
        s_dither_live = s_dither_bit ? s_dither_mode : 0;
        dither_apply_uniforms();
        s_tex_uShift = p_glGetUniformLocation(s_tex_prog, "u_shift");
        s_geo_uZbias = p_glGetUniformLocation(s_geo_prog, "u_zbias");
        s_tex_uZbias = p_glGetUniformLocation(s_tex_prog, "u_zbias");
        p_glUseProgram(s_geo_prog);
        p_glUniform1f(s_geo_uShift, shift);
        /* Native-wide projection defaults: x translation 0, clip half-extent
         * 512 — so the GEO_VS x term reduces to (x+u_shift)/512-1, bit-identical
         * to the pre-native-wide projection. The wide passes set these, then
         * restore these defaults. */
        p_glUniform1f(s_geo_uXoff, 0.0f);
        p_glUniform1f(s_geo_uXhalf, 512.0f);
        p_glUseProgram(s_tex_prog);
        p_glUniform1f(s_tex_uShift, shift);
        p_glUniform1f(s_tex_uXoff, 0.0f);
        p_glUniform1f(s_tex_uXhalf, 512.0f);
        p_glUseProgram(s_blit_prog);
        p_glUniform1f(p_glGetUniformLocation(s_blit_prog, "u_shift"), shift);
        p_glUseProgram(0);
    }

    p_glGenVertexArrays(1, &s_geo_vao);
    p_glBindVertexArray(s_geo_vao);
    p_glGenBuffers(1, &s_geo_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_geo_vbo);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(2 * sizeof(float)));
    p_glEnableVertexAttribArray(1);

    p_glGenVertexArrays(1, &s_tex_vao);
    p_glBindVertexArray(s_tex_vao);
    p_glGenBuffers(1, &s_tex_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_tex_vbo);
    {
        GLsizei st = TEXV * sizeof(float);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, (void*)0);                  p_glEnableVertexAttribArray(0); /* pos    */
        p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (void*)(2*sizeof(float)));  p_glEnableVertexAttribArray(1); /* uv     */
        p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, st, (void*)(4*sizeof(float)));  p_glEnableVertexAttribArray(2); /* col    */
        p_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)(8*sizeof(float)));  p_glEnableVertexAttribArray(3); /* tpage  */
        p_glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, st, (void*)(10*sizeof(float))); p_glEnableVertexAttribArray(4); /* clut   */
        p_glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, st, (void*)(12*sizeof(float))); p_glEnableVertexAttribArray(5); /* depth  */
        p_glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, st, (void*)(13*sizeof(float))); p_glEnableVertexAttribArray(6); /* raw    */
        p_glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, st, (void*)(14*sizeof(float))); p_glEnableVertexAttribArray(7); /* limits */
        p_glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, st, (void*)(18*sizeof(float))); p_glEnableVertexAttribArray(8); /* semi   */
        p_glVertexAttribPointer(9, 1, GL_FLOAT, GL_FALSE, st, (void*)(19*sizeof(float))); p_glEnableVertexAttribArray(9); /* q      */
        p_glVertexAttribPointer(10, 1, GL_FLOAT, GL_FALSE, st, (void*)(20*sizeof(float))); p_glEnableVertexAttribArray(10); /* twin */
        p_glVertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, st, (void*)(21*sizeof(float))); p_glEnableVertexAttribArray(11); /* HD source */
        p_glVertexAttribPointer(12, 1, GL_FLOAT, GL_FALSE, st, (void*)(25*sizeof(float))); p_glEnableVertexAttribArray(12); /* HD alpha mode */
    }

    p_glGenVertexArrays(1, &s_blit_vao);
    p_glBindVertexArray(s_blit_vao);
    p_glGenBuffers(1, &s_blit_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_blit_vbo);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    p_glEnableVertexAttribArray(0);

    p_glGenVertexArrays(1, &s_empty_vao);
    p_glBindVertexArray(0);

    /* Clear the authoritative surface (color + stencil) and queue a full
     * upload of whatever the CPU VRAM already holds (pre-context software
     * draws, BIOS logo state, ...). */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_hr_fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    rect_clear(&s_pack_dirty);
    rect_clear(&s_stencil_stale);
    s_up_nrects = 0;
    up_add(0, 0, VRAM_W - 1, VRAM_H - 1);
    s_gpu_dirty = 0;
    rect_clear(&s_cpu_dirty);
    s_stencil_valid = 1;
    for (int i = 0; i < PRES_ROWS; i++) s_present_dirty[i] = ~0ull;
    s_last_present_path = -1;

    /* Native-wide compositor surfaces start unallocated (lazily created when a
     * widescreen game calls wide_configure + wide_set_target). */
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        s_wide_tex[i] = 0; s_wide_fbo[i] = 0; s_wide_base[i] = -1;
    }
    g_wide_w = 0; g_wide_off = 0; g_wide_cur = 0; g_wide_cur_base = 0;

    s_raster_ok = 1;
    dyn_context_ready();
    gl_perf_init();   /* frame_perf GPU/CPU phase timing (no-op if queries absent) */
    if (s_cpu_auth_dual) {
        fprintf(stdout, "psxrecomp: GL GPU pipeline ready (dual-raster, "
                "internal scale %dx%s, SW@1x authority, no FBO readback)\n",
                s_out_scale, s_hiw ? " windowed" : "");
    } else {
        fprintf(stdout, "psxrecomp: GL GPU pipeline ready (internal scale %dx%s, "
                "mask-bit stencil, texture window, GPU copy/upload)\n", s_out_scale,
                s_hiw ? " windowed" : "");
    }
    return 1;
}

int gl_renderer_texture_banks_supported(void) { return s_raster_ok && !s_cpu_auth_dual && !s_hd_native_authority; }

int gl_renderer_fit_wide_aspect(int disp_w, int *num, int *den) {
    /* No sync point: pure arithmetic on limits that change only while the
     * emulation thread holds the context (main.cpp asks every frame). */
    if (!s_raster_ok || s_gl_max_dim <= 0) return 0;
    /* Against the allocation (the dynamic-resolution ceiling): the aspect
     * must not change when the level steps. */
    int max_w = psx_gl_max_wide_width(alloc_scale_for(s_out_scale), s_gl_max_dim);
    return psx_gl_fit_wide_aspect(disp_w, max_w > 0 ? max_w : 1, num, den);
}

int gl_renderer_scale_info(GlScaleInfo *out) {
    GL_RT_SYNC("scale_info");
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->requested = s_req_scale;
    out->effective = s_raster_ok ? s_out_scale : 0;
    out->max_dim = s_gl_max_dim;
    out->clamp_reason = s_scale_clamp_reason;
    out->alloc_retries = s_scale_alloc_retries;
    out->budget_mib = (int)(s_vram_budget >> 20);
    out->fbo_w = s_raster_ok ? VRAM_W * s_hr_alloc : 0;
    out->fbo_h = s_raster_ok ? VRAM_H * s_hr_alloc : 0;
    out->hr_scale = s_raster_ok ? s_hr_scale : 0;
    out->windowed = s_hiw;
    {
        /* The tile of the last presented display (else the first tile). */
        const HiwTile *T = hiw_tile_holding(s_hiw_last_x0, s_hiw_last_x1);
        if (!T && s_hiw_n) T = &s_hiw_t[0];
        if (T) {
            out->window_x = T->x0;
            out->window_w = T->x1 - T->x0;
            out->window_fbo_w = (T->x1 - T->x0) * s_out_scale;
            out->window_fbo_h = VRAM_H * s_out_scale;
        }
        out->window_tiles = s_hiw_n;
        uint64_t b = 0;
        for (int t = 0; t < s_hiw_n; t++) b += hiw_tile_bytes(s_hiw_t[t].x0, s_hiw_t[t].x1);
        out->window_mib = (int)(b >> 20);
    }
    out->window_grows = s_hiw_grows;
    out->alloc_scale = s_raster_ok ? s_alloc_scale : 0;
    out->max_scale = psx_gl_clamp_full_vram_scale(GL_MAX_INTERNAL_SCALE,
                                                  GL_MAX_INTERNAL_SCALE,
                                                  s_gl_max_dim, s_vram_budget, NULL);
    /* The high-resolution window lifts the ceiling to what one 512-row
     * column of VRAM can hold. */
    if (s_gl_max_dim > 0 && s_gl_max_dim / VRAM_H > out->max_scale) {
        int wmax = s_gl_max_dim / VRAM_H;
        out->max_scale = wmax > GL_MAX_INTERNAL_SCALE ? GL_MAX_INTERNAL_SCALE : wmax;
    }
    if (s_ctx && s_win) SDL_GL_GetDrawableSize(s_win, &out->drawable_w, &out->drawable_h);
    return s_raster_ok;
}

/* Read the display rect at internal resolution straight from the hr FBO:
 * (w*S) x (h*S) ARGB8888, top row first. The CPU-side hires path under GL is
 * native (the software mirror stays 1x), so this is the only capture that
 * shows what the internal resolution actually rendered. cap_px bounds the
 * output; returns the pixel count, 0 when unavailable or too large. */
/* pitch_bytes: output row stride (>= W*4, multiple of 4). Saves and restores
 * the read framebuffer, pixel-pack buffer and pack state, so it is safe from
 * any capture point (present ring, debug server) mid-frame. */
static int gl_read_display_argb(int x, int y, int w, int h, uint32_t *out,
                                int pitch_bytes, int cap_px, int *ow, int *oh) {
    if (!s_raster_ok || !s_ctx || !out || w <= 0 || h <= 0) return 0;
    if (x < 0 || y < 0 || x + w > VRAM_W || y + h > VRAM_H) return 0;
    int S = s_out_scale, W = w * S, H = h * S;
    if ((int64_t)W * H > (int64_t)cap_px) return 0;
    if (pitch_bytes < W * 4 || pitch_bytes % 4) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    GLuint fbo = s_hr_fbo;
    int rx = x;
    if (s_hiw) {
        /* Windowed: the S-scaled pixels exist only inside the tiles. */
        hiw_flush_queue();
        const HiwTile *T = hiw_on() ? hiw_tile_holding(x, x + w) : NULL;
        if (!T) return 0;
        fbo = T->fbo;
        rx = x - T->x0;
    }
    if (!fbo) return 0;
    GLint read_fbo = 0, pack_buffer = 0, pack_row = 0, pack_align = 4;
    glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &read_fbo);
    glGetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &pack_buffer);
    glGetIntegerv(PSXGL_PACK_ROW_LENGTH, &pack_row);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
    p_glBindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(PSXGL_PACK_ROW_LENGTH, pitch_bytes / 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(rx * S, y * S, W, H, GL_BGRA, GL_UNSIGNED_BYTE, out);
    glPixelStorei(PSXGL_PACK_ROW_LENGTH, pack_row);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, (GLuint)read_fbo);
    p_glBindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, (GLuint)pack_buffer);
    /* The hr FBO stores PS1 y=0 at GL row 0 and glReadPixels reads bottom-up,
     * so row 0 is already the display's top line (see glb_render_wide_display). */
    for (int row = 0; row < H; row++) {
        uint32_t *r = (uint32_t *)((uint8_t *)out + (size_t)row * (size_t)pitch_bytes);
        for (int col = 0; col < W; col++) r[col] |= 0xFF000000u;
    }
    if (ow) *ow = W;
    if (oh) *oh = H;
    return W * H;
}

int gl_renderer_read_display_hires(int x, int y, int w, int h, uint32_t *out,
                                   int cap_px, int *ow, int *oh) {
    GL_RT_SYNC("read_display_hires");
    return gl_read_display_argb(x, y, w, h, out, w * s_out_scale * 4,
                                cap_px, ow, oh);
}

int gl_renderer_select_texture_bank(uint16_t id) {
    GL_RT_SYNC("select_texture_bank");
    uint32_t width, height;
    const uint16_t* pixels;
    GLint alignment, row_length;
    s_selected_bank_live_clut = 0;
    if (!id) { s_selected_bank_tex = 0; return 1; }
    if (!gl_renderer_texture_banks_supported()) return 0;
    if (!s_bank_tex[id]) {
        pixels = mod_texture_bank_pixels(id, &width, &height);
        if (!pixels) return 0;
        glGenTextures(1, &s_bank_tex[id]);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_bank_tex[id]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(PSXGL_UNPACK_ROW_LENGTH, &row_length);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, PSXGL_R16UI, (GLsizei)width,
                     (GLsizei)height, 0, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glPixelStorei(PSXGL_UNPACK_ROW_LENGTH, row_length);
        if (glGetError() != GL_NO_ERROR) {
            glDeleteTextures(1, &s_bank_tex[id]); s_bank_tex[id] = 0;
            return 0;
        }
    }
    s_selected_bank_tex = s_bank_tex[id];
    return 1;
}

int gl_renderer_select_texture_bank_live_clut(uint16_t id) {
    GL_RT_SYNC("select_texture_bank_live_clut");
    if (!gl_renderer_select_texture_bank(id)) return 0;
    s_selected_bank_live_clut = id != 0;
    return 1;
}

int gl_renderer_init_context(SDL_Window *win) {
    s_win = win;
    s_present_w = 0;
    s_present_h = 0;
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    s_ctx = SDL_GL_CreateContext(win);
    if (!s_ctx) { fprintf(stdout, "psxrecomp: GL context creation failed (%s)\n", SDL_GetError()); return 0; }
    if (SDL_GL_MakeCurrent(win, s_ctx) != 0) { fprintf(stdout, "psxrecomp: MakeCurrent failed (%s)\n", SDL_GetError()); SDL_GL_DeleteContext(s_ctx); s_ctx=NULL; return 0; }
    /* Swap interval: 1=vsync (tear-free, default), 0=immediate (lowest display
     * latency, may tear; our wall-clock pacer still holds 59.94Hz), -1=adaptive.
     * Adaptive falls back to vsync if the driver rejects it. */
    if (SDL_GL_SetSwapInterval(s_swap_interval) != 0 && s_swap_interval < 0) {
        SDL_GL_SetSwapInterval(1);
        s_swap_interval = 1;
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *gpu = (const char *)glGetString(0x1F01 /* GL_RENDERER */);
    fprintf(stdout, "psxrecomp: OpenGL context created (%s; %s)\n", ver ? ver : "?",
            gpu ? gpu : "?");

    /* All-or-nothing: any missing entry point / failed shader / bad FBO means
     * the whole GL renderer is unavailable and the runtime stays on the pure
     * software path — no half-GL hybrid (that mixed mode is what produced
     * the alternating-present menu jitter). */
    int ok = load_modern_gl();
    if (ok) {
        glGenTextures(1, &s_present_tex);
        glBindTexture(GL_TEXTURE_2D, s_present_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_present_prog = build_program(PRESENT_VS, PRESENT_FS);
        s_interp_prog = build_program(PRESENT_VS, INTERP_FS);
        if (s_present_prog && s_interp_prog) {
            p_glGenVertexArrays(1, &s_present_vao);
            s_present_uTex = p_glGetUniformLocation(s_present_prog, "u_tex");
            s_present_uUvRect = p_glGetUniformLocation(s_present_prog, "u_uv_rect");
            s_present_uTexSize =
                p_glGetUniformLocation(s_present_prog, "u_tex_size");
            s_present_uSharpScale =
                p_glGetUniformLocation(s_present_prog, "u_sharp_scale");
            s_present_uSharp =
                p_glGetUniformLocation(s_present_prog, "u_sharp");
            s_present_uGamma =
                p_glGetUniformLocation(s_present_prog, "u_gamma");
            s_present_uChroma =
                p_glGetUniformLocation(s_present_prog, "u_chroma");
            s_present_uScanline =
                p_glGetUniformLocation(s_present_prog, "u_scanline");
            s_present_uScanStrength =
                p_glGetUniformLocation(s_present_prog, "u_scanline_strength");
            s_present_uScanLines =
                p_glGetUniformLocation(s_present_prog, "u_scanline_lines");
            s_present_uScanScale =
                p_glGetUniformLocation(s_present_prog, "u_scanline_scale");
            s_interp_uScanline =
                p_glGetUniformLocation(s_interp_prog, "u_scanline");
            s_interp_uScanStrength =
                p_glGetUniformLocation(s_interp_prog, "u_scanline_strength");
            s_interp_uScanLines =
                p_glGetUniformLocation(s_interp_prog, "u_scanline_lines");
            s_interp_uScanScale =
                p_glGetUniformLocation(s_interp_prog, "u_scanline_scale");
            s_interp_uPrev = p_glGetUniformLocation(s_interp_prog, "u_prev");
            s_interp_uCurr = p_glGetUniformLocation(s_interp_prog, "u_curr");
            s_interp_uAlpha = p_glGetUniformLocation(s_interp_prog, "u_alpha");
            s_interp_uUvRect = p_glGetUniformLocation(s_interp_prog, "u_uv_rect");
            s_interp_uBlendMode =
                p_glGetUniformLocation(s_interp_prog, "u_blend_mode");
            s_interp_uGamma =
                p_glGetUniformLocation(s_interp_prog, "u_gamma");
            glGenTextures(3, s_interp_tex);
            for (int i = 0; i < 3; i++) {
                glBindTexture(GL_TEXTURE_2D, s_interp_tex[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        } else ok = 0;
    }
    if (ok) ok = init_gpu_raster();
    if (!ok) {
        fprintf(stdout, "psxrecomp: GL pipeline init failed — falling back to software renderer\n");
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
        s_raster_ok = 0;
        return 0;
    }
    return 1;
}

/* Set the GL swap interval (vsync mode): 1=vsync, 0=immediate, -1=adaptive.
 * Safe to call before or after context creation; applies live when a context
 * exists. Adaptive falls back to vsync if unsupported. */
void gl_renderer_set_swap_interval(int interval) {
    GL_RT_SYNC("set_swap_interval");
    s_swap_interval = interval;
    s_pt_interval_gen++;   /* the present thread's context applies it */
    if (s_ctx) {
        if (SDL_GL_SetSwapInterval(interval) != 0 && interval < 0) {
            SDL_GL_SetSwapInterval(1);
            s_swap_interval = 1;
        }
    }
}
int gl_renderer_get_swap_interval(void) {
    GL_RT_SYNC("get_swap_interval");
    if(!s_ctx)return -2;
    if(s_pt_on)return s_swap_interval;
#if defined(PSX_SDL3)
    int interval=0;
    return SDL_GL_GetSwapInterval(&interval)?interval:-2;
#else
    return SDL_GL_GetSwapInterval();
#endif
}

static void pass_resources_release(void);

void gl_renderer_shutdown(void) {
    gl_renderer_render_thread_stop();   /* the context comes back to this thread */
    s_native_surface_enabled=s_native_surface_pending=0;
    gl_renderer_clear_hd_texture_cache();
    pass_resources_release();
    if (s_ctx) {
        for (unsigned i = 1; i < 65536u; ++i)
            if (s_bank_tex[i]) glDeleteTextures(1, &s_bank_tex[i]);
        /* Bezel artwork belongs to the session that loaded it: a lobby
         * rematch creates a new context and loads its own artwork (or none),
         * and present_bezel() must not bind this name in that context. */
        gl_renderer_set_bezel(NULL, 0, 0);
    }
    memset(s_bank_tex, 0, sizeof s_bank_tex);
    s_selected_bank_tex = s_tb_bank_tex = 0;
    s_selected_bank_live_clut = s_tb_bank_live_clut = 0;
    if (s_ctx) {
        ensure_cpu();
        SDL_GL_DeleteContext(s_ctx); s_ctx = NULL;
    }
    free(s_conv); s_conv = NULL;
    s_hq_n = 0; s_hq_vn = 0;   /* queued window draws die with the context */
    s_raster_ok = 0;
    /* New context regenerates s_present_tex empty; a stale size makes
     * upload_present_tex take glTexSubImage2D into an unallocated texture
     * (rematch 24-bit FMV → black picture, audio still runs). */
    s_present_w = 0;
    s_present_h = 0;
    s_osd_tex = 0;
    s_osd_tw = 0;
    s_osd_th = 0;
    s_depth24_skip_up = 0;
    rect_clear(&s_d24_skip_fb);
    hold_invalidate();
    s_hold_tex = 0;
    s_hold_fbo = 0;
    s_hold_tw = 0;
    s_hold_th = 0;
    s_interp_fbo = 0;   /* died with the context */
    post_aa_release();
}

/* CPU-readout present (24-bit FMV frames and the PSX_GL_FORCE_CPU_PRESENT
 * diagnostic): full-window clear, then a quad into the letterbox rect.
 * force_4_3 pins the rect to native 4:3 regardless of the display aspect —
 * FMVs are authored 4:3 and have no GTE squash to compensate a stretch, so
 * widescreen presents them pillarboxed instead of distorted. */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear,
                         int force_4_3, int content_w) {
    GL_RT_SYNC("present");
    if (!s_ctx) return;
    interp_reset_history();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    int lx, ly, lw, lh;
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    /* Short GP1(07h) bands (MotK FMV is 128 lines) only fill a fraction of
     * NTSC active height on hardware. Stretching them to the full letterbox
     * doubles vertical scale vs horizontal and makes the frame look too wide
     * with the right edge clipped. Letterbox within the present rect instead.
     * Apply whenever the source is short — not only when force_4_3 — so a
     * misclassified FMV frame still keeps correct pixel aspect. */
    /* Genuinely windowed video bands only (<80% of the 240-line field, e.g.
     * MotK's 128-line FMV). A game's native short display mode (216/224)
     * fills the rect as on hardware. */
    if (src_h > 0 && src_h < 192) {
        int content_h = (lh * src_h) / 240;
        if (content_h < 1) content_h = 1;
        ly += (lh - content_h) / 2;
        lh = content_h;
    }
    /* Optional trailing-column crop (depth24 margin): shrink the draw width
     * left-aligned so cleared black remains on the right — never stretch. */
    float uv_x1 = 1.f;
    int crop = (content_w > 0 && content_w < src_w && src_w > 0);
    if (crop) {
        uv_x1 = (float)content_w / (float)src_w;
        lw = (lw * content_w) / src_w;
        if (lw < 1) lw = 1;
    }
    /* This is the low-res source path (24-bit FMV, and the forced-CPU present
     * diagnostic): a 320x192-class image blown up to fill the window, so how it
     * is reconstructed is very visible. `linear` (the video AA setting) allows
     * filtered reconstruction when [video] fmv_filter opts into it:
     *
     *   nearest   hard pixels, uneven pixel widths at non-integer scale
     *   bilinear  plain GL_LINEAR — smoothest, but blurs the whole texel
     *   sharp     sharp-bilinear: flat texel interiors, ramp confined to a
     *             one-output-pixel band at the boundary
     *   bicubic   Catmull-Rom
     *
     * Measured on this intro at 1280x960 (fraction of adjacent pixel pairs
     * differing by >=24 luma = visible staircase, vs mean |dx| = overall
     * sharpness): nearest 1.00%/1.028, sharp 0.87%/1.008, bicubic 0.34%/1.038,
     * bilinear 0.14%/0.930. Bicubic removes two thirds of the staircase while
     * holding gradient at the nearest level; bilinear removes the most but
     * costs 10% of it, which reads as blur. Still a taste call, hence the knob. */
    int filt_mode = linear ? fmv_filter_mode()
                           : -1;          /* AA off: nearest, no shader work */
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    upload_present_tex(pixels, src_w, src_h, filt_mode >= 0 ? 1 : 0);
    p_glUseProgram(s_present_prog); p_glUniform1i(s_present_uTex, 0);
    present_set_gamma(s_present_uGamma, 1);
    present_set_sharp(filt_mode, src_w, src_h, lw, lh);
    /* CPU present texture holds exactly the display rect, so v_uv spans it and
     * pitch == display height == src_h. */
    PRESENT_SCANLINE(src_h, src_h, lh);
    if (crop) {
        /* Cropped present keeps left-aligned content; still inset so linear
         * AA does not blend the cut column with undefined border texels. */
        float u0 = (src_w > 0) ? (0.5f / (float)src_w) : 0.f;
        float v0 = (src_h > 0) ? (0.5f / (float)src_h) : 0.f;
        p_glUniform4f(s_present_uUvRect, u0, v0, uv_x1 - u0, 1.f - v0);
    } else if (src_w > 0 && src_h > 0) {
        /* Half-texel UV inset for both nearest and linear. Corner-mapped
         * UV=1.0 grazes past the last texel (driver-dependent border sample);
         * with GL_LINEAR that also blends an edge stripe into the image.
         * Matches present_target_quad / MotK present UV edge-bleed fix. */
        float u0 = 0.5f / (float)src_w, v0 = 0.5f / (float)src_h;
        p_glUniform4f(s_present_uUvRect, u0, v0, 1.f - u0, 1.f - v0);
    } else {
        p_glUniform4f(s_present_uUvRect, 0.f, 0.f, 1.f, 1.f);
    }
    const int chroma = s_fmv_chroma && s_present_d24;
    if (chroma) {
        /* present_set_sharp only sets the source size for filtered modes; the
         * chroma tent needs it in every mode. */
        p_glUniform1i(s_present_uChroma, 1);
        if (s_present_uTexSize >= 0) p_glUniform2f(s_present_uTexSize, (float)src_w, (float)src_h);
        s_fmv_chroma_frames++;
    }
    p_glBindVertexArray(s_present_vao); glDrawArrays(GL_TRIANGLES, 0, 3);
    if (chroma) p_glUniform1i(s_present_uChroma, 0);
    p_glBindVertexArray(0); p_glUseProgram(0);
    pres_record(GL_PRES_CPU, 0, 0, src_w, src_h, lx, ly, lw, lh);
    hold_capture_drawable();
    latency_ring_mark(LAT_SWAP_BEGIN);
    gl_swap_with_osd();
    latency_ring_mark(LAT_SWAP_END);
    s_probe_swap++;
    present_force_consumed();
    s_last_present_path = GL_PRES_CPU;
}

void gl_renderer_present_blank(void) {
    GL_RT_SYNC("present_blank");
    if (!s_ctx) return;
    interp_reset_history();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh); glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    pres_record(GL_PRES_BLANK, 0, 0, 0, 0, 0, 0, ww, wh);
    hold_capture_drawable();
    latency_ring_mark(LAT_SWAP_BEGIN);
    gl_swap_with_osd();
    latency_ring_mark(LAT_SWAP_END);
    s_probe_swap++;
    present_force_consumed();
    s_last_present_path = GL_PRES_BLANK;
}

/* Sync the authoritative FBO down into CPU VRAM (no-op when current).
 * Screenshots / debug server. Not for 24-bit FMV scanout (see flush). */
void gl_renderer_sync_cpu(void) {
    GL_RT_SYNC("sync_cpu");
    ensure_cpu();
}

void gl_renderer_invalidate_present(void) {
    GL_RT_SYNC("invalidate_present");
    stereo_invalidate();
    for (int i = 0; i < PRES_ROWS; i++) s_present_dirty[i] = ~0ull;
    s_last_present_path = -1;
    s_force_present_remaining = 8;
    hold_invalidate();
    interp_reset_history();
}

void gl_renderer_restage_vram_after_savestate(void) {
    GL_RT_SYNC("restage_vram_after_savestate");
    gpu_hd_textures_restage();
    if (!s_raster_ok || !s_vram) return;
    /* Belt-and-suspenders after boot_state VRAM apply: force CPU mirror → FBO
     * even if a depth24 skip swallowed the restore, then re-arm scanout-band
     * clear so leaving FMV does not keep RGB888-as-1555 junk. */
    s_up_nrects = 0;
    rect_clear(&s_d24_skip_fb);
    s_depth24_skip_up = 0;
    up_add_transfer(0, 0, VRAM_W, VRAM_H);
    flush_cpu_upload();
    if (ctx_depth24()) {
        s_depth24_skip_up = 1;
        depth24_mark_scanout_band();
    }
    /* Dual-raster: CPU is authority after snap; FBO is cosmetics only. */
    if (s_cpu_auth_dual || s_hd_native_authority) {
        s_gpu_dirty = 0;
        rect_clear(&s_cpu_dirty);
    }
}

void gl_renderer_set_cpu_auth_dual(int on) {
    GL_RT_SYNC("set_cpu_auth_dual");
    s_cpu_auth_dual = on ? 1 : 0;
    if (s_cpu_auth_dual) {
        s_gpu_dirty = 0;
        rect_clear(&s_cpu_dirty);
    }
}

void gl_renderer_set_hd_texture_mode(int on) {
    /* Drain the GPU-authoritative stream before switching authority. */
    GL_RT_SYNC("set_hd_texture_mode");
    on = on ? 1 : 0;
    s_hd_authority_pending = 0;
    if (on && !s_hd_native_authority && s_rth_a0_open && s_raster_ok) {
        s_hd_authority_pending = 1;
        return;
    }
    if (on == s_hd_native_authority) return;
    if (s_raster_ok) {
        flush_flat_batch(); flush_tex_batch(); flush_cpu_upload(); hiw_flush_queue();
        if (on) ensure_cpu();
        else {
            /* Return GPU authority only after replacing the presentation
             * image with native VRAM; an HD frame must never be read back. */
            gl_renderer_restage_vram_after_savestate();
        }
    }
    s_hd_native_authority = on;
    /* An A0 still streaming keeps its hold (s_rth_a0_open): the context was
     * just taken above, and eligibility keeps it until the commit. */
    /* Smooth motion restarts its lists: their draws resolve textures under
     * the authority they were recorded in. */
    s_fg_broken = 1;
    s_gpu_dirty = 0; rect_clear(&s_cpu_dirty);
    if (on) s_selected_bank_tex = 0;
}

int gl_renderer_cpu_auth_dual(void) {
    return s_cpu_auth_dual;
}

void gl_renderer_present_probe_reset(void) {
    GL_RT_SYNC("present_probe_reset");
    s_probe_skip = 0;
    s_probe_swap = 0;
    s_probe_dirty_marks = 0;
}

void gl_renderer_present_probe_take(uint64_t *skip_delta, uint64_t *swap_delta,
                                    uint64_t *dirty_mark_delta,
                                    int *force_remaining) {
    GL_RT_SYNC("present_probe_take");
    if (skip_delta) { *skip_delta = s_probe_skip; s_probe_skip = 0; }
    if (swap_delta) { *swap_delta = s_probe_swap; s_probe_swap = 0; }
    if (dirty_mark_delta) {
        *dirty_mark_delta = s_probe_dirty_marks;
        s_probe_dirty_marks = 0;
    }
    if (force_remaining) *force_remaining = s_force_present_remaining;
}

int gl_renderer_present_rect_dirty(int disp_x, int disp_y, int w, int h) {
    GL_RT_SYNC("present_rect_dirty");
    if (!s_raster_ok || w <= 0 || h <= 0) return 0;
    return present_dirty_test(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1);
}

void gl_renderer_flush_cpu_uploads(void) {
    GL_RT_SYNC("flush_cpu_uploads");
    if (!s_raster_ok) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
}

/* Diagnostic (debug server "gl_fbo_peek"): read a rect of the GPU-side
 * authoritative VRAM (via the pack pass + raw mirror) WITHOUT writing CPU
 * VRAM — lets a probe diff FBO truth against CPU truth. Returns 0 when the
 * GL pipeline is inactive (software backend). */
int gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out) {
    GL_RT_SYNC("fbo_peek");
    if (!s_raster_ok || !s_ctx) return 0;
    if (x < 0 || y < 0 || w < 1 || h < 1 ||
        x + w > VRAM_W || y + h > VRAM_H) return 0;
    flush_cpu_upload();
    rect_add(&s_pack_dirty, x, y, x + w - 1, y + h - 1);
    pack_flush();
    coh_record(GL_COH_PEEK, x, y, x + w - 1, y + h - 1);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);
    glReadPixels(x, y, w, h, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, out);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    return 1;
}

/* fbo_peek at this point of the command stream without a sync point: with
 * the render thread recording, the readback is queued and lands in out when
 * the render thread reaches it (a reader of out must sync first). Otherwise
 * it is gl_renderer_fbo_peek. Used by the per-frame display ring. */
int gl_renderer_fbo_peek_deferred(int x, int y, int w, int h, uint16_t *out) {
    if (rth_record_mode()) {
        if (!s_raster_ok || !s_ctx || x < 0 || y < 0 || w < 1 || h < 1 ||
            x + w > VRAM_W || y + h > VRAM_H) return 0;
        uint64_t ptr = (uint64_t)(uintptr_t)out;
        RTH_REC(RTH_PEEK, 0, x, y, w, h, (int32_t)(ptr & 0xFFFFFFFFu), (int32_t)(ptr >> 32));
        return 1;
    }
    return gl_renderer_fbo_peek(x, y, w, h, out);
}

/* Diagnostic (debug server "gl_vram_diff"): full-VRAM comparison of the
 * GPU-side truth (FBO via pack) against the CPU array, WITHOUT writing
 * either. Reports mismatch count + bounding box + a few sample coords.
 * Divergence is expected where the GPU is legitimately ahead (gpu_dirty);
 * at upload-only scenes the two must match exactly. */
int gl_renderer_vram_diff(uint32_t *count, int bbox[4],
                          int samples[8][2], uint16_t samples_px[8][2]) {
    GL_RT_SYNC("vram_diff");
    if (!s_raster_ok || !s_ctx) return 0;
    uint16_t *tmp = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * 2);
    if (!tmp) return 0;
    flush_cpu_upload();
    /* Force a full pack: the diff must read FBO truth even where the
     * raw-mirror invariant (raw == FBO outside s_pack_dirty) is broken —
     * a broken invariant is exactly what this tool hunts. */
    rect_add(&s_pack_dirty, 0, 0, VRAM_W - 1, VRAM_H - 1);
    pack_flush();
    coh_record(GL_COH_DIFF, 0, 0, VRAM_W - 1, VRAM_H - 1);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glReadPixels(0, 0, VRAM_W, VRAM_H, PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    uint32_t n = 0;
    int x0 = VRAM_W, y0 = VRAM_H, x1 = -1, y1 = -1, ns = 0;
    for (int y = 0; y < VRAM_H; y++) {
        for (int x = 0; x < VRAM_W; x++) {
            uint16_t f = tmp[y * VRAM_W + x], c = s_vram[y * VRAM_W + x];
            if (f == c) continue;
            n++;
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
            if (ns < 8 && (n % 977) == 1) {  /* spread samples */
                samples[ns][0] = x; samples[ns][1] = y;
                samples_px[ns][0] = f; samples_px[ns][1] = c;
                ns++;
            }
        }
    }
    free(tmp);
    *count = n;
    bbox[0] = x0; bbox[1] = y0; bbox[2] = x1; bbox[3] = y1;
    return 1 + ns;  /* >=1 means valid; ns = samples filled */
}

/* Diagnostic state for the debug server: coherency flags + dirty rects. */
void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5]) {
    GL_RT_SYNC("diag");
    if (gpu_dirty) *gpu_dirty = s_gpu_dirty;
    if (pending) {
        /* [0] = pending rect count; [1..4] = union bbox (diagnostic only —
         * the flush itself paints the exact rects, never this union). */
        pending[0] = s_up_nrects;
        pending[1] = pending[2] = pending[3] = pending[4] = 0;
        for (int i = 0; i < s_up_nrects; i++) {
            if (i == 0) {
                pending[1] = s_up_rects[i].x0; pending[2] = s_up_rects[i].y0;
                pending[3] = s_up_rects[i].x1; pending[4] = s_up_rects[i].y1;
            } else {
                if (s_up_rects[i].x0 < pending[1]) pending[1] = s_up_rects[i].x0;
                if (s_up_rects[i].y0 < pending[2]) pending[2] = s_up_rects[i].y0;
                if (s_up_rects[i].x1 > pending[3]) pending[3] = s_up_rects[i].x1;
                if (s_up_rects[i].y1 > pending[4]) pending[4] = s_up_rects[i].y1;
            }
        }
    }
    if (pack) {
        pack[0] = s_pack_dirty.set;
        pack[1] = s_pack_dirty.x0; pack[2] = s_pack_dirty.y0;
        pack[3] = s_pack_dirty.x1; pack[4] = s_pack_dirty.y1;
    }
}

/* ------------------------------------------------------------------------- *
 * Native-wide compositor (GL). Mirrors gpu_sw_renderer.c's wide functions:
 * canonical VRAM (the hr FBO) is untouched; framebuffer draws are also mirrored
 * into per-base_x wide FBOs (see the draw funcs' wide passes). Present reads the
 * displayed buffer's wide FBO via glReadPixels into the CPU present buffer, then
 * the existing CPU present path uploads/letterboxes it (Option B: reuse the CPU
 * present path). Self-gates on the GL pipeline being live; never runs for
 * 4:3 / non-opted games (gpu.c never calls wide_configure for those).
 * ------------------------------------------------------------------------- */

static void wide_free_all(void) {
    if (s_ctx) hiw_flush_queue();   /* queued mirrors target these surfaces */
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (s_wide_fbo[i]) { p_glDeleteFramebuffers(1, &s_wide_fbo[i]); s_wide_fbo[i] = 0; }
        if (s_wide_tex[i]) { glDeleteTextures(1, &s_wide_tex[i]); s_wide_tex[i] = 0; }
        if (s_wide_rb[i])  { p_glDeleteRenderbuffers(1, &s_wide_rb[i]); s_wide_rb[i] = 0; }
        s_wide_base[i] = -1;
        s_wide_as[i] = 0;
        wst_clear(i);
    }
    g_wide_cur = 0;
}

/* Create a native-wide surface (colour texture, depth-stencil RB, FBO) at
 * scale S, cleared to black with a zero stencil. 0 on failure (nothing left
 * allocated). */
static int wide_alloc_surface(int S, GLuint *tex, GLuint *rb, GLuint *fbo) {
    int w = g_wide_w * S, h = VRAM_H * S;
    s_cw_fbo_creates++;
    *tex = make_tex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    /* Depth-stencil RB, same as the hr FBO: the stencil carries the PSX
     * mask-bit mirror for the wide surface, and (the hard lesson) a
     * stencil-less FBO turns every stencil-enabled mirror draw into ~0.6ms of
     * driver-side work — the 16:9 GL perf collapse. */
    p_glGenRenderbuffers(1, rb);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, *rb);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, w, h);
    if (!make_fbo(fbo, *tex, *rb)) {
        glDeleteTextures(1, tex); *tex = 0;
        p_glDeleteRenderbuffers(1, rb); *rb = 0;
        *fbo = 0;
        return 0;
    }
    /* Clear to black so unwritten margins are clean (not stale). */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    return 1;
}

/* Lazily allocate (or find) the wide FBO+tex for base_x. Returns the FBO id, or
 * 0 on failure / more distinct buffers than WIDE_MAX_SURF. */
static GLuint wide_fbo_for(int base_x) {
    if (g_wide_w <= 0) return 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) return s_wide_fbo[i];
    /* A native-wide surface is g_wide_w*S wide, and the driver limit bounds
     * it: on a 16384 limit, 1024 native columns at 16x (about 38:9 at a
     * 320-px display) and 910 at 18x in the window mode (about 34:9). Fit to
     * Window narrows its aspect to fit (gl_renderer_fit_wide_aspect,
     * main.cpp); a fixed aspect on a wide display mode can still ask for more.
     * Refuse that with one log line instead of failing mid-draw: the frame
     * then presents 4:3 through the CPU path at 1x, without the native-wide
     * margins. */
    /* Dynamic resolution: the limit is checked at the ceiling
     * (alloc_scale_for), which every level may step up to. */
    const int AS = alloc_scale_for(s_out_scale);
    if (s_gl_max_dim > 0 &&
        ((int64_t)g_wide_w * AS > s_gl_max_dim ||
         (int64_t)VRAM_H * AS > s_gl_max_dim)) {
        if (!s_wide_refused_logged) {
            fprintf(stdout, "psxrecomp: GL native-wide surface %dx%d at %dx exceeds "
                    "the GPU limit %d; presenting 4:3\n", g_wide_w * AS,
                    VRAM_H * AS, AS, s_gl_max_dim);
            s_wide_refused_logged = 1;
        }
        return 0;
    }
    for (int i = 0; i < WIDE_MAX_SURF; i++) {
        if (!s_wide_fbo[i]) {
            /* Allocated at the level it renders at; a dynamic-resolution step
             * reallocates it (dyn_apply). The limit above is checked at the
             * ceiling, so no step can exceed it. */
            if (!wide_alloc_surface(s_out_scale, &s_wide_tex[i], &s_wide_rb[i],
                                    &s_wide_fbo[i]))
                return 0;
            s_wide_as[i] = s_out_scale;
            s_wide_base[i] = base_x;
            s_dyn_wide_y0[i] = s_dyn_wide_y1[i] = 0;   /* nothing presented yet */
            wst_clear(i);   /* colour and stencil cleared together */
            return s_wide_fbo[i];
        }
    }
    return 0;  /* more distinct buffers than WIDE_MAX_SURF — shouldn't happen */
}

/* Enable native-wide with a wide width + centering offset (native px), or
 * disable (wide_w <= 0). Re-allocates if the width changed. Mirrors
 * sw_wide_configure. */
static void glb_wide_set_view(int enabled, int shift, int pad_left, int pad_right) {
    if (!enabled) shift = pad_left = pad_right = 0;
    if (view_enabled == enabled && view_shift == shift &&
        view_pad_left == pad_left && view_pad_right == pad_right) return;
    flush_flat_batch(); flush_tex_batch();
    view_enabled = enabled;
    view_shift = shift;
    view_pad_left = pad_left;
    view_pad_right = pad_right;
}

static void glb_wide_configure(int wide_w, int offset) {
    if (!s_raster_ok) return;
    double t0 = cw_ms(); s_cw_wide_cfgs++;
    flush_line_batch();
    flush_tex_batch();   /* a queued batch's wide mirror targets the CURRENT surfaces */
    hiw_flush_queue();   /* ...and so does a windowed-mode queued one */
    if (wide_w <= 0) { wide_free_all(); g_wide_w = 0; g_wide_off = 0; s_cw_wide_ms += cw_ms() - t0; return; }
    if (wide_w != g_wide_w) { wide_free_all(); s_wide_refused_logged = 0; }
    g_wide_w = wide_w;
    g_wide_off = offset;
    s_cw_wide_ms += cw_ms() - t0;
}

/* Select the wide surface to mirror into for the back buffer at base_x. */
static void glb_wide_set_target(int base_x) {
    if (!s_raster_ok) { g_wide_cur = 0; return; }
    double t0 = cw_ms(); s_cw_wide_sets++;
    flush_flat_batch();  /* drain into the OLD target before switching */
    flush_tex_batch();
    g_wide_cur = wide_fbo_for(base_x);
    g_wide_cur_base = base_x;
    s_cw_wide_ms += cw_ms() - t0;
}

/* Stop mirroring (offscreen draws that don't target a framebuffer). */
static void glb_wide_disable_target(void) { flush_flat_batch(); flush_tex_batch(); g_wide_cur = 0; }

/* Mirror a framebuffer clear: fill the full wide width over [y, y+h) of the
 * surface for base_x, so the revealed margins are clean. Mirrors sw_wide_clear:
 * a scissored glClear with the 1555 color converted to RGBA8 (alpha = bit15). */
static void glb_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (!s_raster_ok || s_ws_ablate == 1) return;
    double t0 = cw_ms(); s_cw_wide_clears++;
    flush_tex_batch();
    hiw_flush_queue();   /* queued wide mirrors land before the clear */
    GLuint fbo = wide_fbo_for(base_x);
    if (!fbo) { s_cw_wide_ms += cw_ms() - t0; return; }
    gl_perf_mirror_begin();
    int H = VRAM_H * s_out_scale;
    int y0 = y * s_out_scale, y1 = (y + h) * s_out_scale;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0) return;
    float r = (color & 0x1F) / 31.0f;
    float g = ((color >> 5) & 0x1F) / 31.0f;
    float b = ((color >> 10) & 0x1F) / 31.0f;
    float a = (color >> 15) & 1 ? 1.0f : 0.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    glViewport(0, 0, g_wide_w * s_out_scale, H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, y0, g_wide_w * s_out_scale, y1 - y0);
    glClearColor(r, g, b, a);
    glClearStencil((color >> 15) & 1);   /* stencil mirrors bit15, like the color alpha */
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    gl_perf_mirror_end();
    s_cw_wide_ms += cw_ms() - t0;
}

/* Clear only the two synthetic reveal strips, preserving the centred canonical
 * framebuffer. This is an opt-in transition cleanup driven by gpu.c. */
static void glb_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (!s_raster_ok || s_ws_ablate == 1 || g_wide_off <= 0) return;
    double t0 = cw_ms(); s_cw_wide_clears++;
    flush_flat_batch();
    flush_tex_batch();
    hiw_flush_queue();   /* queued wide mirrors land before the clear */
    GLuint fbo = wide_fbo_for(base_x);
    if (!fbo) { s_cw_wide_ms += cw_ms() - t0; return; }
    gl_perf_mirror_begin();
    int H = VRAM_H * s_out_scale;
    int W = g_wide_w * s_out_scale;
    int margin = g_wide_off * s_out_scale;
    int y0 = y * s_out_scale, y1 = (y + h) * s_out_scale;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y1 <= y0 || margin * 2 >= W) {
        gl_perf_mirror_end();
        s_cw_wide_ms += cw_ms() - t0;
        return;
    }
    float r = (color & 0x1F) / 31.0f;
    float g = ((color >> 5) & 0x1F) / 31.0f;
    float b = ((color >> 10) & 0x1F) / 31.0f;
    float a = (color >> 15) & 1 ? 1.0f : 0.0f;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClearStencil((color >> 15) & 1);
    glStencilMask(0xFF);
    if (sides & 1) {
        glScissor(0, y0, margin, y1 - y0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    if (sides & 2) {
        glScissor(W - margin, y0, margin, y1 - y0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    gl_perf_mirror_end();
    s_cw_wide_ms += cw_ms() - t0;
}

/* Present source: read the wide FBO for the displayed buffer (base_x) into the
 * CPU present buffer as ARGB8888, byte-identical to sw_render_wide_display's
 * output so the shared CPU present path consumes it the same way. Output is
 * (g_wide_w*scale) wide × (disp_h*scale) tall. Returns pixel count (>0), or 0
 * if no surface exists for base_x (caller falls back to the canonical present).
 *
 * PIXEL FORMAT: glReadPixels(GL_BGRA, GL_UNSIGNED_BYTE) yields, per pixel, the
 * little-endian uint32 0xAARRGGBB == ARGB8888 — exactly what rgb555_to_argb
 * produces and what upload_present_tex feeds to glTexImage2D(GL_BGRA,...). The
 * SW path forces alpha to 0xFF; we OR it in to match (present ignores alpha, but
 * we keep the two paths bit-identical). GL's read origin is bottom-left, so the
 * block is read bottom-to-top and copied into `out` reversed (out row 0 = the
 * display's top scanline, as the SW path and the PRESENT_VS V-flip expect). */
static int glb_render_wide_display(uint32_t *out, int pitch, int base_x,
                                   int disp_y, int disp_h) {
    if (!s_raster_ok || !s_ctx || g_wide_w <= 0) return 0;
    GLuint fbo = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) { fbo = s_wide_fbo[i]; break; }
    if (!fbo) return 0;

    /* Fold any pending CPU->VRAM uploads into the canonical FBO first (uploads
     * are never mirrored to wide, but draws after them are; keep op order) and
     * make sure all wide-FBO draws have completed before the readback. */
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    hiw_flush_queue();   /* windowed: queued wide mirrors */
    wide_blit_center(fbo, base_x, disp_y, disp_h);   /* fast-path: authoritative centre before readback */
    glFinish();

    int W = g_wide_w * s_out_scale;
    int H = VRAM_H * s_out_scale;
    int out_h = disp_h * s_out_scale;
    int ry0 = disp_y * s_out_scale;
    if (ry0 < 0) ry0 = 0;
    if (ry0 + out_h > H) out_h = H - ry0;
    if (out_h <= 0) return 0;

    uint32_t *tmp = (uint32_t *)malloc((size_t)W * out_h * sizeof(uint32_t));
    if (!tmp) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, ry0, W, out_h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);

    /* Orientation: the wide FBO stores PS1 y inverted (geo shader maps vram_y=0
     * to NDC y=-1 = FBO BOTTOM), and glReadPixels reads bottom-up — the two
     * inversions CANCEL, so glReadPixels row 0 already = PS1 top scanline. Copy
     * straight (NO flip) so `out` is top-down, matching sw_render_wide_display
     * (which the shared CPU present path + its PRESENT_VS V-flip expect). Force
     * alpha = 0xFF (match SW). [An earlier reversal here made the frame
     * upside-down.] */
    int count = 0;
    for (int row = 0; row < out_h; row++) {
        const uint32_t *src = tmp + (size_t)row * W;
        uint32_t *dst = (uint32_t *)((uint8_t *)out + (size_t)row * pitch);
        for (int col = 0; col < W; col++) { dst[col] = src[col] | 0xFF000000u; count++; }
    }
    free(tmp);
    return count;
}

/* Dump the ENTIRE wide compositor surface for base_x (all double-buffer bands +
 * both reveal margins), g_wide_w x VRAM_H at scale. Debug/inspection tool (TCP
 * wide_full) — the GL analog of sw_wide_dump_full, so native-wide can be
 * inspected without touching the game window. Runs the same authoritative
 * centre blit first so the dump matches what present shows. Top-down, alpha=FF
 * (matches sw_wide_dump_full / render_wide_display orientation). */
static int glb_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh,
                              int base_x) {
    if (!s_raster_ok || !s_ctx || g_wide_w <= 0) return 0;
    GLuint fbo = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) { fbo = s_wide_fbo[i]; break; }
    if (!fbo) return 0;
    flush_tex_batch();
    flush_cpu_upload();
    hiw_flush_queue();   /* windowed: queued wide mirrors */
    wide_blit_center(fbo, base_x, 0, VRAM_H);   /* authoritative centre (full height) */
    glFinish();
    int W = g_wide_w * s_out_scale;
    int H = VRAM_H * s_out_scale;
    if (cap_pixels > 0 && (long)W * H > cap_pixels) { H = cap_pixels / W; if (H <= 0) return 0; }
    uint32_t *tmp = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    if (!tmp) return 0;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    /* Same orientation reasoning as glb_render_wide_display: glReadPixels row 0 =
     * PS1 top scanline, so copy straight (top-down). */
    int count = 0;
    for (int i = 0; i < W * H; i++) { out[i] = tmp[i] | 0xFF000000u; count++; }
    free(tmp);
    if (ow) *ow = W;
    if (oh) *oh = H;
    return count;
}

/* THE present path for 15-bit frames: blit the display region from the
 * authoritative hr FBO into a letterboxed rect. Deterministic — runs
 * every 15-bit frame regardless of what mix of ops produced it.
 * force_4_3 pins to native 4:3 (15-bit MDEC FMV frames on a wide aspect). */
/* ===================== frame_perf: per-frame GPU/CPU phase timing ============
 * Developer builds use two GL_TIME_ELAPSED queries per frame to bracket (a) the
 * scene draws (all GP0 raster issued between two presents) and (b) the present
 * clear+blit, giving TRUE GPU time per phase independent of CPU/GPU overlap
 * (glFinish would only catch the non-overlapped tail and mislead). CPU wall time
 * (present-to-present total, and the present call) comes from SDL perf counters.
 * Results are read back GLPERF_NBUF frames late (no pipeline stall) into a ring
 * the debug server's frame_perf command aggregates. Release builds compile out
 * the debug server, so they also leave this instrumentation disabled: a native-
 * wide frame can otherwise issue hundreds of unused mirror timestamp queries.
 * One question this answers in a diagnostics build:
 * where does a 16:9 frame go vs 4:3 — scene fill, wide composite, or CPU. */
#define GLPERF_NBUF 4
#define GLPERF_RING 256
typedef struct {
    double   total_ms;        /* present-entry to present-entry (full frame)  */
    double   present_wall_ms; /* CPU wall time inside the present call         */
    double   scene_gpu_ms;    /* GPU: all scene draws this frame               */
    double   present_gpu_ms;  /* GPU: the present clear+blit                   */
    double   mirror_gpu_ms;   /* GPU: of scene_gpu, the native-wide mirror passes */
    double   prims;           /* scene primitives submitted this frame         */
    double   mirror_passes;   /* mirror passes this frame (measured + overflow) */
    double   cw_flush_ms;     /* CPU wall inside flush_tex_batch this frame    */
    double   cw_wide_ms;      /* CPU wall inside glb_wide_* this frame         */
    double   batches;         /* flush_tex_batch draws this frame              */
    double   wide_sets;       /* glb_wide_set_target calls this frame          */
    double   fbo_creates;     /* wide FBO+tex creations this frame             */
    int      wide;            /* 1 = native-wide (16:9) present, 0 = 4:3       */
    uint64_t frame;
} GlPerfSample;

/* Native-wide mirror-pass GPU attribution: the scene TIME_ELAPSED query spans
 * the whole frame (queries of one target cannot nest), so each mirror pass is
 * bracketed with a GL_TIMESTAMP pair instead (glQueryCounter does not conflict
 * with an active TIME_ELAPSED query). Pairs are pooled per buffered frame and
 * summed at readback, splitting scene_gpu into canonical vs mirror cost. */
#define GLPERF_MIRQ 1024               /* measured mirror passes per frame */
static GLuint s_mq_q[GLPERF_NBUF][GLPERF_MIRQ * 2];
static int    s_mq_n[GLPERF_NBUF];     /* pairs recorded this frame        */
static int    s_mq_over[GLPERF_NBUF];  /* passes beyond the pool (counted, untimed) */
static int    s_mq_open = 0;           /* begin issued, end pending        */
static int    s_mq_ok = 0;             /* glQueryCounter available         */

static int      s_pf_on = 0;
static GLuint   s_pf_scene_q[GLPERF_NBUF];
static GLuint   s_pf_present_q[GLPERF_NBUF];
static int      s_pf_b = 0;            /* buffer for the CURRENT frame         */
static int      s_pf_scene_active = 0;
static uint64_t s_pf_count = 0;
static uint64_t s_pf_freq = 1;
static uint64_t s_pf_last_enter = 0;
static uint64_t s_pf_enter = 0;
static double   s_pf_total_pending = 0.0;
static double   s_pf_buf_total[GLPERF_NBUF];
static double   s_pf_buf_pwall[GLPERF_NBUF];
static double   s_pf_buf_prims[GLPERF_NBUF];
static int      s_pf_buf_wide[GLPERF_NBUF];
static uint64_t s_pf_buf_frame[GLPERF_NBUF];
static uint64_t s_pf_prims_last = 0;
static double   s_pf_prims_pending = 0.0;
static double   s_pf_cw_pending[5];            /* flush_ms, wide_ms, batches, wide_sets, fbo_creates */
static double   s_pf_buf_cw[GLPERF_NBUF][5];
static GlPerfSample s_pf_ring[GLPERF_RING];
static uint64_t     s_pf_ring_seq = 0;

/* Dynamic resolution's render-thread frame cost owns the TIME_ELAPSED target
 * while it measures (rthf_begin): frame_perf stands aside. */
static int rthf_owns_timer(void);
static void gl_perf_yield(void) {
    if (s_pf_scene_active) { p_glEndQuery(GL_TIME_ELAPSED); s_pf_scene_active = 0; }
}

static void gl_perf_init(void) {
#ifdef PSX_NO_DEBUG_TOOLS
    return;
#else
    /* Timer queries are intentionally available in diagnostics builds, but a
     * driver may serialize command submission while collecting them.  Keep an
     * escape hatch so frame cadence can be A/B tested without rebuilding or
     * losing the rest of the debug server telemetry. */
    {
        const char *enabled = getenv("PSX_GL_PERF");
        if (enabled && enabled[0] == '0') return;
    }
    if (!p_glGenQueries || !p_glBeginQuery || !p_glEndQuery || !p_glGetQueryObjectui64v) return;
    p_glGenQueries(GLPERF_NBUF, s_pf_scene_q);
    p_glGenQueries(GLPERF_NBUF, s_pf_present_q);
    s_mq_ok = (p_glQueryCounter != NULL);
    if (s_mq_ok)
        for (int i = 0; i < GLPERF_NBUF; i++) {
            p_glGenQueries(GLPERF_MIRQ * 2, s_mq_q[i]);
            s_mq_n[i] = 0; s_mq_over[i] = 0;
        }
    s_mq_open = 0;
    s_pf_freq = SDL_GetPerformanceFrequency();
    if (!s_pf_freq) s_pf_freq = 1;
    s_pf_b = 0; s_pf_scene_active = 0; s_pf_count = 0; s_pf_ring_seq = 0;
    s_pf_last_enter = 0;
    s_pf_on = 1;
#endif
}

/* Bracket ONE native-wide mirror pass (called from the wide-mirror draw sites).
 * Timestamp pairs, not TIME_ELAPSED — see the pool comment above. */
static void gl_perf_mirror_begin(void) {
    if (!s_pf_on || !s_mq_ok || rthf_owns_timer()) return;
    int b = s_pf_b;
    if (s_mq_n[b] >= GLPERF_MIRQ) { s_mq_over[b]++; return; }
    p_glQueryCounter(s_mq_q[b][s_mq_n[b] * 2], GL_TIMESTAMP);
    s_mq_open = 1;
}
static void gl_perf_mirror_end(void) {
    if (!s_pf_on || !s_mq_ok || !s_mq_open) return;
    int b = s_pf_b;
    p_glQueryCounter(s_mq_q[b][s_mq_n[b] * 2 + 1], GL_TIMESTAMP);
    s_mq_n[b]++;
    s_mq_open = 0;
}

/* Top of present (after flush_cpu_upload, before clear/blit). */
static void gl_perf_present_enter(void) {
    /* Per-frame boundary for the 2D-backdrop stretch — runs from BOTH present
     * paths (4:3 present_vram AND native-wide present_wide_fbo), before the
     * perf-on gate so native-wide frames reset too. Snapshot this frame's
     * backdrop-stretch diagnostics, then reset the per-frame counters. (The gate
     * is per-prim now — no draw-order phase to reset.) Final batch already flushed
     * by the caller. */
    g_bdg_applied = s_bdg_applied; g_bdg_prims = s_bdg_prims; g_bdg_clearx = s_bdg_clearx;
    g_bdg_cur = (g_wide_cur != 0); g_bdg_base = g_wide_cur_base; g_bdg_w = g_wide_w; g_bdg_off = g_wide_off;
    s_bdg_applied = 0; s_bdg_prims = 0; s_bdg_clearx = -999999;
    /* PGXP depth (G1.14): every displayed frame starts with a clear depth
     * buffer, whatever the game does with its drawing areas. */
    if (s_depth_used) s_depth_need_clear = 1;
    /* Replay: taken on the emulation thread when the present was recorded. */
    { extern void psx_ws_dbg_gate_frame_snapshot(void); if (!rth_replaying()) psx_ws_dbg_gate_frame_snapshot(); }
    if (!s_pf_on || rthf_owns_timer()) return;
    uint64_t now = SDL_GetPerformanceCounter();
    s_pf_enter = now;
    s_pf_total_pending = s_pf_last_enter
        ? (double)(now - s_pf_last_enter) * 1000.0 / (double)s_pf_freq : 0.0;
    s_pf_last_enter = now;
    s_pf_prims_pending = (double)(s_scene_prims - s_pf_prims_last);   /* prims drawn this frame */
    s_pf_prims_last = s_scene_prims;
    s_pf_cw_pending[0] = s_cw_flush_ms;  s_pf_cw_pending[1] = s_cw_wide_ms;
    s_pf_cw_pending[2] = (double)s_cw_batches;
    s_pf_cw_pending[3] = (double)s_cw_wide_sets;
    s_pf_cw_pending[4] = (double)s_cw_fbo_creates;
    s_cw_flush_ms = 0.0; s_cw_wide_ms = 0.0;
    s_cw_batches = 0; s_cw_wide_sets = 0; s_cw_wide_cfgs = 0;
    s_cw_wide_clears = 0; s_cw_fbo_creates = 0;
    if (s_pf_scene_active) { p_glEndQuery(GL_TIME_ELAPSED); s_pf_scene_active = 0; } /* end frame b's scene draws */
    p_glBeginQuery(GL_TIME_ELAPSED, s_pf_present_q[s_pf_b]);                         /* time frame b's present */
}

/* End of present (after SwapWindow). wide = native-wide path. */
static void gl_perf_present_exit(int wide) {
    if (!s_pf_on || rthf_owns_timer()) return;
    uint64_t now = SDL_GetPerformanceCounter();
    p_glEndQuery(GL_TIME_ELAPSED);   /* end present_q[b] */
    s_pf_buf_total[s_pf_b] = s_pf_total_pending;
    s_pf_buf_pwall[s_pf_b] = (double)(now - s_pf_enter) * 1000.0 / (double)s_pf_freq;
    s_pf_buf_prims[s_pf_b] = s_pf_prims_pending;
    for (int ci = 0; ci < 5; ci++) s_pf_buf_cw[s_pf_b][ci] = s_pf_cw_pending[ci];
    s_pf_buf_wide[s_pf_b]  = wide;
    s_pf_buf_frame[s_pf_b] = s_pf_count;
    int rd = (s_pf_b + 1) % GLPERF_NBUF;   /* oldest buffer (frame count+1-NBUF), now done */
    if (s_pf_count >= (uint64_t)GLPERF_NBUF) {
        GLuint64 sc = 0, pr = 0;
        p_glGetQueryObjectui64v(s_pf_scene_q[rd],   GL_QUERY_RESULT, &sc);
        p_glGetQueryObjectui64v(s_pf_present_q[rd], GL_QUERY_RESULT, &pr);
        double mir = 0.0;
        for (int i = 0; i < s_mq_n[rd]; i++) {   /* sum that frame's mirror pairs */
            GLuint64 t0 = 0, t1 = 0;
            p_glGetQueryObjectui64v(s_mq_q[rd][i * 2],     GL_QUERY_RESULT, &t0);
            p_glGetQueryObjectui64v(s_mq_q[rd][i * 2 + 1], GL_QUERY_RESULT, &t1);
            if (t1 > t0) mir += (double)(t1 - t0);
        }
        GlPerfSample *s = &s_pf_ring[s_pf_ring_seq % GLPERF_RING];
        s->total_ms        = s_pf_buf_total[rd];
        s->present_wall_ms = s_pf_buf_pwall[rd];
        s->scene_gpu_ms    = (double)sc / 1.0e6;
        s->present_gpu_ms  = (double)pr / 1.0e6;
        s->mirror_gpu_ms   = mir / 1.0e6;
        s->prims           = s_pf_buf_prims[rd];
        s->mirror_passes   = (double)(s_mq_n[rd] + s_mq_over[rd]);
        s->cw_flush_ms     = s_pf_buf_cw[rd][0];
        s->cw_wide_ms      = s_pf_buf_cw[rd][1];
        s->batches         = s_pf_buf_cw[rd][2];
        s->wide_sets       = s_pf_buf_cw[rd][3];
        s->fbo_creates     = s_pf_buf_cw[rd][4];
        s->wide            = s_pf_buf_wide[rd];
        s->frame           = s_pf_buf_frame[rd];
        s_pf_ring_seq++;
    }
    s_mq_n[rd] = 0; s_mq_over[rd] = 0;   /* rd becomes the next frame's buffer */
    s_pf_count++;
    s_pf_b = rd;                                          /* reuse oldest for next frame */
    p_glBeginQuery(GL_TIME_ELAPSED, s_pf_scene_q[s_pf_b]); /* open next frame's scene draws */
    s_pf_scene_active = 1;
}

/* Aggregate the ring for the debug server. wide_filter: -1 all, 0 = 4:3, 1 = wide.
 * out[0]=count, [1]=total_avg, [2]=total_max, [3]=emu_cpu_avg (total-present_wall),
 * [4]=present_wall_avg, [5]=scene_gpu_avg, [6]=scene_gpu_max, [7]=present_gpu_avg,
 * [8]=present_gpu_max. Returns the sample count. */
/* Cumulative textured fraction of scene prims (decides flat vs textured batching
 * priority). out_tex_frac = textured/total since boot; returns total prim count. */
uint64_t gl_renderer_perf_prim_split(double *out_tex_frac) {
    GL_RT_SYNC("perf_prim_split");
    if (out_tex_frac) *out_tex_frac = s_scene_prims ? (double)s_scene_prims_tex / (double)s_scene_prims : 0.0;
    return s_scene_prims;
}

int gl_renderer_perf_aggregate(int wide_filter, double out[18]) {
    GL_RT_SYNC("perf_aggregate");
    for (int i = 0; i < 18; i++) out[i] = 0.0;
    if (!s_pf_on) return 0;
    int navail = (int)(s_pf_ring_seq < (uint64_t)GLPERF_RING ? s_pf_ring_seq : GLPERF_RING);
    uint64_t start = s_pf_ring_seq - (uint64_t)navail;
    int n = 0;
    for (int i = 0; i < navail; i++) {
        const GlPerfSample *s = &s_pf_ring[(start + i) % GLPERF_RING];
        if (wide_filter >= 0 && s->wide != wide_filter) continue;
        double emu = s->total_ms - s->present_wall_ms; if (emu < 0) emu = 0;
        out[1] += s->total_ms;       if (s->total_ms     > out[2]) out[2] = s->total_ms;
        out[3] += emu;
        out[4] += s->present_wall_ms;
        out[5] += s->scene_gpu_ms;   if (s->scene_gpu_ms > out[6]) out[6] = s->scene_gpu_ms;
        out[7] += s->present_gpu_ms; if (s->present_gpu_ms > out[8]) out[8] = s->present_gpu_ms;
        out[9] += s->prims;
        out[10] += s->mirror_gpu_ms; if (s->mirror_gpu_ms > out[11]) out[11] = s->mirror_gpu_ms;
        out[12] += s->mirror_passes;
        out[13] += s->cw_flush_ms;
        out[14] += s->cw_wide_ms;
        out[15] += s->batches;
        out[16] += s->wide_sets;
        out[17] += s->fbo_creates;
        n++;
    }
    if (n) {
        out[1]/=n; out[3]/=n; out[4]/=n; out[5]/=n; out[7]/=n; out[9]/=n; out[10]/=n; out[12]/=n;
        out[13]/=n; out[14]/=n; out[15]/=n; out[16]/=n; out[17]/=n;
    }
    out[0] = (double)n;
    return n;
}

/* Native-wide mirror ablation (perf attribution): see s_ws_ablate. */
void gl_renderer_set_ws_ablate(int mode) {
    GL_RT_SYNC("set_ws_ablate"); s_ws_ablate = (mode >= 0 && mode <= 3) ? mode : 0; }
int  gl_renderer_get_ws_ablate(void)     { return s_ws_ablate; }

static void interp_reset_history_unlocked(void) {
    s_interp_valid = 0;
    s_interp_w = s_interp_h = 0;
    s_interp_src_w = s_interp_src_h = 0;
    s_interp_source_path = -1;
    frame_interpolation_schedule_reset(&s_interp_schedule);
    frame_flip_tracker_reset(&s_interp_flip);
    s_interp_origin_x = s_interp_origin_y = -1;
    s_interp_phase_lo = 0.0;
    s_interp_phase_hi = 1.0;
    pass_gens_invalidate();
}

static void interp_reset_history(void) {
    interp_reset_history_unlocked();
}

void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz,
                                   double source_hz, int blend_mode) {
    GL_RT_SYNC("set_interpolation");
    double effective_hz = target_hz > 0.0 ? target_hz : host_hz;
    if (effective_hz < source_hz) effective_hz = source_hz;
    int active = (enabled && source_hz >= 1.0 && source_hz <= 1000.0 &&
                  effective_hz >= source_hz && effective_hz <= 1000.0) ? 1 : 0;
    const char *diag = getenv("PSX_GL_INTERP_DIAG");
    s_interp_diag = diag && diag[0] && diag[0] != '0';
    if (active != s_interp_enabled || source_hz != s_interp_source_hz ||
        effective_hz != s_interp_target_hz)
        interp_reset_history_unlocked();
    s_interp_enabled = active;
    s_interp_host_hz = host_hz;
    s_interp_target_hz = active ? effective_hz : 0.0;
    s_interp_source_hz = active ? source_hz : 0.0;
    s_interp_blend_mode = blend_mode == 1 ? 1 : 0;
    s_interp_hold = blend_mode == 2 ? 1 : 0;
    if (active)
        fprintf(stdout, "psxrecomp: GL temporal frame blending enabled: %.1f "
                "presents/s from %.3f guest frames/s on the render thread "
                "(%s; no motion vectors)\n",
                effective_hz, source_hz,
                s_interp_hold ? "hold, in-between frames from render passes"
                : s_interp_blend_mode ? "change-adaptive blend"
                                      : "linear blend");
    else
        fprintf(stdout, "psxrecomp: GL temporal frame blending disabled "
                "(host %.1f Hz)\n", host_hz);
}

void gl_renderer_set_interpolation_blend(int blend_mode) {
    GL_RT_SYNC("set_interpolation_blend");
    s_interp_blend_mode = blend_mode == 1 ? 1 : 0;
    s_interp_hold = blend_mode == 2 ? 1 : 0;
}

void gl_renderer_set_interpolation_suspended(int suspended) {
    if (rth_record_mode()) { RTH_REC(RTH_INTERP_SUSPENDED, 0, suspended ? 1 : 0); return; }
    suspended = suspended ? 1 : 0;
    if (suspended != s_interp_suspended) interp_reset_history_unlocked();
    s_interp_suspended = suspended;
}

void gl_renderer_set_interpolation_source(int source) {
    GL_RT_SYNC("set_interpolation_source");
    source = source == 1 ? 1 : 0;
    if (source != s_interp_source) interp_reset_history_unlocked();
    s_interp_source = source;
    if (s_interp_enabled && source)
        fprintf(stdout, "psxrecomp: GL temporal blending follows guest frame "
                "flips (flip-aware source)\n");
}

void gl_renderer_interpolation_source_diag(int *source, uint32_t *flip_period,
                                           uint64_t *captures,
                                           uint64_t *duplicates) {
    GL_RT_SYNC("interpolation_source_diag");
    if (source) *source = s_interp_source;
    if (flip_period) *flip_period = s_interp_flip.period;
    if (captures) *captures = s_interp_captures;
    if (duplicates) *duplicates = s_interp_duplicates;
}

/* Host suspension OR a running OpenXR session: every XR present would otherwise
 * wait/begin/submit a frame per interpolated sub-present. The compositor
 * reprojects instead. Constant 0 contribution without an active XR session;
 * history is reset on each transition like the FMV suspension. */
static int s_interp_xr_gated;
static int interp_suspended_now(void) {
    int xr = psx_openxr_session_active();
    if (xr != s_interp_xr_gated) { s_interp_xr_gated = xr; interp_reset_history_unlocked(); }
    return s_interp_suspended || xr;
}

int gl_renderer_interpolation_owns_cadence(void) {
    return s_ctx && s_interp_enabled && !interp_suspended_now();
}

void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps) {
    GL_RT_SYNC("interpolation_diag");
    if (enabled) *enabled = s_interp_enabled;
    if (suspended) *suspended = s_interp_suspended;
    if (history_frames) *history_frames = s_interp_valid;
    if (host_hz) *host_hz = s_interp_host_hz;
    if (target_hz) *target_hz = s_interp_target_hz;
    if (swaps) *swaps = s_interp_swaps;
}

/* Copy a stable display image out of the mutable VRAM/wide render target.
 * Returns true when temporal blending owns this source-frame interval. */
static int interp_capture(GLuint fbo, int x, int y, int w, int h,
                          int linear, int force_4_3, int source_path,
                          int origin_x, int origin_y, int redrawn) {
    if (!s_interp_enabled || interp_suspended_now() || !fbo || w <= 0 || h <= 0) return 0;
    int sw = w * s_out_scale, sh = h * s_out_scale, pw, ph;
    hiw_capture_size(sw, sh, force_4_3, &pw, &ph);
    int geometry_changed =
        pw != s_interp_w || ph != s_interp_h ||
        sw != s_interp_src_w || sh != s_interp_src_h ||
        source_path != s_interp_source_path || force_4_3 != s_interp_force_4_3;
    if (s_interp_source == 1) {
        /* FLIP source: a VBlank that re-presents the same displayed image is
         * not a new source frame. Keep history and advance the phase window. */
        int new_frame = frame_flip_is_new_frame(
            geometry_changed, s_interp_valid == 0, redrawn, origin_x, origin_y,
            s_interp_origin_x, s_interp_origin_y);
        s_interp_origin_x = origin_x;
        s_interp_origin_y = origin_y;
        (void)frame_flip_tracker_vblank(&s_interp_flip, new_frame,
                                        &s_interp_phase_lo, &s_interp_phase_hi);
        if (!new_frame) {
            s_interp_duplicates++;
            return 1;
        }
        pass_note_new_frame(origin_x, origin_y, source_path, pw, ph);
    } else {
        s_interp_phase_lo = 0.0;
        s_interp_phase_hi = 1.0;
    }
    if (geometry_changed) {
        s_interp_valid = 0;
        s_interp_w = pw; s_interp_h = ph;
        s_interp_src_w = sw; s_interp_src_h = sh;
        s_interp_prev = s_interp_cur = 0;
        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, s_interp_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
    }

    int dst = 0;
    if (s_interp_valid == 1) dst = s_interp_cur == 0 ? 1 : 0;
    else if (s_interp_valid >= 2) dst = s_interp_prev;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    if (pw == sw && ph == sh) {
        glBindTexture(GL_TEXTURE_2D, s_interp_tex[dst]);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            x * s_out_scale, y * s_out_scale, pw, ph);
    } else {   /* windowed high-resolution mode: at the presented size */
        if (!s_interp_fbo) p_glGenFramebuffers(1, &s_interp_fbo);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_interp_fbo);
        p_glFramebufferTexture2D(PSXGL_DRAW_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, s_interp_tex[dst], 0);
        glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(x * s_out_scale, y * s_out_scale,
                            x * s_out_scale + sw, y * s_out_scale + sh,
                            0, 0, pw, ph, GL_COLOR_BUFFER_BIT,
                            linear ? GL_LINEAR : GL_NEAREST);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    if (s_interp_valid == 0) {
        s_interp_cur = dst;
        s_interp_valid = 1;
    } else {
        s_interp_prev = s_interp_cur;
        s_interp_cur = dst;
        s_interp_valid = 2;
    }
    s_interp_linear = linear;
    s_interp_force_4_3 = force_4_3;
    s_interp_source_path = source_path;
    s_interp_captures++;
    return 1;
}

static void interp_draw_textures(GLuint prev_tex, GLuint curr_tex, float alpha,
                                 int blend_mode, int lx, int ly, int lw, int lh) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, curr_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_interp_linear ? GL_LINEAR : GL_NEAREST);
    p_glUseProgram(s_interp_prog);
    p_glUniform1i(s_interp_uPrev, 0);
    p_glUniform1i(s_interp_uCurr, 1);
    p_glUniform1f(s_interp_uAlpha, alpha);
    p_glUniform1i(s_interp_uBlendMode, blend_mode);
    present_set_gamma(s_interp_uGamma, 1);
    p_glUniform4f(s_interp_uUvRect, 0.f, 0.f, 1.f, 1.f);
    /* Interp textures hold exactly the display rect (uv_rect is 0..1), so pitch
     * == display height == s_interp_src_h rows at the output scale (the
     * texture itself can be smaller, see hiw_capture_size). */
    INTERP_SCANLINE(s_interp_src_h, s_interp_src_h, lh);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glActiveTexture(PSXGL_TEXTURE0);
}

static uint64_t s_present_ticks_accum_fwd(uint64_t add);
static int interp_present_pair(GLuint a, GLuint b, float t, int blend_mode) {
    if (!s_ctx || !s_interp_enabled || interp_suspended_now() || s_interp_valid < 1)
        return 0;
    uint64_t present_t0 = SDL_GetPerformanceCounter();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    if (s_interp_force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    present_bezel(ww, wh, lx, ly, lw, lh);
    interp_draw_textures(a, b, t, blend_mode, lx, ly, lw, lh);
    pres_record(GL_PRES_INTERP, 0, 0, s_interp_w, s_interp_h,
                lx, ly, lw, lh);
    s_dyn_last_swap_ticks = 0;
    gl_swap_with_osd();
    s_interp_swaps++;
    {
        uint64_t spent = SDL_GetPerformanceCounter() - present_t0;
        (void)s_present_ticks_accum_fwd(spent);
        if (s_dyn_on) {   /* the blend's own work, its swap wait apart */
            s_dyn_ledger.interp_work_ticks += spent > s_dyn_last_swap_ticks
                                              ? spent - s_dyn_last_swap_ticks : 0;
            s_dyn_ledger.interp_presents++;
        }
    }
    return 1;
}

static int interp_present(float alpha) {
    if (s_interp_hold) alpha = 1.0f;
    return interp_present_pair(s_interp_tex[s_interp_prev],
                               s_interp_tex[s_interp_cur], alpha,
                               s_interp_blend_mode);
}

static void interp_wait_until(uint64_t deadline, uint64_t frequency) {
    uint64_t now, start;
    if (!deadline || !frequency) return;
    start = SDL_GetPerformanceCounter();
    for (;;) {
        now = SDL_GetPerformanceCounter();
        if (now >= deadline) {
            /* Idle host time feeds the render-pass budget. */
            (void)s_idle_ticks_accum_fwd(now - start);
            if (s_dyn_on) s_dyn_ledger.idle_ticks += now - start;
            return;
        }
        uint64_t remain = deadline - now;
        uint32_t ms = (uint32_t)((remain * 1000u) / frequency);
        if (ms > 1) psx_host_sleep_ms(ms - 1);
    }
}

static void interp_present_source_interval(void) {
    static uint64_t diag_start, diag_swaps, diag_captures;
    uint64_t frequency = SDL_GetPerformanceFrequency();
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t deadline;
    float alpha;

    if (!frame_interpolation_schedule_begin_phase(
            &s_interp_schedule, now, frequency,
            s_interp_source_hz, s_interp_target_hz,
            s_interp_phase_lo, s_interp_phase_hi))
        return;
    pass_apply_promotion();

    while (frame_interpolation_schedule_next(
               &s_interp_schedule, SDL_GetPerformanceCounter(),
               &deadline, &alpha)) {
        interp_wait_until(deadline, frequency);
        latency_ring_mark(LAT_SWAP_BEGIN);
        if (!pass_gen_present(deadline))
            (void)interp_present(alpha);
        latency_ring_mark(LAT_SWAP_END);
    }
    interp_wait_until(frame_interpolation_schedule_end(&s_interp_schedule),
                      frequency);

    now = SDL_GetPerformanceCounter();
    if (!diag_start) {
        diag_start = now;
        diag_swaps = s_interp_swaps;
        diag_captures = s_interp_captures;
    } else if (s_interp_diag && frequency &&
               now - diag_start >= frequency * 5u) {
        double seconds = (double)(now - diag_start) / (double)frequency;
        fprintf(stdout, "psxrecomp: GL temporal-blend cadence: "
                "%.2f captures/s, %.2f presents/s (single context)\n",
                (double)(s_interp_captures - diag_captures) / seconds,
                (double)(s_interp_swaps - diag_swaps) / seconds);
        fflush(stdout);
        diag_start = now;
        diag_captures = s_interp_captures;
        diag_swaps = s_interp_swaps;
    }
}

/* ==== Render passes (render_pass.c, docs/RENDER_PASSES.md) ==================
 *
 * VRAM transaction. A pass draws into the guest's own display rect, which
 * still holds the finished stock image the next flip shows. Begin backs that
 * rect up on the GPU (hr color + mask stencil, the raw 16-bit mirror, the
 * native-wide band) and in the CPU VRAM array, plus the renderer's coherency
 * bookkeeping; end captures the drawn rect into a pass slot and puts all of it
 * back. While a pass is open, GPU writes outside the rect are refused and
 * counted (the pass is then rolled back without an image).
 *
 * Presentation. A generation holds one game frame's images: slot 0 is the
 * game's own image (captured when the generation opens, before the first
 * pass draws), then one slot per pass phase. It opens at the plugin's pass
 * point and becomes current when the FLIP source sees the display flip to its
 * rect; its phases then map onto host time from that interval's start. At
 * each output deadline the presenter shows the newest image at or before the
 * deadline's phase, crossfading to the next one when passes were shed. Two
 * generations exist: the one on screen and the one being built for the next
 * flip. */

#define PASS_SLOTS (RENDER_PASS_MAX_PHASES + 1u)
typedef struct PassGen {
    int      valid;
    int      promoted;
    int      x, y, w, h;          /* captured VRAM rect (what is presented) */
    int      rx, ry, rw, rh;      /* the plugin's pass rect (backed up)     */
    int      tex_w, tex_h;        /* slot size (hr pixels; wide band if wide) */
    int      source_path;         /* GL_PRES_VRAM or GL_PRES_WIDE */
    uint32_t n;                   /* images: [0] = the game's own */
    uint32_t phase[PASS_SLOTS];   /* Q16, ascending */
    uint32_t period;              /* guest VBlanks the frame stays on screen */
    int      shown;               /* built for a frame already on screen */
    double   t_start, t_len;      /* host ticks, set on promotion */
} PassGen;
static int      s_pass_flip_shown = 0;
static PassGen  s_pgen[2];
/* Double-buffer complete pairs: the unpublished set may be overwritten while
 * the last complete pair remains visible. No temporal phases or generation. */
typedef struct StereoPair {
    GLuint tex[2], fbo[2];
    int tw[2], th[2];
    int x, y, w, h;
    uint32_t mask;
    uint64_t id, cycle;
    int32_t view[2][3];
} StereoPair;
static StereoPair s_stereo_pair[2];
static int s_stereo_current, s_stereo_valid, s_stereo_mode;
static uint64_t s_stereo_presents;
static char s_stereo_dump_dir[400];
static int s_stereo_dump_left;
enum { STEREO_CAPTURE_MAX = 100 };
static GLRenderStereoCapture s_stereo_captures[STEREO_CAPTURE_MAX];
static uint32_t s_stereo_capture_count;
/* The part of the displayed buffer the VRAM present captures, relative to the
 * display origin: the whole display, or (netplay local viewport) this peer's
 * half. A pass captures the same part of its own buffer, so its images are
 * what the presenter would have shown. */
static int      s_present_crop_dx = 0, s_present_crop_dy = 0;
static int      s_present_crop_w = 0, s_present_crop_h = 0;
/* Slot textures are made as slots fill: [0, s_pgen_alloc_n) exist, all at
 * s_pgen_alloc_w x h. A generation never fills past pass_slot_cap, so two
 * generations stay inside its budget whatever the internal scale. */
static GLuint   s_pgen_tex[2][PASS_SLOTS];
static uint32_t s_pgen_alloc_n[2];
static int      s_pgen_alloc_w[2], s_pgen_alloc_h[2];
static int      s_pgen_cur = 0;
static int      s_pgen_promote = 0;
static uint64_t s_pgen_promotions = 0, s_pgen_presents = 0, s_pgen_blends = 0;
static uint64_t s_pgen_expired = 0, s_pgen_unmatched = 0, s_pgen_early = 0;
static uint64_t s_pgen_late = 0;     /* presents past the frame's planned end */

static GLuint   s_pb_hr_tex = 0, s_pb_hr_rb = 0, s_pb_hr_fbo = 0;
static int      s_pb_hr_w = 0, s_pb_hr_h = 0;
static GLuint   s_pb_raw_tex = 0, s_pb_raw_fbo = 0;
static int      s_pb_raw_w = 0, s_pb_raw_h = 0;
static GLuint   s_pb_wide_tex = 0, s_pb_wide_rb = 0, s_pb_wide_fbo = 0;
static int      s_pb_wide_w = 0, s_pb_wide_h = 0;
static GLuint   s_pb_wide_src = 0;
/* The backup above still equals VRAM (the last pass restored it): rect and
 * scale it was taken at. A later pass of the same frame reuses it. */
static int      s_pb_valid = 0;
static int      s_pb_x = 0, s_pb_y = 0, s_pb_w = 0, s_pb_h = 0, s_pb_scale = 0;
static uint64_t s_pb_reused = 0;
static uint16_t *s_pb_cpu = NULL;
static size_t   s_pb_cpu_cap = 0;
static DirtyRect s_pb_cpu_dirty, s_pb_pack_dirty, s_pb_up_rects[UP_RECTS_MAX];
static int      s_pb_up_n = 0, s_pb_gpu_dirty = 0, s_pb_stencil_valid = 1;
static uint64_t s_pb_present_dirty[PRES_ROWS];
static int      s_pb_force_present = 0, s_pb_last_path = -1;
static int      s_pb_last_dx = 0, s_pb_last_dy = 0, s_pb_last_dw = 0, s_pb_last_dh = 0;

/* Per-pass host cost (render_pass_plan.h RenderPassCost), and the presented
 * image size it was measured at. Another size (an aspect or internal-
 * resolution change) starts it over: pass cost does not scale with pixels
 * alone, and while it is unknown a plan asks for one pass. */
static RenderPassCost s_pass_cost;
static int      s_pass_cost_w = 0, s_pass_cost_h = 0;
static uint64_t s_pass_cost_rewarms = 0;   /* stale estimates re-measured */
static uint64_t s_pass_ticks_accum = 0, s_idle_ticks_accum = 0;
static uint64_t s_pass_ticks_last = 0, s_idle_ticks_last = 0;
static uint64_t s_present_ticks_accum = 0, s_present_ticks_last = 0;
static double   s_present_cost_ema = 0.0;     /* host ticks per present */
static uint32_t s_intervals_since_plan = 0;
static int      s_pass_budget_pct = -1;
static GLRenderPassPlanDiag s_pass_plan_diag;

static int      s_pass_verify = -1;
static uint8_t *s_pv_hr = NULL, *s_pv_raw = NULL;
static size_t   s_pv_hr_cap = 0, s_pv_raw_cap = 0;
static int      s_pv_ok = 1;
static uint64_t s_pv_cpu_hash = 0;

/* The whole CPU VRAM: the guest-visible VRAM (GPUREAD, savestates, netplay
 * digests and, in dual raster, the authoritative surface). */
static uint64_t pass_cpu_vram_hash(void) {
    uint64_t h = 1469598103934665603ULL;
    if (!s_vram) return 0;
    const uint64_t *w = (const uint64_t *)(const void *)s_vram;
    for (size_t i = 0; i < (size_t)VRAM_W * VRAM_H / 4u; i++)
        h = (h ^ w[i]) * 1099511628211ULL;
    return h;
}

uint64_t gl_renderer_perf_ticks(void) { return SDL_GetPerformanceCounter(); }
uint64_t gl_renderer_perf_frequency(void) { return SDL_GetPerformanceFrequency(); }

static void pass_gens_invalidate(void) {
    s_pgen[0].valid = s_pgen[1].valid = 0;
    s_pgen[0].promoted = s_pgen[1].promoted = 0;
    s_pgen_promote = 0;
}

static int s_pass_force_refuse = -1;   /* -1: read PSX_RENDER_PASS_REFUSE */
static GLRenderPassBeginDiag s_pass_begin_diag;

void gl_renderer_pass_begin_diag(GLRenderPassBeginDiag *out) {
    GL_RT_SYNC("pass_begin_diag");
    if (out) *out = s_pass_begin_diag;
}

static int pass_begin_refuse(const char *reason) {
    s_pass_begin_diag.reason = reason;
    return 0;
}

/* Keep pre-existing errors separate from this allocation's errors. */
static uint32_t pass_gl_errors(void) {
    GLenum e;
    uint32_t first = 0;
    while ((e = glGetError()) != GL_NO_ERROR) {
        if (!first) first = (uint32_t)e;
    }
    return first;
}

void gl_renderer_pass_force_refuse(int on) {
    GL_RT_SYNC("pass_force_refuse");
    s_pass_force_refuse = on ? 1 : 0;
}

static void pass_refusal_init(void) {
    if (s_pass_force_refuse < 0) {
        const char *e = getenv("PSX_RENDER_PASS_REFUSE");
        s_pass_force_refuse = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (s_pass_force_refuse)
            fprintf(stderr, "psxrecomp: render passes refused by the backend "
                    "(PSX_RENDER_PASS_REFUSE, debug)\n");
    }
}

uint32_t gl_renderer_pass_unavailable(void) {
    GL_RT_SYNC("pass_unavailable");
    pass_refusal_init();
    if (!s_ctx || !s_raster_ok || !s_interp_enabled || s_interp_suspended ||
        s_interp_source != 1 || !(s_interp_source_hz > 0.0))
        return PSX_MOD_RENDER_PASS_NO_PRESENTER;
    /* Dual raster (netplay CPU-authoritative VRAM) hosts passes once netplay
     * passes are opted in: its authoritative surface is the CPU VRAM the
     * pass backs up and restores (rect rows, journaled out-of-rect writes),
     * and the FBO it presents is the one passes capture.
     * PSX_RENDER_PASS_VERIFY=1 checks the whole CPU VRAM after each pass.
     * HD native authority and the debug refusal decline. */
    if (s_hd_native_authority || (s_cpu_auth_dual && !render_pass_netplay_enabled()) || s_pass_force_refuse)
        return PSX_MOD_RENDER_PASS_BACKEND;
    /* Windowed high-resolution mode: the presented surfaces are the window
     * tiles at s_out_scale, which a pass (backing up s_hr_fbo at
     * s_hr_scale) would not restore. */
    if (s_hiw)
        return PSX_MOD_RENDER_PASS_BACKEND;
    /* The history restarts when the display mode changes (e.g. title ->
     * race); passes wait for its first frame, a VBlank or two. */
    if (s_interp_valid <= 0)
        return PSX_MOD_RENDER_PASS_BUSY;
    return PSX_MOD_RENDER_PASS_READY;
}

int gl_renderer_pass_ready(void) {
    GL_RT_SYNC("pass_ready");
    return gl_renderer_pass_unavailable() == PSX_MOD_RENDER_PASS_READY;
}

/* Slots per generation that fit a 256 MiB budget for both generations. */
static uint32_t pass_slot_cap(int tex_w, int tex_h) {
    double bytes = (double)tex_w * (double)tex_h * 4.0 * 2.0;
    uint32_t cap = bytes > 0.0 ? (uint32_t)((256.0 * 1024.0 * 1024.0) / bytes)
                               : PASS_SLOTS;
    if (cap > PASS_SLOTS) cap = PASS_SLOTS;
    return cap;
}

uint32_t gl_renderer_pass_plan(uint32_t period_vblanks,
                               uint32_t shown_after_vblanks,
                               uint32_t *alpha_q16, uint32_t max,
                               uint32_t *wanted) {
    GL_RT_SYNC("pass_plan");
    RenderPassPlanInput in;
    double freq, sp, spare;
    uint32_t cap;
    int live;

    if (wanted) *wanted = 0;
    /* One plan per game frame: close the previous frame's host-time books. */
    s_idle_ticks_last = s_idle_ticks_accum;
    s_pass_ticks_last = s_pass_ticks_accum;
    s_present_ticks_last = s_present_ticks_accum;
    s_idle_ticks_accum = s_pass_ticks_accum = s_present_ticks_accum = 0;
    live = s_intervals_since_plan > 0;   /* turbo/headless present nothing */
    s_intervals_since_plan = 0;
    if (!gl_renderer_pass_ready() || !live || !alpha_q16 || max == 0) return 0;
    if (s_interp_schedule.target_period <= 0.0 ||
        s_interp_schedule.source_deadline <= 0.0)
        return 0;
    if (s_pass_budget_pct < 0) {
        const char *e = getenv("PSX_RENDER_PASS_BUDGET");
        int v = e ? atoi(e) : 0;
        s_pass_budget_pct = (v >= 5 && v <= 100) ? v : 80;
    }
    freq = (double)SDL_GetPerformanceFrequency();
    sp = freq / s_interp_source_hz;
    cap = pass_slot_cap(s_interp_w, s_interp_h);
    if (cap < 2) return 0;
    memset(&in, 0, sizeof in);
    in.next_deadline = s_interp_schedule.next_present_deadline;
    in.target_period = s_interp_schedule.target_period;
    in.frame_start = s_interp_schedule.source_deadline +
                     (double)shown_after_vblanks * sp;
    in.frame_length = (double)period_vblanks * sp;
    in.pass_cost = (s_pass_cost_w == s_interp_w && s_pass_cost_h == s_interp_h)
                   ? render_pass_cost_estimate(&s_pass_cost) : 0.0;
    {
        /* Presents beyond two per frame are traded for passes: a shed
         * present is coalesced, a shed pass is a missing motion sample. */
        spare = (double)s_idle_ticks_last + (double)s_present_ticks_last -
                2.0 * s_present_cost_ema;
        if (spare < 0.0) spare = 0.0;
        in.budget = render_pass_budget(spare, (double)s_pass_ticks_last,
                                       in.frame_length,
                                       (double)s_pass_budget_pct / 100.0);
    }
    in.max = max < cap - 1u ? max : cap - 1u;
    {
        uint32_t want = 0, n = render_pass_plan_phases(&in, alpha_q16, &want);
        if (wanted) *wanted = want;
        /* An estimate no pass has confirmed for a while is measured again
         * (render_pass_cost_note_plan): its plans then ask for one pass. */
        if (want && s_pass_cost_w == s_interp_w && s_pass_cost_h == s_interp_h &&
            render_pass_cost_note_plan(&s_pass_cost)) {
            s_pass_cost_rewarms++;
            in.pass_cost = 0.0;
            n = render_pass_plan_phases(&in, alpha_q16, NULL);
        }
        /* Record the actual input used, including a cost rewarm, without
         * making a debug query drive admission or reset its history. */
        {
            double ms = 1000.0 / freq;
            s_pass_plan_diag.plans++;
            s_pass_plan_diag.frame = s_frame_count;
            s_pass_plan_diag.idle_ms = (double)s_idle_ticks_last * ms;
            s_pass_plan_diag.present_ms = (double)s_present_ticks_last * ms;
            s_pass_plan_diag.prior_pass_ms = (double)s_pass_ticks_last * ms;
            s_pass_plan_diag.present_reserve_ms = 2.0 * s_present_cost_ema * ms;
            s_pass_plan_diag.spare_ms = spare * ms;
            s_pass_plan_diag.frame_ms = in.frame_length * ms;
            s_pass_plan_diag.cost_ms = in.pass_cost * ms;
            s_pass_plan_diag.budget_ms = in.budget * ms;
            s_pass_plan_diag.wanted = want;
            s_pass_plan_diag.planned = n;
            if (want && !n && !(in.budget > 0.0))
                s_pass_plan_diag.zero_credit_refusals++;
        }
        return n;
    }
}

void gl_renderer_pass_plan_diag(GLRenderPassPlanDiag *out) {
    if (out) *out = s_pass_plan_diag;
}

void gl_renderer_pass_note_cost(uint64_t ticks) {
    GL_RT_SYNC("pass_note_cost");
    if (s_pass_cost_w != s_interp_w || s_pass_cost_h != s_interp_h) {
        memset(&s_pass_cost, 0, sizeof s_pass_cost);
        s_pass_cost_w = s_interp_w;
        s_pass_cost_h = s_interp_h;
    }
    /* A pass that made pass textures or framebuffers (first use, a size
     * change) is not a cost sample: see render_pass_cost_add(). */
    render_pass_cost_add(&s_pass_cost, (double)ticks,
                         s_pass_allocs != s_pass_allocs_begin);
    s_pass_ticks_accum += ticks;
    if (s_dyn_on) s_dyn_ledger.pass_ticks += ticks;
}

uint32_t gl_renderer_pass_leaks(void) {
    GL_RT_SYNC("pass_leaks"); return s_pass_leaks; }
int gl_renderer_pass_verify_vram(void) {
    GL_RT_SYNC("pass_verify_vram"); return s_pv_ok; }


static GLuint pass_wide_fbo_for(int base_x) {
    if (g_wide_w <= 0) return 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == base_x) return s_wide_fbo[i];
    return 0;
}

static int pass_make_color_fbo(GLuint *tex, GLuint *rb, GLuint *fbo,
                               int *cur_w, int *cur_h, int w, int h,
                               GLenum internal, GLenum fmt, GLenum type) {
    if (*fbo && *cur_w == w && *cur_h == h) return 1;
    s_pass_begin_diag.gl_error_before = pass_gl_errors();
    s_pass_begin_diag.gl_error = 0;
    s_pass_begin_diag.fbo_status = 0;
    if (*fbo) p_glDeleteFramebuffers(1, fbo);
    if (*tex) glDeleteTextures(1, tex);
    if (rb && *rb) p_glDeleteRenderbuffers(1, rb);
    *fbo = 0; *tex = 0; if (rb) *rb = 0;
    s_pass_allocs++;
    *tex = make_tex(internal, w, h, fmt, type);
    if (rb) {
        p_glGenRenderbuffers(1, rb);
        p_glBindRenderbuffer(PSXGL_RENDERBUFFER, *rb);
        p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, w, h);
        p_glBindRenderbuffer(PSXGL_RENDERBUFFER, 0);
    }
    int complete = make_fbo(fbo, *tex, rb ? *rb : 0);
    s_pass_begin_diag.fbo_status = (uint32_t)s_last_fbo_status;
    s_pass_begin_diag.gl_error = pass_gl_errors();
    if (!complete) {
        *cur_w = *cur_h = 0;
        return 0;
    }
    *cur_w = w; *cur_h = h;
    return 1;
}

static void pass_gen_release(int gi) {
    if (s_ctx && s_pgen_alloc_n[gi])
        glDeleteTextures((GLsizei)s_pgen_alloc_n[gi], s_pgen_tex[gi]);
    memset(s_pgen_tex[gi], 0, sizeof s_pgen_tex[gi]);
    s_pgen_alloc_n[gi] = 0;
    s_pgen_alloc_w[gi] = s_pgen_alloc_h[gi] = 0;
}

/* Make sure slots [0, need) of generation gi exist at w x h. A size change
 * frees the old set first (the slot cap depends on the size only). */
static int pass_gen_reserve(int gi, uint32_t need, int w, int h) {
    if (need > PASS_SLOTS || need > pass_slot_cap(w, h)) {
        s_pass_begin_diag.resource = "generation_slot_cap";
        return 0;
    }
    if (s_pgen_alloc_w[gi] != w || s_pgen_alloc_h[gi] != h) {
        pass_gen_release(gi);
        s_pgen_alloc_w[gi] = w;
        s_pgen_alloc_h[gi] = h;
    }
    while (s_pgen_alloc_n[gi] < need) {
        GLuint *t = &s_pgen_tex[gi][s_pgen_alloc_n[gi]];
        s_pass_begin_diag.gl_error_before = pass_gl_errors();
        s_pass_begin_diag.resource = "generation_texture";
        glGenTextures(1, t);
        if (!*t) {
            s_pass_begin_diag.gl_error = pass_gl_errors();
            return 0;
        }
        glBindTexture(GL_TEXTURE_2D, *t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        s_pass_begin_diag.gl_error = pass_gl_errors();
        s_pgen_alloc_n[gi]++;
        s_pass_allocs++;
    }
    return 1;
}

/* Copy the rect as the presenter would see it into `tex`. Native-wide first
 * refreshes the band's canonical centre, as the wide present does. */
static void pass_capture_into(GLuint tex, const PassGen *g) {
    int S = s_hr_scale;
    hiw_flush_queue();   /* queued wide mirrors land before the capture reads */
    if (g->source_path == GL_PRES_WIDE) {
        GLuint wf = pass_wide_fbo_for(g->x);
        int native_w = g_wide_w - 2 * g_wide_off;
        if (!wf) return;
        if (wide_center_is_canonical() && native_w > 0) {
            /* Colour only: the centre's stencil is left behind (s_wst_*). */
            wst_add(wide_index(wf), g_wide_off, g->y, g_wide_off + native_w, g->y + g->h);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, wf);
            glDisable(GL_SCISSOR_TEST);
            p_glBlitFramebuffer(g->x * S, g->y * S, (g->x + native_w) * S,
                                (g->y + g->h) * S, g_wide_off * S, g->y * S,
                                (g_wide_off + native_w) * S, (g->y + g->h) * S,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
        }
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, wf);
        glBindTexture(GL_TEXTURE_2D, tex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, g->y * S,
                            g_wide_w * S, g->h * S);
    } else {
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
        glBindTexture(GL_TEXTURE_2D, tex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g->x * S, g->y * S,
                            g->w * S, g->h * S);
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
}

static void pass_blit(GLuint src, GLuint dst, int sx, int sy, int dx, int dy,
                      int w, int h, GLbitfield mask) {
    hiw_flush_queue();   /* queued wide mirrors land before a copy reads or writes */
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, src);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, dst);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, mask,
                        GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}

static void pass_verify_read(uint8_t **hr, size_t *hr_cap, uint8_t **raw,
                             size_t *raw_cap) {
    hiw_flush_queue();
    int S = s_hr_scale;
    size_t hn = (size_t)s_pass_w * S * (size_t)s_pass_h * S * 4u;
    size_t rn = (size_t)s_pass_w * (size_t)s_pass_h * 2u;
    if (*hr_cap < hn) { free(*hr); *hr = (uint8_t *)malloc(hn); *hr_cap = *hr ? hn : 0; }
    if (*raw_cap < rn) { free(*raw); *raw = (uint8_t *)malloc(rn); *raw_cap = *raw ? rn : 0; }
    if (!*hr || !*raw) return;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    glReadPixels(s_pass_x * S, s_pass_y * S, s_pass_w * S, s_pass_h * S,
                 GL_RGBA, GL_UNSIGNED_BYTE, *hr);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_raw_fbo);
    glReadPixels(s_pass_x, s_pass_y, s_pass_w, s_pass_h, PSXGL_RED_INTEGER,
                 GL_UNSIGNED_SHORT, *raw);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
}

/* VRAM journal for out-of-rect writes during a pass (see pass_refuse_write). */
#define PASS_JOURNAL_MAX RENDER_PASS_JOURNAL_MAX
typedef struct PassJournal {
    GLuint hr_tex, hr_rb, hr_fbo, raw_tex, raw_fbo;
    int hr_w, hr_h, raw_w, raw_h;
} PassJournal;
static PassJournal s_pj[PASS_JOURNAL_MAX];
static uint64_t s_pj_total = 0;

static void pass_free_color_fbo(GLuint *tex, GLuint *rb, GLuint *fbo,
                                int *w, int *h) {
    if (s_ctx) {
        if (*fbo) p_glDeleteFramebuffers(1, fbo);
        if (*tex) glDeleteTextures(1, tex);
        if (rb && *rb) p_glDeleteRenderbuffers(1, rb);
    }
    *fbo = 0; *tex = 0;
    if (rb) *rb = 0;
    *w = *h = 0;
}

/* Context teardown (gl_renderer_shutdown): free the pass images, backups
 * and journal, and forget their names so a new context makes fresh ones. */
static void pass_resources_release(void) {
    psx_openxr_shutdown();
    if(s_ctx) {
        if(s_xr_color_prog && p_glDeleteProgram) p_glDeleteProgram(s_xr_color_prog);
        if(s_xr_native_tex) glDeleteTextures(1,&s_xr_native_tex);
    }
    s_xr_color_prog=s_xr_native_tex=0;
    stereo_resources_release();
    pass_gen_release(0);
    pass_gen_release(1);
    pass_gens_invalidate();
    pass_free_color_fbo(&s_pb_hr_tex, &s_pb_hr_rb, &s_pb_hr_fbo,
                        &s_pb_hr_w, &s_pb_hr_h);
    pass_free_color_fbo(&s_pb_raw_tex, NULL, &s_pb_raw_fbo,
                        &s_pb_raw_w, &s_pb_raw_h);
    pass_free_color_fbo(&s_pb_wide_tex, &s_pb_wide_rb, &s_pb_wide_fbo,
                        &s_pb_wide_w, &s_pb_wide_h);
    for (int i = 0; i < PASS_JOURNAL_MAX; i++) {
        PassJournal *e = &s_pj[i];
        pass_free_color_fbo(&e->hr_tex, &e->hr_rb, &e->hr_fbo, &e->hr_w, &e->hr_h);
        pass_free_color_fbo(&e->raw_tex, NULL, &e->raw_fbo, &e->raw_w, &e->raw_h);
    }
    render_pass_journal_free(&s_pj_cpu);
    s_pb_valid = 0;
}

static int pass_journal_protect(int x, int y, int w, int h) {
    int S = s_hr_scale, i = s_pj_cpu.n;
    PassJournal *j;
    if (i >= PASS_JOURNAL_MAX) return 0;
    j = &s_pj[i];
    if (!pass_make_color_fbo(&j->hr_tex, &j->hr_rb, &j->hr_fbo, &j->hr_w,
                             &j->hr_h, w * S, h * S, GL_RGBA8, GL_RGBA,
                             GL_UNSIGNED_BYTE) ||
        !pass_make_color_fbo(&j->raw_tex, NULL, &j->raw_fbo, &j->raw_w,
                             &j->raw_h, w, h, PSXGL_R16UI, PSXGL_RED_INTEGER,
                             GL_UNSIGNED_SHORT))
        return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (render_pass_journal_add(&s_pj_cpu, s_vram, VRAM_W, x, y, w, h) != i)
        return 0;
    pass_blit(s_hr_fbo, j->hr_fbo, x * S, y * S, 0, 0, w * S, h * S,
              GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    pass_blit(s_raw_fbo, j->raw_fbo, x, y, 0, 0, w, h, GL_COLOR_BUFFER_BIT);
    s_pj_total++;
    return 1;
}

static void pass_journal_rollback(void) {
    int S = s_hr_scale;
    for (int i = s_pj_cpu.n - 1; i >= 0; i--) {
        PassJournal *j = &s_pj[i];
        int x = s_pj_cpu.x[i], y = s_pj_cpu.y[i];
        int w = s_pj_cpu.w[i], h = s_pj_cpu.h[i];
        pass_blit(j->hr_fbo, s_hr_fbo, 0, 0, x * S, y * S, w * S, h * S,
                  GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        pass_blit(j->raw_fbo, s_raw_fbo, 0, 0, x, y, w, h, GL_COLOR_BUFFER_BIT);
    }
    render_pass_journal_rollback(&s_pj_cpu, s_vram, VRAM_W);
}

/* kind: 0 temporal pass, 1 stereo eye, 2 netplay local view. */
static int transaction_begin(int x, int y, int w, int h, int open_gen,
                              uint32_t period_vblanks, int reuse_backup, int kind) {
    const int stereo = kind == 1, local = kind == 2;
    int S = s_hr_scale, gi, wide, tw, th;
    PassGen *g;
    memset(&s_pass_begin_diag, 0, sizeof s_pass_begin_diag);
    s_pass_begin_diag.status = local ? gl_renderer_local_view_unavailable()
                             : stereo ? gl_renderer_stereo_unavailable()
                                      : gl_renderer_pass_unavailable();
    s_pass_begin_diag.active = s_pass_active;
    s_pass_begin_diag.open_gen = open_gen;
    s_pass_begin_diag.hr_scale = S;
    s_pass_begin_diag.out_scale = s_out_scale;
    s_pass_begin_diag.source_path = s_interp_source_path;
    s_pass_begin_diag.capture_w = s_interp_w;
    s_pass_begin_diag.capture_h = s_interp_h;
    gi = 1 - s_pgen_cur;
    g = &s_pgen[gi];
    s_pass_begin_diag.generation = gi;
    s_pass_begin_diag.valid = g->valid;
    s_pass_begin_diag.promoted = g->promoted;
    s_pass_begin_diag.generation_x = g->x;
    s_pass_begin_diag.generation_y = g->y;
    s_pass_begin_diag.generation_w = g->w;
    s_pass_begin_diag.generation_h = g->h;
    if (s_pass_begin_diag.status != PSX_MOD_RENDER_PASS_READY)
        return pass_begin_refuse("gl_status");
    if (s_pass_active) return pass_begin_refuse("gl_active");
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > VRAM_W || y + h > VRAM_H)
        return pass_begin_refuse("rect_range");
    s_pass_allocs_begin = s_pass_allocs;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();

    wide = s_interp_source_path == GL_PRES_WIDE && g_wide_w > 0 &&
           pass_wide_fbo_for(x) != 0;
    /* Capture what the presenter captures of a displayed buffer: its crop
     * (the VRAM path) applied to the pass rect. Temporal passes only: a
     * stereo pair and the netplay local view keep the plugin's rect. */
    int cx = x, cy = y, cw = w, ch = h;
    if (!local && !stereo && !wide && s_present_crop_w > 0 && s_present_crop_h > 0) {
        cx = x + s_present_crop_dx; cy = y + s_present_crop_dy;
        cw = s_present_crop_w;      ch = s_present_crop_h;
        if (cx < x || cy < y || cx + cw > x + w || cy + ch > y + h)
            return pass_begin_refuse("present_crop");
    }
    tw = (wide ? g_wide_w : cw) * S;
    th = ch * S;
    s_pass_begin_diag.wide = wide;
    s_pass_begin_diag.requested_w = tw;
    s_pass_begin_diag.requested_h = th;
    gi = 1 - s_pgen_cur;
    g = &s_pgen[gi];
    if (local) {
        /* No generation: the image stays in the presenter surface. */
    } else if (stereo) {
        StereoPair *pair = &s_stereo_pair[1 - s_stereo_current];
        pair->x = x; pair->y = y; pair->w = w; pair->h = h;
    } else if (open_gen) {
        if (tw != s_interp_w || th != s_interp_h)
            return pass_begin_refuse("capture_size"); /* not what is presented */
        if (!pass_gen_reserve(gi, 1u, tw, th))
            return pass_begin_refuse("generation_reserve");
        memset(g, 0, sizeof *g);
        g->x = cx; g->y = cy; g->w = cw; g->h = ch;
        g->rx = x; g->ry = y; g->rw = w; g->rh = h;
        g->tex_w = tw; g->tex_h = th;
        g->source_path = wide ? GL_PRES_WIDE : GL_PRES_VRAM;
        g->period = period_vblanks ? period_vblanks : 1u;
        g->shown = s_pass_flip_shown;
        pass_capture_into(s_pgen_tex[gi][0], g);   /* the game's own image */
        g->phase[0] = 0;
        g->n = 1;
        g->valid = 1;
    } else if (!g->valid || g->promoted || g->rx != x || g->ry != y ||
               g->rw != w || g->rh != h) {
        return pass_begin_refuse("generation_state");
    }

    /* The previous pass of this frame restored exactly this backup and no
     * guest code ran since: VRAM and the coherency state already equal it. */
    if (reuse_backup && s_pb_valid && s_pb_x == x && s_pb_y == y &&
        s_pb_w == w && s_pb_h == h && s_pb_scale == S &&
        s_pb_wide_src == (g_wide_w > 0 ? pass_wide_fbo_for(x) : 0)) {
        s_pb_reused++;
        goto backed_up;
    }
    s_pb_valid = 0;
    /* Back up the rect: hr color + stencil, raw mirror, wide band, CPU rows. */
    s_pass_begin_diag.resource = "backup_hr";
    if (!pass_make_color_fbo(&s_pb_hr_tex, &s_pb_hr_rb, &s_pb_hr_fbo,
                             &s_pb_hr_w, &s_pb_hr_h, w * S, h * S,
                             GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE))
        return pass_begin_refuse("backup_hr");
    s_pass_begin_diag.resource = "backup_raw";
    if (!pass_make_color_fbo(&s_pb_raw_tex, NULL, &s_pb_raw_fbo,
                             &s_pb_raw_w, &s_pb_raw_h, w, h, PSXGL_R16UI,
                             PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT))
        return pass_begin_refuse("backup_raw");
    pass_blit(s_hr_fbo, s_pb_hr_fbo, x * S, y * S, 0, 0, w * S, h * S,
              GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    pass_blit(s_raw_fbo, s_pb_raw_fbo, x, y, 0, 0, w, h, GL_COLOR_BUFFER_BIT);
    s_pb_wide_src = g_wide_w > 0 ? pass_wide_fbo_for(x) : 0;
    if (s_pb_wide_src) {
        s_pass_begin_diag.resource = "backup_wide";
        if (!pass_make_color_fbo(&s_pb_wide_tex, &s_pb_wide_rb, &s_pb_wide_fbo,
                                 &s_pb_wide_w, &s_pb_wide_h, g_wide_w * S,
                                 h * S, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE))
            return pass_begin_refuse("backup_wide");
        pass_blit(s_pb_wide_src, s_pb_wide_fbo, 0, y * S, 0, 0, g_wide_w * S,
                  h * S, GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    if (s_pb_cpu_cap < (size_t)w * (size_t)h) {
        free(s_pb_cpu);
        s_pb_cpu = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
        s_pb_cpu_cap = s_pb_cpu ? (size_t)w * (size_t)h : 0;
        if (!s_pb_cpu) return pass_begin_refuse("backup_cpu");
    }
    for (int row = 0; row < h; row++)
        memcpy(s_pb_cpu + (size_t)row * w, s_vram + (size_t)(y + row) * VRAM_W + x,
               (size_t)w * sizeof(uint16_t));
    s_pb_cpu_dirty = s_cpu_dirty;
    s_pb_pack_dirty = s_pack_dirty;
    memcpy(s_pb_up_rects, s_up_rects, sizeof s_pb_up_rects);
    s_pb_up_n = s_up_nrects;
    s_pb_gpu_dirty = s_gpu_dirty;
    s_pb_stencil_valid = s_stencil_valid;
    memcpy(s_pb_present_dirty, s_present_dirty, sizeof s_pb_present_dirty);
    s_pb_force_present = s_force_present_remaining;
    s_pb_last_path = s_last_present_path;
    s_pb_last_dx = s_last_dx; s_pb_last_dy = s_last_dy;
    s_pb_last_dw = s_last_dw; s_pb_last_dh = s_last_dh;
    s_pb_x = x; s_pb_y = y; s_pb_w = w; s_pb_h = h; s_pb_scale = S;

backed_up:
    s_pass_x = x; s_pass_y = y; s_pass_w = w; s_pass_h = h;
    if (s_pass_verify < 0) {
        const char *e = getenv("PSX_RENDER_PASS_VERIFY");
        s_pass_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (s_pass_verify) {
        pass_verify_read(&s_pv_hr, &s_pv_hr_cap, &s_pv_raw, &s_pv_raw_cap);
        s_pv_cpu_hash = pass_cpu_vram_hash();
    }
    s_pj_cpu.n = 0;
    s_pass_active = 1;
    s_pass_local = local;
    return 1;
}

/* keep_color: a committed netplay local view keeps the rect's presented
 * colour (hr surface, native-wide band); everything else is put back. */
static void transaction_restore_ex(int keep_color) {
    int S = s_hr_scale, gi = 1 - s_pgen_cur;
    const GLbitfield hr_bits = keep_color ? GL_STENCIL_BUFFER_BIT
                                          : GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    (void)gi;
    if (!s_pass_active) return;
    /* Roll the journal, then the rect back. */
    pass_journal_rollback();
    pass_blit(s_pb_hr_fbo, s_hr_fbo, 0, 0, s_pass_x * S, s_pass_y * S,
              s_pass_w * S, s_pass_h * S, hr_bits);
    pass_blit(s_pb_raw_fbo, s_raw_fbo, 0, 0, s_pass_x, s_pass_y, s_pass_w,
              s_pass_h, GL_COLOR_BUFFER_BIT);
    if (s_pb_wide_src && s_pb_wide_src == pass_wide_fbo_for(s_pass_x)) {
        pass_blit(s_pb_wide_fbo, s_pb_wide_src, 0, 0, 0, s_pass_y * S,
                  g_wide_w * S, s_pass_h * S, hr_bits);
        /* The backed-up stencil may predate a rebuild inside the pass. */
        wst_add(wide_index(s_pb_wide_src), 0, s_pass_y, g_wide_w, s_pass_y + s_pass_h);
    }
    for (int row = 0; row < s_pass_h; row++)
        memcpy(s_vram + (size_t)(s_pass_y + row) * VRAM_W + s_pass_x,
               s_pb_cpu + (size_t)row * s_pass_w,
               (size_t)s_pass_w * sizeof(uint16_t));
    s_cpu_dirty = s_pb_cpu_dirty;
    s_pack_dirty = s_pb_pack_dirty;
    memcpy(s_up_rects, s_pb_up_rects, sizeof s_up_rects);
    s_up_nrects = s_pb_up_n;
    s_gpu_dirty = s_pb_gpu_dirty;
    s_stencil_valid = s_pb_stencil_valid;
    memcpy(s_present_dirty, s_pb_present_dirty, sizeof s_present_dirty);
    s_force_present_remaining = s_pb_force_present;
    s_last_present_path = s_pb_last_path;
    s_last_dx = s_pb_last_dx; s_last_dy = s_pb_last_dy;
    s_last_dw = s_pb_last_dw; s_last_dh = s_pb_last_dh;
    s_pass_active = 0;
    s_pass_local = 0;
    s_pb_valid = !keep_color;
    if (keep_color) {
        /* The rect now differs from the backup and from the last present. */
        present_dirty_rect(s_pass_x, s_pass_y, s_pass_x + s_pass_w - 1,
                           s_pass_y + s_pass_h - 1, 1);
        s_pv_ok = 1; /* the kept rect is the product, not a leak */
        return;
    }

    if (s_pass_verify) {
        static uint8_t *after_hr = NULL, *after_raw = NULL;
        static size_t after_hr_cap = 0, after_raw_cap = 0;
        size_t hn = (size_t)s_pass_w * S * (size_t)s_pass_h * S * 4u;
        size_t rn = (size_t)s_pass_w * (size_t)s_pass_h * 2u;
        pass_verify_read(&after_hr, &after_hr_cap, &after_raw, &after_raw_cap);
        s_pv_ok = s_pv_hr && after_hr && s_pv_raw && after_raw &&
                  memcmp(s_pv_hr, after_hr, hn) == 0 &&
                  memcmp(s_pv_raw, after_raw, rn) == 0 &&
                  pass_cpu_vram_hash() == s_pv_cpu_hash;
    }
}

static void transaction_restore(void) { transaction_restore_ex(0); }

uint32_t gl_renderer_local_view_unavailable(void) {
    pass_refusal_init();
    if (!s_ctx || !s_raster_ok || !s_hr_fbo || gpu_display_is_depth24())
        return PSX_MOD_RENDER_PASS_NO_PRESENTER;
    /* Only a presenter surface separate from the authoritative VRAM can hold
     * a peer's own image; the high-resolution window presents other tiles. */
    if (!s_cpu_auth_dual || s_hiw || s_pass_force_refuse > 0)
        return PSX_MOD_RENDER_PASS_BACKEND;
    return PSX_MOD_RENDER_PASS_READY;
}
int gl_renderer_local_view_begin(int x, int y, int w, int h) {
    GL_RT_SYNC("local_view_begin");
    return transaction_begin(x, y, w, h, 0, 0, 0, 2);
}
int gl_renderer_local_view_end(int keep) {
    GL_RT_SYNC("local_view_end");
    if (!s_pass_active || !s_pass_local) return 0;
    flush_flat_batch(); flush_tex_batch(); flush_cpu_upload();
    transaction_restore_ex(keep ? 1 : 0);
    if (keep) s_local_commits++;
    return 1;
}

int gl_renderer_pass_begin(int x, int y, int w, int h, int open_gen,
                           uint32_t period, int reuse) {
    GL_RT_SYNC("pass_begin");
    return transaction_begin(x, y, w, h, open_gen, period, reuse, 0);
}
int gl_renderer_stereo_begin(int x, int y, int w, int h, int reuse) {
    GL_RT_SYNC("stereo_begin");
    return transaction_begin(x, y, w, h, 0, 0, reuse, 1);
}
void gl_renderer_pass_end(uint32_t alpha_q16, int keep) {
    GL_RT_SYNC("pass_end");
    int gi = 1 - s_pgen_cur;
    PassGen *g = &s_pgen[gi];
    if (!s_pass_active) return;
    flush_flat_batch(); flush_tex_batch(); flush_cpu_upload();
    if (keep && alpha_q16 && g->valid && !g->promoted && g->n < PASS_SLOTS &&
        g->n < pass_slot_cap(g->tex_w, g->tex_h) &&
        pass_gen_reserve(gi, g->n + 1u, g->tex_w, g->tex_h)) {
        uint32_t slot = g->n, at = g->n;
        GLuint t;
        pass_capture_into(s_pgen_tex[gi][slot], g);
        while (at > 1 && g->phase[at - 1] > alpha_q16) at--;
        t = s_pgen_tex[gi][slot];
        for (uint32_t i = slot; i > at; i--) {
            s_pgen_tex[gi][i] = s_pgen_tex[gi][i - 1];
            g->phase[i] = g->phase[i - 1];
        }
        s_pgen_tex[gi][at] = t; g->phase[at] = alpha_q16; g->n++;
    }
    transaction_restore();
}

uint32_t gl_renderer_stereo_unavailable(void) {
    GL_RT_SYNC("stereo_unavailable");
    pass_refusal_init();
    if (!s_ctx || !s_raster_ok || !s_hr_fbo || ctx_depth24())
        return PSX_MOD_RENDER_PASS_NO_PRESENTER;
    if (s_cpu_auth_dual || s_hd_native_authority || s_hiw || g_wide_w > 0 || s_pass_force_refuse > 0)
        return PSX_MOD_RENDER_PASS_BACKEND;
    return PSX_MOD_RENDER_PASS_READY;
}
void gl_renderer_stereo_stage_reset(void) {
    GL_RT_SYNC("stereo_stage_reset");
    s_stereo_pair[1 - s_stereo_current].mask = 0;
}
static void stereo_invalidate(void) {
    s_stereo_valid = 0;
    s_stereo_pair[0].mask = s_stereo_pair[1].mask = 0;
}
void gl_renderer_stereo_reset(void) {
    GL_RT_SYNC("stereo_reset");
    stereo_invalidate(); s_stereo_mode = 0; s_stereo_dump_left = 0;
    s_stereo_capture_count = 0;
    s_stereo_presents = 0;
}
int gl_renderer_stereo_set_presentation(uint32_t mode) {
    GL_RT_SYNC("stereo_set_presentation");
    if (mode > 1u) return 0;
    s_stereo_mode = (int)mode;
    return 1;
}
static void stereo_resources_release(void) {
    for (int gi = 0; gi < 2; gi++) for (int eye = 0; eye < 2; eye++) {
        StereoPair *p = &s_stereo_pair[gi];
        pass_free_color_fbo(&p->tex[eye], NULL, &p->fbo[eye],
                            &p->tw[eye], &p->th[eye]);
    }
    stereo_invalidate();
}
int gl_renderer_stereo_end(uint32_t eye, int keep) {
    GL_RT_SYNC("stereo_end");
    StereoPair *p = &s_stereo_pair[1 - s_stereo_current];
    PassGen g;
    int ok = 0, tw = s_pass_w * s_hr_scale, th = s_pass_h * s_hr_scale;
    if (!s_pass_active) return 0;
    flush_flat_batch(); flush_tex_batch(); flush_cpu_upload();
    if (keep && eye < 2u && (double)tw * th * 16.0 <= 256.0 * 1024.0 * 1024.0 &&
        pass_make_color_fbo(&p->tex[eye], NULL, &p->fbo[eye],
                            &p->tw[eye], &p->th[eye], tw, th,
                            GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE)) {
        memset(&g, 0, sizeof g);
        g.x = s_pass_x; g.y = s_pass_y; g.w = s_pass_w; g.h = s_pass_h;
        g.source_path = GL_PRES_VRAM;
        (void)pass_gl_errors(); /* distinguish this copy from preceding errors */
        pass_capture_into(p->tex[eye], &g);
        ok = pass_gl_errors() == 0;
        if (ok) p->mask |= 1u << eye;
    }
    transaction_restore();
    return keep ? ok : 1;
}
void gl_renderer_stereo_diag(GLRenderStereoDiag *out) {
    GL_RT_SYNC("stereo_diag");
    StereoPair *p = &s_stereo_pair[s_stereo_current];
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->valid = (uint32_t)s_stereo_valid; out->mode = (uint32_t)s_stereo_mode;
    out->staged_mask = s_stereo_pair[1 - s_stereo_current].mask;
    out->pair_id = p->id; out->guest_cycle = p->cycle;
    out->width = (uint32_t)p->tw[0]; out->height = (uint32_t)p->th[0];
    out->presents = s_stereo_presents;
}
void gl_renderer_stereo_dump_arm(const char *dir, int pairs) {
    GL_RT_SYNC("stereo_dump_arm");
    if (!dir || !*dir || pairs <= 0) { s_stereo_dump_left = 0; return; }
    snprintf(s_stereo_dump_dir, sizeof s_stereo_dump_dir, "%s", dir);
    s_stereo_dump_left = pairs > STEREO_CAPTURE_MAX ? STEREO_CAPTURE_MAX : pairs;
    s_stereo_capture_count = 0;
}
uint32_t gl_renderer_stereo_capture_records(GLRenderStereoCapture *out, uint32_t capacity) {
    GL_RT_SYNC("stereo_capture_records");
    uint32_t count = s_stereo_capture_count;
    if (!out) return 0;
    if (count > capacity) count = capacity;
    memcpy(out, s_stereo_captures, count * sizeof *out);
    return count;
}
static void stereo_dump(const StereoPair *p) {
    int w = p->tw[0], h = p->th[0];
    size_t n = (size_t)w * h * 3u;
    uint8_t *rgb = (uint8_t *)malloc(n * 2u);
    uint8_t *sbs = (uint8_t *)malloc(n * 2u);
    char path[512]; FILE *f;
    if (!rgb || !sbs) { free(rgb); free(sbs); return; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int eye = 0; eye < 2; eye++) {
        glBindTexture(GL_TEXTURE_2D, p->tex[eye]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb + eye * n);
        snprintf(path, sizeof path, "%s/p%06llu_%s.png", s_stereo_dump_dir,
                 (unsigned long long)p->id, eye ? "right" : "left");
        f = fopen(path, "wb");
        if (f) { (void)png_write_rgb(f, rgb + eye * n, w, h); fclose(f); }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    for (int row = 0; row < h; row++) for (int eye = 0; eye < 2; eye++)
        memcpy(sbs + ((size_t)row * w * 2u + eye * w) * 3u,
               rgb + eye * n + (size_t)row * w * 3u, (size_t)w * 3u);
    snprintf(path, sizeof path, "%s/p%06llu_sbs.png", s_stereo_dump_dir,
             (unsigned long long)p->id);
    f = fopen(path, "wb");
    if (f) { (void)png_write_rgb(f, sbs, w * 2u, h); fclose(f); }
    if (s_stereo_capture_count < STEREO_CAPTURE_MAX) {
        GLRenderStereoCapture *record = &s_stereo_captures[s_stereo_capture_count++];
        record->pair_id = p->id; record->guest_cycle = p->cycle;
        record->width = (uint32_t)w; record->height = (uint32_t)h;
        memcpy(record->view_offset, p->view, sizeof record->view_offset);
    }
    free(rgb); free(sbs); s_stereo_dump_left--;
}
int gl_renderer_stereo_publish(uint64_t id, uint64_t cycle, const int32_t view[2][3]) {
    GL_RT_SYNC("stereo_publish");
    StereoPair *p = &s_stereo_pair[1 - s_stereo_current];
    if (p->mask != 3u || p->tw[0] != p->tw[1] || p->th[0] != p->th[1]) return 0;
    p->id = id; p->cycle = cycle;
    memcpy(p->view, view, sizeof p->view);
    s_stereo_current = 1 - s_stereo_current; s_stereo_valid = 1;
    if (s_stereo_dump_left > 0) stereo_dump(p);
    return 1;
}

static uint64_t s_xr_begin_pair;
int psx_mod_openxr_enable(int enabled) {
    GL_RT_SYNC("openxr_enable");
    PSXOpenXRStats s; psx_openxr_stats(&s);
    if (s_pass_active || s.frame_open) return 0;
    return psx_openxr_enable(enabled);
}
void psx_mod_openxr_recenter(void) {
    GL_RT_SYNC("openxr_recenter"); psx_openxr_recenter(); }
int psx_mod_openxr_quad(double distance,double width,double height) {
    GL_RT_SYNC("openxr_quad");
    if(s_pass_active)return 0;
    return psx_openxr_quad(distance,width,height);
}
int psx_mod_openxr_native_surface(double distance,double width,double units) {
    GL_RT_SYNC("openxr_native_surface");
    PSXOpenXRStats stats;psx_openxr_stats(&stats);
    if(s_pass_active || stats.frame_open || !stats.compiled ||
       !isfinite(distance) || !isfinite(width) || !isfinite(units))return 0;
    if(distance && (distance<.25 || distance>20 || width<=0 || width>10 || units<1 || units>65536))return 0;
    s_native_surface_enabled=distance!=0;s_native_surface_pending=0;
    s_native_surface_distance=distance;s_native_surface_width=width;s_native_surface_units=units;
    return 1;
}
int psx_mod_openxr_begin(uint32_t width, uint32_t height, double units) {
    GL_RT_SYNC("openxr_begin");
    if (s_native_surface_enabled || s_pass_active || gl_renderer_stereo_unavailable() != PSX_MOD_RENDER_PASS_READY ||
        !width || !height || width > VRAM_W || height > VRAM_H) return 0;
    s_xr_begin_pair = s_stereo_valid ? s_stereo_pair[s_stereo_current].id : 0;
    return psx_openxr_begin((int)width, (int)height, units);
}
int psx_mod_openxr_view(uint32_t eye, PSXModRenderView *view) {
    GL_RT_SYNC("openxr_view");
    return psx_openxr_view(eye, view);
}
int psx_mod_openxr_input(PSXModOpenXRInput *input) {
    GL_RT_SYNC("openxr_input");
    if (s_pass_active) return 0;
    return psx_openxr_input(input);
}
int psx_mod_openxr_hands(PSXModOpenXRHands *hands) {
    GL_RT_SYNC("openxr_hands");
    return psx_openxr_hands(hands); /* Snapshot only, also safe during replay. */
}
/* The target stores display-encoded RGB for an sRGB swapchain, or explicitly
 * decoded RGB for a linear-only runtime. Never apply hardware sRGB encoding
 * to these outputs. All shader state is restored before returning to native. */
static int openxr_color_draw(GLuint source, int w, int h, int flip, float gamma, int linear) {
    if (!s_xr_color_prog) s_xr_color_prog = build_program(PRESENT_VS, PSX_XR_COLOR_FS);
    if (!s_xr_color_prog || !s_present_vao) return 0;
    GLint program, vao, viewport[4], active, binding, min_filter, mag_filter;
    GLboolean mask[4];
    const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE};
    GLboolean enabled[4];
    glGetIntegerv(0x8B8D, &program);glGetIntegerv(0x85B5, &vao);
    glGetIntegerv(GL_VIEWPORT, viewport);glGetIntegerv(0x84E0, &active);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    p_glActiveTexture(PSXGL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    glBindTexture(GL_TEXTURE_2D, source);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    for (int i=0;i<4;i++) {enabled[i]=glIsEnabled(caps[i]);glDisable(caps[i]);}
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);glViewport(0,0,w,h);
    p_glUseProgram(s_xr_color_prog);
    p_glUniform1i(p_glGetUniformLocation(s_xr_color_prog,"u_tex"),0);
    p_glUniform1i(p_glGetUniformLocation(s_xr_color_prog,"u_linear"),linear);
    p_glUniform1f(p_glGetUniformLocation(s_xr_color_prog,"u_gamma"),gamma);
    p_glUniform4f(p_glGetUniformLocation(s_xr_color_prog,"u_uv_rect"),0,flip?0:1,1,flip?1:0);
    p_glBindVertexArray(s_present_vao);glDrawArrays(GL_TRIANGLES,0,3);
    p_glBindVertexArray((GLuint)vao);p_glUseProgram((GLuint)program);
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    glColorMask(mask[0],mask[1],mask[2],mask[3]);
    for (int i=0;i<4;i++) if(enabled[i]) glEnable(caps[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    glBindTexture(GL_TEXTURE_2D,(GLuint)binding);p_glActiveTexture((GLenum)active);
    return 1;
}
static int openxr_copy_eye(uint32_t eye, uint32_t texture, int w, int h) {
    StereoPair *p = &s_stereo_pair[s_stereo_current];
    GLint read_fbo, draw_fbo; GLuint target = 0;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean srgb = glIsEnabled(0x8DB9); /* GL_FRAMEBUFFER_SRGB */
    PSXOpenXRStats xr;psx_openxr_stats(&xr);
    glGetIntegerv(0x8CAA, &read_fbo);
    glGetIntegerv(0x8CA6, &draw_fbo);
    p_glGenFramebuffers(1, &target);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, target);
    p_glFramebufferTexture2D(PSXGL_DRAW_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0,GL_TEXTURE_2D, texture, 0);
    int ok = p_glCheckFramebufferStatus(PSXGL_DRAW_FRAMEBUFFER) == PSXGL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glDisable(GL_SCISSOR_TEST);
        glDisable(0x8DB9);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, p->fbo[eye]);
        /* Stereo capture texture row convention is opposite the XR layer.
         * User confirmed inverted headset output with the original direct blit.
         * Flip only submission; eye dumps and desktop presentation stay intact. */
        if (xr.swapchain_format == PSX_XR_SRGB8_ALPHA8 && s_present_gamma == 1.0f)
            p_glBlitFramebuffer(0, p->th[eye], p->tw[eye], 0, 0, 0, w, h,
                                GL_COLOR_BUFFER_BIT, GL_LINEAR);
        else
            ok = openxr_color_draw(p->tex[eye], w, h, 1, s_present_gamma,
                                   xr.swapchain_format != PSX_XR_SRGB8_ALPHA8);
        glFlush();
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, (GLuint)read_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (srgb) glEnable(0x8DB9);
    p_glDeleteFramebuffers(1, &target);return ok;
}
int psx_mod_openxr_end(int rendered) {
    GL_RT_SYNC("openxr_end");
    int fresh = rendered && s_stereo_valid && !s_pass_active &&
                s_stereo_pair[s_stereo_current].id != s_xr_begin_pair;
    if (fresh) psx_openxr_pair_metadata(s_stereo_pair[s_stereo_current].id,
                                       s_stereo_pair[s_stereo_current].cycle);
    return psx_openxr_end(fresh, openxr_copy_eye);
}

static int openxr_copy_native(uint32_t eye,uint32_t texture,int w,int h) {
    GLint read_fbo,draw_fbo,read_buffer;GLuint target=0;
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
    GLboolean srgb=glIsEnabled(0x8DB9);
    PSXOpenXRStats xr;psx_openxr_stats(&xr);
    (void)eye;
    glGetIntegerv(0x8CAA,&read_fbo);glGetIntegerv(0x8CA6,&draw_fbo);
    glGetIntegerv(GL_READ_BUFFER,&read_buffer);
    p_glGenFramebuffers(1,&target);p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,target);
    p_glFramebufferTexture2D(PSXGL_DRAW_FRAMEBUFFER,PSXGL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    int ok=p_glCheckFramebufferStatus(PSXGL_DRAW_FRAMEBUFFER)==PSXGL_FRAMEBUFFER_COMPLETE;
    if(ok) {
        int *r=s_native_surface_rect;
        glDisable(GL_SCISSOR_TEST);p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,0);glReadBuffer(s_pt_on?PSXGL_COLOR_ATTACHMENT0:GL_BACK);
        glDisable(0x8DB9);
        /* Default framebuffer is already upright: unlike native VRAM/eye
         * textures, its bottom GL row is the displayed bottom row. */
        (void)pass_gl_errors();
        if (xr.swapchain_format == PSX_XR_SRGB8_ALPHA8)
            p_glBlitFramebuffer(r[0],r[1],r[0]+r[2],r[1]+r[3],0,0,w,h,GL_COLOR_BUFFER_BIT,GL_LINEAR);
        else {
            GLint active,binding;glGetIntegerv(0x84E0,&active);
            p_glActiveTexture(PSXGL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
            if(!s_xr_native_tex) glGenTextures(1,&s_xr_native_tex);
            glBindTexture(GL_TEXTURE_2D,s_xr_native_tex);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,r[0],r[1],r[2],r[3],0);
            glBindTexture(GL_TEXTURE_2D,(GLuint)binding);p_glActiveTexture((GLenum)active);
            /* Desktop gamma is already baked into GL_BACK; decode it once. */
            ok=openxr_color_draw(s_xr_native_tex,w,h,0,1.0f,1);
        }
        glFlush();ok=ok && pass_gl_errors()==0;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,(GLuint)read_fbo);glReadBuffer((GLenum)read_buffer);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,(GLuint)draw_fbo);
    if(scissor)glEnable(GL_SCISSOR_TEST);
    if(srgb)glEnable(0x8DB9);
    p_glDeleteFramebuffers(1,&target);return ok;
}
static void openxr_present_native(void) {
    int ww=0,wh=0,*r=s_native_surface_rect;
    if(!s_native_surface_enabled || !s_native_surface_pending || s_pass_active)return;
    s_native_surface_pending=0;
    SDL_GL_GetDrawableSize(s_win,&ww,&wh);
    if(r[0]<0 || r[1]<0 || r[2]<1 || r[3]<1 || r[0]+r[2]>ww || r[1]+r[3]>wh)return;
    /* No replay or retained pair: this exact native present is the source.
     * Direct lifecycle permits native 24-bit videos as well as 15-bit UI. */
    /* The shared locator also builds PSX projection matrices, whose domain
     * is native VRAM dimensions. Quad geometry does not use those matrices;
     * keep that domain bounded while copying the full drawable rectangle. */
    if(!psx_openxr_begin(512,240,s_native_surface_units))return;
    if(!psx_openxr_quad(s_native_surface_distance,s_native_surface_width,
                       s_native_surface_width*(double)r[3]/r[2])) {
        (void)psx_openxr_end(0,NULL);return;
    }
    psx_openxr_native_metadata(s_frame_count);
    (void)psx_openxr_end(1,openxr_copy_native);
}

static int stereo_present(int w, int h) {
    StereoPair *p = &s_stereo_pair[s_stereo_current];
    int ww, wh, lx, ly, lw, lh;
    /* A pair is shown only while the plugin keeps submitting; menus, FMV at the
     * same size and state loads stop publishing, and the last pair must not
     * stick on screen. Stale pairs fall back to the flat present below. */
    if (s_stereo_valid &&
        !render_pass_stereo_pair_fresh(p->cycle, psx_cycle_count, g_psx_vblank_cycles))
        return 0;
    if (!s_stereo_mode || !s_stereo_valid || s_pass_active ||
        p->w != w || p->h != h || p->tw[0] != w * s_hr_scale ||
        gl_renderer_stereo_unavailable() != PSX_MOD_RENDER_PASS_READY) return 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    gl_perf_present_enter();
    letterbox_rect_aspect(ww, wh, s_aspect_num * 2, s_aspect_den, &lx, &ly, &lw, &lh);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glViewport(0, 0, ww, wh);
    glClearColor(0.f, 0.f, 0.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
    for (int eye = 0; eye < 2; eye++) {
        int start = eye ? lw / 2 : 0, width = eye ? lw - lw / 2 : lw / 2;
        present_target_quad(p->tex[eye], p->tw[eye], p->th[eye],
                            0, 0, p->tw[eye], p->th[eye], 0,
                            lx + start, ly, width, lh, 1, 1, 1);
    }
    pres_record(GL_PRES_STEREO, p->x, p->y, w, h, lx, ly, lw, lh);
    hold_capture_drawable();
    latency_ring_mark(LAT_SWAP_BEGIN);
    gl_swap_with_osd();
    latency_ring_mark(LAT_SWAP_END);
    s_stereo_presents++; s_probe_swap++;
    gl_perf_present_exit(0);
    present_dirty_rect(p->x, p->y, p->x + w - 1, p->y + h - 1, 0);
    present_force_consumed();
    s_last_present_path = GL_PRES_STEREO;
    return 1;
}

/* Draw two presented images crossfaded (t = weight of b) into the window. */
static int interp_present_pair(GLuint a, GLuint b, float t, int blend_mode);

static int pass_gen_present(uint64_t deadline) {
    PassGen *g = &s_pgen[s_pgen_cur];
    uint32_t lo = 0, hi = 0;
    float t = 0.0f;
    double p;
    if (!g->valid || !g->promoted || !(g->t_len > 0.0)) return 0;
    if (g->source_path != s_interp_source_path || g->tex_w != s_interp_w ||
        g->tex_h != s_interp_h)
        return 0;
    p = ((double)deadline - g->t_start) / g->t_len;
    /* Past phase 1 the next flip is late (a lagging tick): the newest image
     * holds until it comes. Only a game that stops flipping for several
     * frame lengths expires the generation. */
    if (!render_pass_gen_select(g->phase, g->n, p, &lo, &hi, &t)) {
        g->valid = 0;
        s_pgen_expired++;
        return 0;
    }
    if (!interp_present_pair(s_pgen_tex[s_pgen_cur][lo],
                             s_pgen_tex[s_pgen_cur][hi], t, 0))
        return 0;
    s_pgen_presents++;
    if (lo != hi) s_pgen_blends++;
    /* A frame's own last deadline falls at p = 1 (the end of its planned
     * VBlanks); one past it by more than 1/64 is in a VBlank it was not
     * planned to cover. */
    if (p > 1.0 + 1.0 / 64.0) s_pgen_late++;
    return 1;
}

void gl_renderer_pass_service_presents(void) {
    GL_RT_SYNC("pass_service_presents");
    uint64_t deadline;
    double sp;
    if (!gl_renderer_pass_ready() || s_pass_active) return;
    sp = (double)SDL_GetPerformanceFrequency() / s_interp_source_hz;
    while (frame_interpolation_schedule_due(
               &s_interp_schedule, SDL_GetPerformanceCounter(),
               s_interp_schedule.frame_end + sp, &deadline)) {
        latency_ring_mark(LAT_SWAP_BEGIN);
        if (!pass_gen_present(deadline)) { latency_ring_mark(LAT_SWAP_END); break; }
        latency_ring_mark(LAT_SWAP_END);
        frame_interpolation_schedule_consume(&s_interp_schedule);
        s_pgen_early++;
    }
}

void gl_renderer_pass_diag(uint64_t out[10]) {
    GL_RT_SYNC("pass_diag");
    out[0] = s_pgen_promotions;
    out[1] = s_pgen_presents;
    out[2] = s_pgen_blends;
    out[3] = s_pgen_expired;
    out[4] = s_pgen_unmatched;
    out[5] = s_pgen_early;
    out[6] = (uint64_t)(render_pass_cost_estimate(&s_pass_cost) * 1e6 /
                        (double)SDL_GetPerformanceFrequency()); /* us */
    out[7] = (uint64_t)s_pgen[s_pgen_cur].n;
    out[8] = s_pgen_late;
    out[9] = s_pass_cost_rewarms;
}

uint64_t gl_renderer_pass_journaled(void) {
    GL_RT_SYNC("pass_journaled"); return s_pj_total; }
uint64_t gl_renderer_pass_backups_reused(void) {
    GL_RT_SYNC("pass_backups_reused"); return s_pb_reused; }

uint32_t gl_renderer_pass_image_textures(uint64_t *bytes) {
    GL_RT_SYNC("pass_image_textures");
    uint32_t n = s_pgen_alloc_n[0] + s_pgen_alloc_n[1];
    if (bytes) {
        *bytes = 0;
        for (int gi = 0; gi < 2; gi++)
            *bytes += (uint64_t)s_pgen_alloc_n[gi] * (uint64_t)s_pgen_alloc_w[gi] *
                      (uint64_t)s_pgen_alloc_h[gi] * 4u;
    }
    return n;
}

static uint64_t s_idle_ticks_accum_fwd(uint64_t add) {
    s_idle_ticks_accum += add;
    return s_idle_ticks_accum;
}

static uint64_t s_present_ticks_accum_fwd(uint64_t add) {
    s_present_ticks_accum += add;
    s_present_cost_ema = render_pass_ema(s_present_cost_ema, (double)add);
    return s_present_ticks_accum;
}

void gl_renderer_pass_set_flip_shown(int shown) {
    GL_RT_SYNC("pass_set_flip_shown"); s_pass_flip_shown = shown ? 1 : 0; }

/* FLIP source saw a new frame: it is the flip a pending generation was built
 * for (render_pass_gen_flip_matches) or a frame without passes. */
static void pass_note_new_frame(int origin_x, int origin_y, int source_path,
                                int pw, int ph) {
    PassGen *pend = &s_pgen[1 - s_pgen_cur];
    if (pend->valid && !pend->promoted &&
        render_pass_gen_flip_matches(pend->shown, pend->x, pend->y,
                                     pend->source_path, pend->tex_w, pend->tex_h,
                                     origin_x, origin_y, source_path, pw, ph)) {
        s_pgen_promote = 1;
    } else {
        s_pgen_promote = 0;
        if (s_pgen[s_pgen_cur].valid) s_pgen_unmatched++;
        s_pgen[s_pgen_cur].valid = 0;
        s_pgen[s_pgen_cur].promoted = 0;
    }
}

/* render_pass_dump (debug server): write every image of the next promoted
 * generations as PNGs, <dir>/g<gen>_<index>_a<phase q16>.png. */
static char s_pdump_dir[400];
static int  s_pdump_left = 0;
static uint64_t s_pdump_gen = 0;
void gl_renderer_pass_dump_arm(const char *dir, int generations) {
    GL_RT_SYNC("pass_dump_arm");
    if (!dir || !*dir || generations <= 0) { s_pdump_left = 0; return; }
    snprintf(s_pdump_dir, sizeof s_pdump_dir, "%s", dir);
    s_pdump_left = generations;
}
static void pass_dump_generation(int gi) {
    PassGen *g = &s_pgen[gi];
    size_t n = (size_t)g->tex_w * (size_t)g->tex_h * 3u;
    uint8_t *rgb = (uint8_t *)malloc(n);
    if (!rgb) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (uint32_t i = 0; i < g->n; i++) {
        char path[512];
        FILE *f;
        glBindTexture(GL_TEXTURE_2D, s_pgen_tex[gi][i]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        snprintf(path, sizeof path, "%s/g%03llu_%02u_a%05u.png", s_pdump_dir,
                 (unsigned long long)s_pdump_gen, (unsigned)i,
                 (unsigned)g->phase[i]);
        f = fopen(path, "wb");
        if (f) { (void)png_write_rgb(f, rgb, (uint32_t)g->tex_w, (uint32_t)g->tex_h); fclose(f); }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    free(rgb);
    s_pdump_gen++;
    s_pdump_left--;
}

static void pass_apply_promotion(void) {
    s_intervals_since_plan++;
    if (!s_pgen_promote) return;
    s_pgen_promote = 0;
    s_pgen[s_pgen_cur].valid = 0;
    s_pgen[s_pgen_cur].promoted = 0;
    s_pgen_cur = 1 - s_pgen_cur;
    {
        PassGen *g = &s_pgen[s_pgen_cur];
        double sp = s_interp_schedule.frame_end - s_interp_schedule.frame_start;
        g->promoted = 1;
        g->t_start = s_interp_schedule.frame_start;
        g->t_len = (double)g->period * sp;
        /* Phase 0 is the image the game actually flipped to. The capture at
         * the pass point is the same for a game that draws nothing more into
         * the rect before its flip; a game that finishes the frame later (V8:2
         * draws its HUD at the next submit, onto the rect about to be shown)
         * would otherwise show its own frame without that last layer. The
         * flip was matched to this generation's rect and presented geometry,
         * and no guest code ran since it, so the rect holds that image. A
         * FLIP_SHOWN generation's rect is not the one flipped to: it keeps
         * the image taken when it opened. */
        if (!g->shown) {
            pass_capture_into(s_pgen_tex[s_pgen_cur][0], g);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        }
        s_pgen_promotions++;
        if (s_pdump_left > 0) pass_dump_generation(s_pgen_cur);
    }
}

/* Draw one host OSD ARGB image into the default framebuffer at (vx,vy)
 * in top-left window coordinates (y down). Bitmap is ow×oh; viewport is
 * dw×dh (may upscale for HiDPI / large windows). */
static void gl_draw_osd_image(const uint32_t *px, int ow, int oh,
                              int dw, int dh, int vx, int vy, int ww, int wh) {
    if (!px || ow <= 0 || oh <= 0 || dw <= 0 || dh <= 0 ||
        !s_present_prog || ww <= 0 || wh <= 0)
        return;
    if (!s_osd_tex) glGenTextures(1, &s_osd_tex);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_osd_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (s_osd_tw != ow || s_osd_th != oh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, px);
        s_osd_tw = ow;
        s_osd_th = oh;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh,
                        GL_BGRA, GL_UNSIGNED_BYTE, px);
    }
    if (vx + dw > ww) dw = ww - vx;
    if (vy + dh > wh) dh = wh - vy;
    if (dw < 1 || dh < 1) return;
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    /* host_osd bakes opaque panels (A=0xFF). Do not blend — PSX mode-2
     * REVERSE_SUBTRACT left armed across FMV present made toasts solid black. */
    glDisable(GL_BLEND);
    if (p_glBlendEquationSeparate)
        p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
    /* GL viewport origin is bottom-left. */
    glViewport(vx, wh - vy - dh, dw, dh);
    p_glUseProgram(s_present_prog);
    p_glUniform1i(s_present_uTex, 0);
    present_set_gamma(s_present_uGamma, 0);
    present_set_sharp(0, 0, 0, 0, 0);   /* OSD is authored at output res */
    PRESENT_SCANLINE(0, 0, 0);          /* never scanline the host OSD */
    /* Host OSD bitmaps are top-down (row 0 = top), same as guest CPU
     * present with v_flip=1: uv (0,0)-(1,1). (0,1)-(1,0) was the hold-last
     * cancel for already-oriented captures and made toasts upside-down. */
    p_glUniform4f(s_present_uUvRect, 0.f, 0.f, 1.f, 1.f);
    {
        p_glBindVertexArray(s_present_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glBindVertexArray(0);
    }
    p_glUseProgram(0);
}

/* Composite host toast + volume bar into the default framebuffer, then swap. */
/* ---- presented-image ring feed (debug tools) ------------------------------
 * Every present, before the host OSD is drawn: blit the composed default
 * framebuffer (what the window shows, letterbox included) down to a
 * PRESENT_IMAGE_RING_H-tall thumbnail FBO, and read it back through a pair of
 * pixel-pack buffers so the GPU->CPU copy of frame N completes while frame
 * N+1 is presented (no pipeline stall). Tagged with the frame number at issue
 * time. Absent buffer-mapping entry points disable only this feed. */
#ifndef PSX_NO_DEBUG_TOOLS
#include "present_image_ring.h"
extern uint64_t s_frame_count;
#define PSXGL_PIXEL_PACK_BUFFER 0x88EB
#define PSXGL_STREAM_READ       0x88E1
#define PSXGL_READ_ONLY         0x88B8
static GLuint   s_pir_fbo, s_pir_tex, s_pir_pbo[2];
static int      s_pir_w, s_pir_pending[2], s_pir_pending_w[2], s_pir_idx;
static uint32_t s_pir_frame[2];
static int      s_pir_disabled;

static void present_image_ring_capture_gl(void) {
    if (s_pir_disabled || !p_glMapBuffer || !p_glUnmapBuffer) return;
    if (!present_image_ring_accepting()) return;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    const int tw = present_image_ring_thumb_w(ww, wh);
    const int th = PRESENT_IMAGE_RING_H;
    if (tw <= 0) return;
    if (!s_pir_fbo) {
        glGenTextures(1, &s_pir_tex);
        glBindTexture(GL_TEXTURE_2D, s_pir_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PRESENT_IMAGE_RING_MAX_W, th, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        p_glGenFramebuffers(1, &s_pir_fbo);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_pir_fbo);
        p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, s_pir_tex, 0);
        const GLenum st = p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
        if (st != PSXGL_FRAMEBUFFER_COMPLETE) { s_pir_disabled = 1; return; }
        p_glGenBuffers(2, s_pir_pbo);
        for (int i = 0; i < 2; i++) {
            p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, s_pir_pbo[i]);
            p_glBufferData(PSXGL_PIXEL_PACK_BUFFER,
                           (ptrdiff_t)PRESENT_IMAGE_RING_MAX_W * th * 4, NULL,
                           PSXGL_STREAM_READ);
        }
        p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, 0);
    }
    /* Downscale the presented image (default framebuffer back buffer). */
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_pir_fbo);
    p_glBlitFramebuffer(0, 0, ww, wh, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    /* Queue this frame's readback into the current pack buffer. */
    const int cur = s_pir_idx;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_pir_fbo);
    p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, s_pir_pbo[cur]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, (void *)0);
    s_pir_pending[cur] = 1;
    s_pir_pending_w[cur] = tw;
    s_pir_frame[cur] = (uint32_t)s_frame_count;
    /* Harvest the previous frame's buffer (its copy finished during this one). */
    const int prev = cur ^ 1;
    if (s_pir_pending[prev]) {
        p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, s_pir_pbo[prev]);
        const uint8_t *px = (const uint8_t *)p_glMapBuffer(PSXGL_PIXEL_PACK_BUFFER,
                                                           PSXGL_READ_ONLY);
        if (px) {
            present_image_ring_push(s_pir_frame[prev], px, s_pir_pending_w[prev],
                                    s_pir_pending_w[prev] * 4, 1 /* GL rows bottom-up */);
            p_glUnmapBuffer(PSXGL_PIXEL_PACK_BUFFER);
        }
        s_pir_pending[prev] = 0;
    }
    p_glBindBuffer(PSXGL_PIXEL_PACK_BUFFER, 0);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    s_pir_idx = prev;
    s_pir_w = tw;
}
#endif

/* Host overlays (OSD text, volume bar, rewind strip, savestate menu) are
 * rasterized by the emulation thread. A recorded present carries a copy taken
 * at record time; replay draws that copy (ov_* below), never the live ones. */
static struct {
    int valid, needs_present;
    const uint32_t *px[4];
    int w[4], h[4];
    float slide;
} s_rth_ov;
static int ov_image(int i, const uint32_t **px, int *w, int *h) {
    if (rth_replaying() && s_rth_ov.valid) {
        *px = s_rth_ov.px[i]; *w = s_rth_ov.w[i]; *h = s_rth_ov.h[i];
        return s_rth_ov.px[i] != NULL;
    }
    switch (i) {
    case 0: return host_osd_image(px, w, h);
    case 1: return host_osd_volume_image(px, w, h);
    case 2: return psx_rewind_overlay_image(px, w, h);
    default: return psx_savestate_menu_overlay_image(px, w, h);
    }
}
static float ov_rewind_slide(void) {
    return (rth_replaying() && s_rth_ov.valid) ? s_rth_ov.slide : psx_rewind_slide();
}
static int ov_needs_present(void) {
    return (rth_replaying() && s_rth_ov.valid) ? s_rth_ov.needs_present
                                               : host_osd_needs_present();
}

static uint64_t s_swaps_total = 0;   /* every swap, real or generated (diagnostic) */

/* ---- Present thread ([video] present_thread) -------------------------------
 * Opt-in under the render thread. Each composed frame (everything that used
 * to land in the window's back buffer: the display quad, OSD, captures) goes
 * to an offscreen slot of the window's size; the swap becomes "fence, queue
 * the slot, take the next free one". The present thread owns a second context
 * on the same window, sharing objects with s_ctx, and does the 1:1 copy and
 * the real swap, so the compositor's wait no longer blocks rendering. The
 * composing side waits only while every slot is queued or on screen. */
#define PT_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define PT_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
static int           s_pt_want = 0, s_pt_slots = 3;
static GLenum        s_pt_format = GL_RGBA8;   /* the window's: no alpha -> GL_RGB8 */
static SDL_GLContext s_pt_ctx = NULL;
static GLuint        s_pts_tex[PT_MAX_SLOTS], s_pts_rb[PT_MAX_SLOTS], s_pts_fbo[PT_MAX_SLOTS];
static int           s_pts_w[PT_MAX_SLOTS], s_pts_h[PT_MAX_SLOTS];
static unsigned      s_pts_gen[PT_MAX_SLOTS];
/* present thread's own objects (FBOs are per-context) */
static GLuint        s_ptp_fbo[PT_MAX_SLOTS];
static unsigned      s_ptp_gen[PT_MAX_SLOTS];
static int           s_ptp_interval_gen = -1;

void gl_renderer_set_present_thread(int on, int slots) {
    s_pt_want = on ? 1 : 0;
    s_pt_slots = slots < 2 ? 2 : slots > PT_MAX_SLOTS ? PT_MAX_SLOTS : slots;
}
int gl_renderer_present_thread_active(void) { return s_pt_on; }

/* Composing context: the current slot, (re)allocated at the window's size. */
static GLuint pt_target_fbo(void) {
    const int i = pt_current();
    if (i < 0) return 0;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;
    if (s_pts_fbo[i] && s_pts_w[i] == ww && s_pts_h[i] == wh) return s_pts_fbo[i];
    GLint tex_prev = 0, rb_prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex_prev);
    glGetIntegerv(0x8CA7 /* GL_RENDERBUFFER_BINDING */, &rb_prev);
    if (s_pts_fbo[i]) {
        p_glDeleteFramebuffers(1, &s_pts_fbo[i]);
        p_glDeleteRenderbuffers(1, &s_pts_rb[i]);
        glDeleteTextures(1, &s_pts_tex[i]);
        s_pts_fbo[i] = s_pts_rb[i] = s_pts_tex[i] = 0;
    }
    s_pts_tex[i] = make_tex(s_pt_format, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE);
    p_glGenRenderbuffers(1, &s_pts_rb[i]);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_pts_rb[i]);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, ww, wh);
    p_glGenFramebuffers(1, &s_pts_fbo[i]);
    p_glBindFramebuffer_raw(PSXGL_FRAMEBUFFER, s_pts_fbo[i]);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_pts_tex[i], 0);
    p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                PSXGL_RENDERBUFFER, s_pts_rb[i]);
    if (p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER) != PSXGL_FRAMEBUFFER_COMPLETE)
        fprintf(stdout, "psxrecomp: present thread: slot FBO incomplete\n");
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex_prev);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER, (GLuint)rb_prev);
    s_pts_w[i] = ww; s_pts_h[i] = wh;
    s_pts_gen[i]++;
    return s_pts_fbo[i];
}

static int pt_cb_ctx(void *user, int current) {
    (void)user;
    if (current) {
        if (SDL_GL_MakeCurrent(s_win, s_pt_ctx) != 0) return 0;
        s_ptp_interval_gen = -1;
        return 1;
    }
    for (int i = 0; i < PT_MAX_SLOTS; i++) {
        if (s_ptp_fbo[i]) p_glDeleteFramebuffers(1, &s_ptp_fbo[i]);
        s_ptp_fbo[i] = 0;
        s_ptp_gen[i] = 0;
    }
    glFinish();
    SDL_GL_MakeCurrent(s_win, NULL);
    return 1;
}

/* The one owned swap call: the composing thread's (direct) or the present
 * thread's. */
static void gl_window_swap(void) { SDL_GL_SwapWindow(s_win); }

static void *pt_cb_present(void *user, int slot, void *ready) {
    (void)user;
    if (s_ptp_interval_gen != s_pt_interval_gen) {
        s_ptp_interval_gen = s_pt_interval_gen;
        if (SDL_GL_SetSwapInterval(s_swap_interval) != 0 && s_swap_interval < 0)
            SDL_GL_SetSwapInterval(1);
    }
    if (ready) { p_glWaitSync(ready, 0, PT_TIMEOUT_IGNORED); p_glDeleteSync(ready); }
    if (!s_ptp_fbo[slot] || s_ptp_gen[slot] != s_pts_gen[slot]) {
        if (!s_ptp_fbo[slot]) p_glGenFramebuffers(1, &s_ptp_fbo[slot]);
        p_glBindFramebuffer_raw(PSXGL_READ_FRAMEBUFFER, s_ptp_fbo[slot]);
        p_glFramebufferTexture2D(PSXGL_READ_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                 s_pts_tex[slot], 0);
        s_ptp_gen[slot] = s_pts_gen[slot];
    }
    const int sw = s_pts_w[slot], sh = s_pts_h[slot];
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    p_glBindFramebuffer_raw(PSXGL_READ_FRAMEBUFFER, s_ptp_fbo[slot]);
    p_glBindFramebuffer_raw(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    if (ww == sw && wh == sh) {
        p_glBlitFramebuffer(0, 0, sw, sh, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        /* The window changed after this frame was composed: fit it. */
        glViewport(0, 0, ww, wh);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        p_glBlitFramebuffer(0, 0, sw, sh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
#ifdef GL_PRESENT_THREAD_TEST_HOOK
    GL_PRESENT_THREAD_TEST_HOOK(slot);
#endif
    void *done = p_glFenceSync(PT_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl_window_swap();
    return done;
}

static void pt_cb_dispose(void *user, void *token) {
    (void)user;
    if (token) p_glDeleteSync(token);
}

/* Composing context current; 1 when the present thread runs. */
static int pt_begin(void) {
    if (!s_pt_want || s_pt_on || !s_win || !s_ctx) return 0;
    if (!p_glFenceSync || !p_glWaitSync || !p_glDeleteSync) {
        fprintf(stdout, "psxrecomp: present thread: no sync objects; swaps stay on the render thread\n");
        return 0;
    }
    /* The slots stand in for the window's back buffer: same channels, so
     * destination alpha and readbacks behave as they did there. */
    {
        GLint a = 8;
        typedef void (APIENTRY *PFN_gfap)(GLenum, GLenum, GLenum, GLint *);
        PFN_gfap gfap = (PFN_gfap)SDL_GL_GetProcAddress("glGetFramebufferAttachmentParameteriv");
        p_glBindFramebuffer_raw(PSXGL_FRAMEBUFFER, 0);
        while (glGetError() != GL_NO_ERROR) {}
        if (gfap) gfap(PSXGL_FRAMEBUFFER, GL_BACK_LEFT, 0x8215 /* ALPHA_SIZE */, &a);
        if (glGetError() != GL_NO_ERROR) a = 8;
        s_pt_format = a > 0 ? GL_RGBA8 : GL_RGB8;
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    s_pt_ctx = SDL_GL_CreateContext(s_win);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!s_pt_ctx) {
        fprintf(stdout, "psxrecomp: present thread: shared context failed (%s); swaps stay on "
                "the render thread\n", SDL_GetError());
        SDL_GL_MakeCurrent(s_win, s_ctx);
        return 0;
    }
    SDL_GL_MakeCurrent(s_win, NULL);   /* creation made it current here */
    SDL_GL_MakeCurrent(s_win, s_ctx);
    memset(s_pts_fbo, 0, sizeof s_pts_fbo);
    memset(s_ptp_fbo, 0, sizeof s_ptp_fbo);
    PtConfig c = { s_pt_slots, pt_cb_ctx, pt_cb_present, pt_cb_dispose, NULL };
    if (!pt_start(&c)) {
        fprintf(stdout, "psxrecomp: present thread: start failed; swaps stay on the render thread\n");
        SDL_GL_DeleteContext(s_pt_ctx);
        s_pt_ctx = NULL;
        SDL_GL_MakeCurrent(s_win, s_ctx);
        return 0;
    }
    s_pt_on = 1;
    fprintf(stdout, "psxrecomp: present thread on (%d slots)\n", s_pt_slots);
    return 1;
}

/* Composing context current, render thread stopped. */
static void pt_end(void) {
    if (!s_pt_on) return;
    glFlush();
    pt_stop();
    s_pt_on = 0;
    for (int i = 0; i < PT_MAX_SLOTS; i++) {
        if (s_pts_fbo[i]) {
            p_glDeleteFramebuffers(1, &s_pts_fbo[i]);
            p_glDeleteRenderbuffers(1, &s_pts_rb[i]);
            glDeleteTextures(1, &s_pts_tex[i]);
        }
        s_pts_fbo[i] = s_pts_rb[i] = s_pts_tex[i] = 0;
        s_pts_w[i] = s_pts_h[i] = 0;
    }
    SDL_GL_DeleteContext(s_pt_ctx);
    s_pt_ctx = NULL;
    SDL_GL_MakeCurrent(s_win, s_ctx);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* The swap: direct, or queued to the present thread. */
static void gl_present_swap(void) {
    if (!s_pt_on) { gl_window_swap(); return; }
    (void)pt_target_fbo();   /* a frame that never bound 0 still has its slot */
    void *ready = p_glFenceSync(PT_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    void *done = NULL;
    (void)pt_submit(ready, &done, NULL);
    if (done) { p_glWaitSync(done, 0, PT_TIMEOUT_IGNORED); p_glDeleteSync(done); }
    /* Whatever is bound as the window now means the new slot. */
    GLint d = 0, r = 0;
    glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &d);
    glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &r);
    for (int i = 0; i < PT_MAX_SLOTS; i++) {
        if (s_pts_fbo[i] && (GLuint)d == s_pts_fbo[i]) p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
        if (s_pts_fbo[i] && (GLuint)r == s_pts_fbo[i]) p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    }
}

int gl_renderer_present_thread_json(char *out, size_t cap) {
    PtStats st;
    pt_get_stats(&st);
    return snprintf(out, cap,
        "\"present_thread\":{\"active\":%d,\"slots\":%d,\"submits\":%llu,\"presents\":%llu,"
        "\"waits\":%llu,\"wait_ms\":%.3f,\"swap_ms_avg\":%.3f,\"swap_ms_max\":%.3f,"
        "\"queued_high\":%d}",
        s_pt_on, st.slots, (unsigned long long)st.submits, (unsigned long long)st.presents,
        (unsigned long long)st.waits, st.wait_ns / 1e6,
        st.presents ? (double)st.present_ns / (double)st.presents / 1e6 : 0.0,
        st.present_max_ns / 1e6, st.queued_high);
}
static void gl_swap_now(int generated);
static int  fg_keep_real(void);      /* frame generation */
static void fg_note_swap(uint64_t ns);
static void gl_swap_with_osd(void) {
    openxr_present_native(); /* Copy guest content before host-only overlays. */
    s_native_surface_pending=0; /* Hold-last/resim cannot reuse a native source. */
#ifndef PSX_NO_DEBUG_TOOLS
    if (s_ctx) present_image_ring_capture_gl();
#endif
    if (s_present_prog && s_ctx) {
        int ww = 0, wh = 0;
        SDL_GL_GetDrawableSize(s_win, &ww, &wh);
        if (ww > 0 && wh > 0) {
            const uint32_t *px = NULL;
            int ow = 0, oh = 0;
            /* OSD authored for ~480-tall present; grow with drawable height. */
            int ui = wh / 480;
            int margin;
            if (ui < 1) ui = 1;
            if (ui > 8) ui = 8;
            margin = 8 * ui;
            if (ov_image(0, &px, &ow, &oh) && px)
                gl_draw_osd_image(px, ow, oh, ow * ui, oh * ui,
                                  margin, margin, ww, wh);
            if (ov_image(1, &px, &ow, &oh) && px) {
                const int dw = ow * ui, dh = oh * ui;
                int vx = (ww > dw + margin) ? (ww - dw - margin) : margin;
                int vy = (wh > dh) ? ((wh - dh) / 2) : margin;
                gl_draw_osd_image(px, ow, oh, dw, dh, vx, vy, ww, wh);
            }
            if (ov_image(2, &px, &ow, &oh) && px) {
                float slide = ov_rewind_slide();
                int dw = ww;
                int dh = (wh * oh) / 480;
                int vy;
                if (dh < 8) dh = oh;
                vy = wh - (int)((float)dh * slide + 0.5f);
                gl_draw_osd_image(px, ow, oh, dw, dh, 0, vy, ww, wh);
            }
            if (ov_image(3, &px, &ow, &oh) && px)
                gl_draw_osd_image(px, ow, oh, ww, wh, 0, 0, ww, wh);
        }
    }
    if (!rth_replaying()) host_osd_present_done();   /* replay: done at record */
    /* present_shot (GL backend): the default framebuffer now holds the composed
     * frame — display quad fitted to the window, plus OSD — so this is the only
     * capture that carries the presented aspect. Buffer-level captures resolve
     * before the fit and answer the raw display size instead. Read back before
     * the swap; GL rows come out bottom-up, so flip into the PNG. */
    {
        extern int  present_shot_take(char *out, int n);
        extern void present_shot_done(int ok);
        char shot_path[512];
        /* PSX_PRESENT_SHOT_GENERATED=1 (diagnostic): a staged present_shot
         * waits for the next generated frame (frame generation). */
        static int gen_only = -1;
        if (gen_only < 0) {
            const char *e = getenv("PSX_PRESENT_SHOT_GENERATED");
            gen_only = (e && *e && *e != '0') ? 1 : 0;
        }
        /* PSX_PRESENT_SHOT_BURST=N (diagnostic): a staged present_shot takes
         * the next N composed frames, kept in memory and written when the
         * burst ends as <path>_NN_r.png (real) / _NN_g.png (generated), in
         * composition order: with frame generation a real frame is composed
         * before the in-between frames that lead up to it. */
        static int burst = -1, b_i = 0, b_w = 0, b_h = 0;
        static uint8_t **b_px = NULL; static char *b_kind = NULL; static uint64_t b_flip[240];
        static char b_path[512];
        if (burst < 0) {
            const char *e = getenv("PSX_PRESENT_SHOT_BURST");
            burst = e ? atoi(e) : 0;
            if (burst < 0) burst = 0;
            if (burst > 240) burst = 240;
            if (burst) {
                b_px = (uint8_t **)calloc((size_t)burst, sizeof *b_px);
                b_kind = (char *)calloc((size_t)burst, 1);
                if (!b_px || !b_kind) burst = 0;
            }
        }
        if (burst && b_i > 0) {
            int ww = 0, wh = 0;
            SDL_GL_GetDrawableSize(s_win, &ww, &wh);
            if (ww == b_w && wh == b_h && (b_px[b_i] = (uint8_t *)malloc((size_t)ww * wh * 3))) {
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, ww, wh, GL_RGB, GL_UNSIGNED_BYTE, b_px[b_i]);
                b_kind[b_i] = s_fg_presenting ? 'g' : 'r';
                b_flip[b_i] = s_fg_flips;
                b_i++;
            } else {
                b_i = burst;   /* resized: end it */
            }
            if (b_i >= burst) {
                int ok = 1;
                uint8_t *flip = (uint8_t *)malloc((size_t)b_w * b_h * 3);
                for (int i = 0; i < burst; i++) {
                    if (!b_px[i] || !flip) { ok = 0; continue; }
                    for (int y = 0; y < b_h; y++)
                        memcpy(flip + (size_t)y * b_w * 3, b_px[i] + (size_t)(b_h - 1 - y) * b_w * 3,
                               (size_t)b_w * 3);
                    char fn[600];
                    snprintf(fn, sizeof fn, "%s_%02d_%c_f%llu.png", b_path, i, b_kind[i],
                             (unsigned long long)b_flip[i]);
                    FILE *pf = fopen(fn, "wb");
                    if (!pf || !png_write_rgb(pf, flip, (uint32_t)b_w, (uint32_t)b_h)) ok = 0;
                    if (pf) fclose(pf);
                    free(b_px[i]); b_px[i] = NULL;
                }
                free(flip);
                b_i = 0;
                present_shot_done(ok);
            }
        } else if (burst && present_shot_take(shot_path, (int)sizeof(shot_path))) {
            SDL_GL_GetDrawableSize(s_win, &b_w, &b_h);
            snprintf(b_path, sizeof b_path, "%s", shot_path);
            if (b_w > 0 && b_h > 0 && (b_px[0] = (uint8_t *)malloc((size_t)b_w * b_h * 3))) {
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, b_w, b_h, GL_RGB, GL_UNSIGNED_BYTE, b_px[0]);
                b_kind[0] = s_fg_presenting ? 'g' : 'r';
                b_flip[0] = s_fg_flips;
                b_i = 1;
            } else {
                present_shot_done(0);
            }
        } else if (!burst && (!gen_only || s_fg_presenting) &&
            present_shot_take(shot_path, (int)sizeof(shot_path))) {
            int ww = 0, wh = 0;
            int wrote = 0;
            SDL_GL_GetDrawableSize(s_win, &ww, &wh);
            if (ww > 0 && wh > 0) {
                uint8_t *rows = (uint8_t *)malloc((size_t)ww * wh * 3);
                uint8_t *flip = rows ? (uint8_t *)malloc((size_t)ww * wh * 3) : NULL;
                if (rows && flip) {
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, ww, wh, GL_RGB, GL_UNSIGNED_BYTE, rows);
                    for (int y = 0; y < wh; y++)
                        memcpy(flip + (size_t)y * ww * 3,
                               rows + (size_t)(wh - 1 - y) * ww * 3,
                               (size_t)ww * 3);
                    FILE *pf = fopen(shot_path, "wb");
                    if (pf) {
                        wrote = png_write_rgb(pf, flip, (uint32_t)ww, (uint32_t)wh);
                        fclose(pf);
                    }
                }
                /* free() tolerates NULL, so both exits are covered. */
                free(flip);
                free(rows);
            }
            present_shot_done(wrote);
        }
    }
    /* Frame generation: a real frame shown after its in-between frames is
     * composed now (its buffer may be drawn over before it is shown) and
     * kept; fg_present_real swaps it later. */
    if (s_fg_capture_real && fg_keep_real()) return;
    gl_swap_now(s_fg_presenting);
}

/* The swap itself (and its accounting). */
static void gl_swap_now(int generated) {
    (void)generated;
    /* Dynamic resolution's ledger: time blocked in the swap is the driver's
     * vsync wait unless the frame was late (main.cpp). Keep one owned swap. */
#ifdef GL_PRESENT_TEST_HOOK
    GL_PRESENT_TEST_HOOK(generated);
#endif
    s_swaps_total++;
    uint64_t t0 = s_dyn_on ? SDL_GetPerformanceCounter() : 0;
    const uint64_t rthf_t0 = rth_replaying() ? host_now_ns_rthf() : 0;
    gl_present_swap();
    if (rthf_t0) {
        const uint64_t sw = host_now_ns_rthf() - rthf_t0;
        s_rthf_swap_ns += sw;
        fg_note_swap(sw);
    }
    if (s_dyn_on) {
        s_dyn_last_swap_ticks = SDL_GetPerformanceCounter() - t0;
        s_dyn_ledger.swap_ticks += s_dyn_last_swap_ticks;
        s_dyn_ledger.swaps++;
    }
}

/* Output pixels per source texel below which a supersampled present switches
 * from one tap to the area resolve (1 / 1.25: the source is 25% larger than the
 * output). Only used for S > 1 sources, so native presents never change. */
#define PRESENT_AREA_MIN_RATIO 1.25f

/* tex_w/tex_h and x/y/w/h are in the texture's NATIVE units; src_scale is how
 * many texels one native unit spans (the internal scale for the hr FBO and the
 * wide surfaces, 1 for textures that hold native or already-composed pixels).
 * src_scale <= 1 is exactly the historical behaviour. tex_w/tex_h are the
 * texture's extent at src_scale, which is fractional only for a surface
 * allocated above the scale it renders at (dynamic resolution,
 * present_alloc_extent). */
static void present_target_quad(GLuint tex, float tex_w, float tex_h,
                                int x, int y, int w, int h, int linear,
                                int lx, int ly, int lw, int lh, int v_flip,
                                int apply_gamma, int src_scale) {
    float u0, v0, u1, v1;
    int area = 0;
    if (src_scale > 1 && lw > 0 && lh > 0 && w > 0 && h > 0) {
        float rx = (float)(w * src_scale) / (float)lw;
        float ry = (float)(h * src_scale) / (float)lh;
        area = (rx > PRESENT_AREA_MIN_RATIO || ry > PRESENT_AREA_MIN_RATIO);
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(lx, ly, lw, lh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* The area resolve averages bilinear taps; it needs LINEAR whatever the
     * AA setting (the source is already the supersampled image). */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (linear || area) ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (linear || area) ? GL_LINEAR : GL_NEAREST);
    p_glUseProgram(s_present_prog);
    p_glUniform1i(s_present_uTex, 0);
    present_set_gamma(s_present_uGamma, apply_gamma);
    /* The rasterized path already renders at the internal scale, so it has no
     * low-res source to reconstruct — keep the plain sample, unless the
     * internal image is larger than the output: then resolve its area. */
    if (area && s_present_uSharp >= 0) {
        p_glUniform1i(s_present_uSharp, 3);
        if (s_present_uTexSize >= 0)
            p_glUniform2f(s_present_uTexSize, tex_w * (float)src_scale,
                          tex_h * (float)src_scale);
        if (s_present_uSharpScale >= 0)   /* output px per source texel */
            p_glUniform2f(s_present_uSharpScale,
                          (float)lw / (float)(w * src_scale),
                          (float)lh / (float)(h * src_scale));
    } else {
        present_set_sharp(0, 0, 0, 0, 0);
    }
    /* Scanline at the native line grid. v_uv is normalized against tex_h (the
     * full VRAM/FBO texture), and one texel row is one PS1 scanline, so tex_h is
     * the phase pitch; h is the displayed line count for the output-scale gate.
     * v_flip=0 is the already-composed hold-last drawable (scanlines, if any,
     * are already baked) — skip it. */
    PRESENT_SCANLINE(v_flip ? tex_h : 0.0f, v_flip ? h : 0, lh);
    /* Half-texel inset: with GL_LINEAR, corner-mapped UVs make the outermost
     * dest pixels blend the border texel with VRAM outside the content rect
     * (visible edge stripe with AA on). Center-mapped UVs keep edge samples
     * inside the rect; interior sampling is unchanged. */
    /* The inset is half a TEXEL. For a supersampled source that is 0.5/S of a
     * native unit; the old 0.5 native units cropped (S-1)/2 texels per edge
     * (about 4 px at 9x) and zoomed the image. src_scale <= 1: unchanged. */
    {
        float in = src_scale > 1 ? 0.5f / (float)src_scale : 0.5f;
        u0 = ((float)x + in) / tex_w;
        v0 = ((float)y + in) / tex_h;
        u1 = ((float)(x + w) - in) / tex_w;
        v1 = ((float)(y + h) - in) / tex_h;
    }
    /* PRESENT_VS always samples with mix(v0,v1,1-p.y). For CPU/FBO guest
     * bands that is the correct PSX top-down → GL mapping (v_flip=1). For a
     * glCopyTexSubImage2D of the already-presented drawable, the texture
     * already matches screen orientation — swapping v ends cancels the
     * shader flip so hold-last is not upside-down for one frame. */
    if (!v_flip) {
        float t = v0;
        v0 = v1;
        v1 = t;
    }
    p_glUniform4f(s_present_uUvRect, u0, v0, u1, v1);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
}

/* Bezel art: a still image behind the frame, filling whatever the letterbox or
 * pillarbox leaves over. It never samples the frame, so unlike an edge-stretch
 * backdrop it is independent of what the game is currently drawing. */
static GLuint s_bezel_tex = 0;

int gl_renderer_set_bezel(const void *rgba, int w, int h) {
    GL_RT_SYNC("set_bezel");
    if (s_bezel_tex) { glDeleteTextures(1, &s_bezel_tex); s_bezel_tex = 0; }
    if (!rgba || w <= 0 || h <= 0) return 1;
    if (!s_ctx || !s_raster_ok) return 0;
    glGenTextures(1, &s_bezel_tex);
    if (!s_bezel_tex) return 0;
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_bezel_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return 1;
}

int gl_renderer_has_bezel(void) { return s_bezel_tex != 0; }

/* Cover the drawable with the bezel before the game quad. */
static void present_bezel(int ww, int wh, int lx, int ly, int lw, int lh) {
    if (!s_bezel_tex || ww <= 0 || wh <= 0) return;
    if (lx <= 0 && ly <= 0 && lw >= ww && lh >= wh) return;
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, ww, wh);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_bezel_tex);
    p_glUseProgram(s_present_prog);
    p_glUniform1i(s_present_uTex, 0);
    present_set_gamma(s_present_uGamma, 0);
    p_glUniform4f(s_present_uUvRect, 0.0f, 0.0f, 1.0f, 1.0f);
    PRESENT_SCANLINE(0, 0, 0);          /* bezel art is not scanlined */
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
}

int gl_renderer_present_hold_last(void) {
    GL_RT_SYNC("present_hold_last");
    int ww = 0, wh = 0;
    int lx, ly, lw, lh;
    if (!s_ctx || !s_win || s_hold_kind == HOLD_NONE || !s_hold_tex)
        return 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww < 1 || wh < 1)
        return 0;
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s_hold_kind == HOLD_DRAWABLE) {
        /* Captured image already includes letterbox bars. Stretch 1:1, or
         * letterbox to preserve aspect if the drawable resized mid-resim. */
        lx = 0;
        ly = 0;
        lw = ww;
        lh = wh;
        if (s_hold_tw > 0 && s_hold_th > 0 &&
            ((int64_t)s_hold_tw * wh != (int64_t)s_hold_th * ww)) {
            if ((int64_t)ww * s_hold_th > (int64_t)wh * s_hold_tw) {
                lw = (int)(((int64_t)wh * s_hold_tw) / s_hold_th);
                if (lw < 1) lw = 1;
                lx = (ww - lw) / 2;
                lh = wh;
                ly = 0;
            } else {
                lh = (int)(((int64_t)ww * s_hold_th) / s_hold_tw);
                if (lh < 1) lh = 1;
                ly = (wh - lh) / 2;
                lw = ww;
                lx = 0;
            }
        }
        /* v_flip=0: drawable capture is already screen-oriented. */
        present_target_quad(s_hold_tex, s_hold_tw, s_hold_th,
                            0, 0, s_hold_tw, s_hold_th, 0, lx, ly, lw, lh, 0, 0, 1);
    } else {
        if (s_hold_force_4_3)
            letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
        else
            letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
        present_target_quad(s_hold_tex, s_hold_tw, s_hold_th,
                            0, 0, s_hold_tw, s_hold_th, s_hold_linear,
                            lx, ly, lw, lh, 1, 1, 1);
        /* Upgrade to drawable after letterboxing once. Live+interp leaves
         * HOLD_NATIVE (no Swap on main); drawable path is the soak-proven
         * full-window image (GL without interp). */
        hold_capture_drawable();
    }
    gpu_timeline_note(GTL_PRESENT, GTL_PATH_HOLD, 0);
    latency_ring_mark(LAT_SWAP_BEGIN);
    gl_swap_with_osd();
    latency_ring_mark(LAT_SWAP_END);
    s_probe_swap++;
    return 1;
}

/* The extent, in native units at `scale`, of a surface allocated at
 * alloc_scale_for(scale) that holds `units` native columns or rows: units
 * itself unless dynamic resolution renders it below its allocation. */
static float present_alloc_extent(int units, int scale) {
    const int as = s_hr_alloc > scale ? s_hr_alloc : scale;
    if (as <= scale || scale < 1) return (float)units;
    return (float)units * (float)as / (float)scale;
}

static void present_vram_impl(int disp_x, int disp_y, int w, int h, int linear,
                              int force_4_3) {
    /* The crop a pass applies (transaction_begin), taken on the emulation
     * thread at the call: a render-thread replay of this present keeps it. */
    if (!rt_on_render_thread()) {
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        s_present_crop_dx = disp_x - (int)di.display_x;
        s_present_crop_dy = disp_y - (int)di.display_y;
        s_present_crop_w = w;
        s_present_crop_h = h;
    }
    if (rth_record_mode()) {
        const int32_t a[6] = { disp_x, disp_y, w, h, linear, force_4_3 };
        if (rth_record_present(RTH_PRESENT_VRAM, 6, a)) return;
        gl_rth_acquire("present_oversize");
    }
    if (!s_ctx || !s_raster_ok) return;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    if (stereo_present(w, h)) return;
    if (!s_native_surface_enabled && s_force_present_remaining <= 0 &&
        s_last_present_path == GL_PRES_VRAM &&
        s_last_dx == disp_x && s_last_dy == disp_y &&
        s_last_dw == w && s_last_dh == h &&
        !present_dirty_test(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1) &&
        !ov_needs_present() &&
        !psx_present_vsync_owns_cadence() &&
        !gl_renderer_interpolation_owns_cadence()) {
        s_probe_skip++;
        gpu_timeline_note(GTL_PRESENT_SKIP, GTL_PATH_VRAM_FBO,
                          (uint32_t)disp_x | ((uint32_t)disp_y << 16));
        gl_perf_present_enter();
        gl_perf_present_exit(0);
        return;
    }
    gl_perf_present_enter();   /* per-frame backdrop-phase reset + dbg snapshot live in here */
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    if (force_4_3)
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    else
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    /* No short-band adjustment on the 15-bit FBO path: a game's native short
     * display mode (e.g. 216/224-line menus) must fill the rect as before.
     * The FMV band fix lives in the depth24/CPU present paths only. */

    /* Present source: the hr surface, or in windowed high-resolution mode the
     * window holding this display (grown to it on first sight). A display the
     * window cannot hold presents from the 1x surface without temporal
     * blending (whose captures assume the S-scaled source). */
    GLuint src_fbo = s_hr_fbo, src_tex = s_hr_tex, interp_fbo = s_hr_fbo;
    int src_tw = VRAM_W, src_x = disp_x, src_scale = s_out_scale;
    if (s_hiw) {
        const HiwTile *T = hiw_ensure(disp_x, disp_x + w);
        s_hiw_last_x0 = disp_x; s_hiw_last_x1 = disp_x + w;
        if (T) {
            src_fbo = interp_fbo = T->fbo; src_tex = T->tex;
            src_tw = T->x1 - T->x0; src_x = disp_x - T->x0;
        } else {
            src_scale = s_hr_scale;
            interp_fbo = 0;
        }
    }

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f,0.f,0.f,1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    int interp_pair = interp_capture(
        interp_fbo, src_x, disp_y, w, h, linear, force_4_3, GL_PRES_VRAM,
        disp_x, disp_y,
        present_dirty_test(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1));
    if (interp_pair) {
        /* Temporal blending owns this stock frame interval. Capture hold-last
         * before pacing; every blend and Swap remains on this context/thread. */
        hold_capture_native_fbo(src_fbo, src_x, disp_y, w, h, force_4_3, linear);
        interp_present_source_interval();
        gl_perf_present_exit(0);
        present_dirty_rect(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1, 0);
        present_force_consumed();
        s_last_present_path = GL_PRES_VRAM;
        s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = w; s_last_dh = h;
        return;
    }
    present_bezel(ww, wh, lx, ly, lw, lh);
    {
        /* The hr surface may be allocated above the level it renders at
         * (dynamic resolution); window tiles never are. */
        float tw = (float)src_tw, th = (float)VRAM_H;
        if (src_tex == s_hr_tex) {
            tw = present_alloc_extent(src_tw, s_hr_scale);
            th = present_alloc_extent(VRAM_H, s_hr_scale);
        }
        present_target_quad(src_tex, tw, th,
                            src_x, disp_y, w, h, linear, lx, ly, lw, lh, 1, 1, src_scale);
    }
    pres_record(GL_PRES_VRAM, disp_x, disp_y, w, h, lx, ly, lw, lh);
    gpu_timeline_note(GTL_PRESENT, GTL_PATH_VRAM_FBO,
                      (uint32_t)disp_x | ((uint32_t)disp_y << 16));
    hold_capture_drawable();
    latency_ring_mark(LAT_SWAP_BEGIN);
    gl_swap_with_osd();
    latency_ring_mark(LAT_SWAP_END);
    s_probe_swap++;
    gl_perf_present_exit(0);
    present_dirty_rect(disp_x, disp_y, disp_x + w - 1, disp_y + h - 1, 0);
    present_force_consumed();
    s_last_present_path = GL_PRES_VRAM;
    s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = w; s_last_dh = h;
    coh_record(GL_COH_PRESENT, disp_x, disp_y, disp_x + w - 1, disp_y + h - 1);
}

/* Native-wide fast path: authoritatively copy the canonical 4:3 framebuffer
 * into the wide surface's CENTRE columns [g_wide_off, g_wide_off+native_w) for
 * the displayed Y band, so the per-prim mirror could skip every centre-only
 * prim (the dominant native-wide GPU saving). The reveal margins were already
 * produced by the mirror; this leaves them untouched. One FBO->FBO blit,
 * x-translated by the reveal offset. No-op when s_wide_fast is off (then the
 * mirror drew the full surface, as before). Shared by both present paths. */
static void wide_blit_center(GLuint wide_fbo, int base_x, int disp_y, int disp_h) {
    if (!wide_center_is_canonical() || g_wide_w <= 0) return;
    int native_w = g_wide_w - 2 * g_wide_off;
    if (native_w <= 0) return;
    int S = s_out_scale;
    (void)disp_y; (void)disp_h;
    /* Colour only: the centre's stencil is left behind (see s_wst_*). */
    wst_add(wide_index(wide_fbo), g_wide_off, 0, g_wide_off + native_w, VRAM_H);
    /* Copy the canonical framebuffer column into the wide surface CENTRE over the
     * FULL VRAM height, not just the current display band [disp_y, disp_y+disp_h].
     * The wide surface holds BOTH vertical double-buffer bands (Ape flips
     * display_y 0<->256), and the game's draw area / display band can differ per
     * scene (the cityscape intro exposed rows outside disp_h). Copying the whole
     * column makes the wide CENTRE bit-identical to what the full mirror would
     * have drawn there for every band, so nothing the present reads is ever left
     * black. The margins (x outside the centre) are untouched. */
    if (s_hiw) {
        /* Windowed high-resolution mode: the centre comes from a tile at S
         * (grown to it on first sight). Columns no tile holds are the 1x
         * surface, upscaled; tiles overwrite the columns they hold. */
        int cx0 = base_x, cx1 = base_x + native_w;
        const HiwTile *in = hiw_ensure(cx0, cx1);
        int R = s_hr_scale;
        glDisable(GL_SCISSOR_TEST);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, wide_fbo);
        if (!in) {
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
            p_glBlitFramebuffer(cx0 * R, 0, cx1 * R, VRAM_H * R,
                                g_wide_off * S, 0, (g_wide_off + native_w) * S, VRAM_H * S,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        for (int t = 0; t < s_hiw_n; t++) {
            const HiwTile *T = in ? in : &s_hiw_t[t];
            int h0 = cx0 > T->x0 ? cx0 : T->x0, h1 = cx1 < T->x1 ? cx1 : T->x1;
            if (h1 > h0) {
                p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, T->fbo);
                p_glBlitFramebuffer((h0 - T->x0) * S, 0, (h1 - T->x0) * S, VRAM_H * S,
                                    (g_wide_off + h0 - cx0) * S, 0,
                                    (g_wide_off + h1 - cx0) * S, VRAM_H * S,
                                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            if (in) break;
        }
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
        return;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, wide_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(base_x * S, 0,
                        (base_x + native_w) * S, VRAM_H * S,
                        g_wide_off * S, 0,
                        (g_wide_off + native_w) * S, VRAM_H * S,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}

/* GPU-direct native-wide present: blit the displayed buffer's wide FBO straight
 * to the window (no glReadPixels / glFinish CPU round-trip). The wide surface is
 * g_wide_w wide × VRAM_H tall (at scale S); present its [0,g_wide_w] × [disp_y,
 * disp_y+disp_h] region into the letterbox, V-flipped like present_vram (FBO y
 * bottom-origin → window top). Returns 0 if there's no wide surface for base_x
 * (caller falls back). disp_x is the displayed buffer base (the wide-surface key). */
static int present_wide_fbo_impl(int disp_x, int disp_y, int disp_h, int linear) {
    if (rth_record_mode()) {
        /* The caller falls back to the CPU path on 0: answer from the mirror. */
        if (!s_ctx || !s_raster_ok || !rth_mirror_wide_present_ok(disp_x)) return 0;
        const int32_t a[4] = { disp_x, disp_y, disp_h, linear };
        if (rth_record_present(RTH_PRESENT_WIDE, 4, a)) return 1;
        gl_rth_acquire("present_oversize");
    }
    if (!s_ctx || !s_raster_ok || g_wide_w <= 0) return 0;
    GLuint fbo = 0, tex = 0;
    int wide_as = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_wide_fbo[i] && s_wide_base[i] == disp_x) {
            fbo = s_wide_fbo[i]; tex = s_wide_tex[i]; wide_as = s_wide_as[i];
            if (s_alloc_scale) {   /* the rows a scale step rescales */
                int y0 = disp_y < 0 ? 0 : disp_y;
                int y1 = disp_y + disp_h > VRAM_H ? VRAM_H : disp_y + disp_h;
                if (s_dyn_wide_y1[i] <= s_dyn_wide_y0[i]) {
                    s_dyn_wide_y0[i] = y0; s_dyn_wide_y1[i] = y1;
                } else {
                    if (y0 < s_dyn_wide_y0[i]) s_dyn_wide_y0[i] = y0;
                    if (y1 > s_dyn_wide_y1[i]) s_dyn_wide_y1[i] = y1;
                }
            }
            break;
        }
    if (!fbo) return 0;
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    hiw_flush_queue();   /* windowed: queued wide mirrors */
    if (!s_native_surface_enabled && s_force_present_remaining <= 0 &&
        s_last_present_path == GL_PRES_WIDE &&
        s_last_dx == disp_x && s_last_dy == disp_y &&
        s_last_dw == g_wide_w && s_last_dh == disp_h &&
        !present_dirty_test(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1) &&
        !ov_needs_present() &&
        !psx_present_vsync_owns_cadence() &&
        !gl_renderer_interpolation_owns_cadence()) {
        s_probe_skip++;
        gpu_timeline_note(GTL_PRESENT_SKIP, GTL_PATH_WIDE_FBO,
                          (uint32_t)disp_x | ((uint32_t)disp_y << 16));
        gl_perf_present_enter();
        gl_perf_present_exit(1);
        return 1;
    }
    gl_perf_present_enter();
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    wide_blit_center(fbo, disp_x, disp_y, disp_h);   /* fast-path: authoritative centre */

    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f, 0.f, 0.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    int interp_pair = interp_capture(
        fbo, 0, disp_y, g_wide_w, disp_h, linear, 0, GL_PRES_WIDE,
        disp_x, disp_y,
        present_dirty_test(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1));
    if (interp_pair) {
        hold_capture_native_fbo(fbo, 0, disp_y, g_wide_w, disp_h, 0, linear);
        interp_present_source_interval();
        gl_perf_present_exit(1);
        present_dirty_rect(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1, 0);
        present_force_consumed();
        s_last_present_path = GL_PRES_WIDE;
        s_last_dx = disp_x; s_last_dy = disp_y;
        s_last_dw = g_wide_w; s_last_dh = disp_h;
        return 1;
    }
    /* The surface is allocated at its own scale (s_wide_as), the level. */
    const float wext = (wide_as > s_out_scale && s_out_scale >= 1)
                           ? (float)wide_as / (float)s_out_scale : 1.0f;
    present_target_quad(tex, (float)g_wide_w * wext, (float)VRAM_H * wext,
                        0, disp_y, g_wide_w, disp_h, linear, lx, ly, lw, lh, 1, 1, s_out_scale);
    pres_record(GL_PRES_WIDE, disp_x, disp_y, g_wide_w, disp_h, lx, ly, lw, lh);
    gpu_timeline_note(GTL_PRESENT, GTL_PATH_WIDE_FBO,
                      (uint32_t)disp_x | ((uint32_t)disp_y << 16));
    hold_capture_drawable();
    latency_ring_mark(LAT_SWAP_BEGIN);
    gl_swap_with_osd();
    latency_ring_mark(LAT_SWAP_END);
    s_probe_swap++;
    gl_perf_present_exit(1);
    present_dirty_rect(0, disp_y, VRAM_W - 1, disp_y + disp_h - 1, 0);
    present_force_consumed();
    s_last_present_path = GL_PRES_WIDE;
    s_last_dx = disp_x; s_last_dy = disp_y; s_last_dw = g_wide_w; s_last_dh = disp_h;
    coh_record(GL_COH_PRESENT, 0, disp_y, g_wide_w - 1, disp_y + disp_h - 1);
    return 1;
}

/* ==== Dynamic internal resolution: runtime scale steps =======================
 * [video] dynamic_resolution (docs/ENHANCEMENTS.md, IR3). Opt-in: off,
 * s_alloc_scale stays 0 and nothing here runs.
 *
 * The hr surface and the native-wide surfaces are allocated once, at the
 * scale context init gave them (the ceiling); a lower level renders into
 * their lower-left VRAM*S corner (see s_alloc_scale). A step runs between
 * frames, after a present and never inside an open render pass:
 *  1. land every queued draw and upload (the pack and the raw mirror are
 *     then current);
 *  2. take the top-left sample of every native block of the old surface --
 *     the sample the pack reads -- into a 1x RGBA8 image, and copy the
 *     displayed rect and the current draw area aside at the old scale;
 *  3. reseed the whole corner at the new scale from the 1x image (nearest):
 *     every new block's top-left sample is the old one byte for byte, so the
 *     pack, and everything the guest can read back, cannot change;
 *  4. rescale the two saved rects back (each block's top-left exact, the rest
 *     the nearest old sample): the frame on screen and the one being drawn
 *     keep their detail instead of showing 1x blocks for a frame;
 *  5. rescale the native-wide surfaces' presented rows in place, through the
 *     scratch in row bands ordered so no source row is overwritten before it
 *     is read (only the margins while the centre is spliced from hr at each
 *     present);
 *  6. switch the scale (s_hr_scale, s_out_scale, the u_shift uniforms), mark
 *     the mask stencil stale (rebuilt from alpha as for any deferred encoding,
 *     at once while mask checking is on) and drop what was built at the old
 *     size (render-pass backup and images; the blend history restarts at the
 *     next capture).
 * Integer scales only: pack exactness, the u_shift grid and one-native-pixel
 * lines depend on them. Reallocating per step instead measured 30-106 ms at
 * 7x-10x on an Apple M4 (the first touch of the new surface), a visible hitch;
 * the reseed is a single pass. */
static GLuint s_dyn_tl_tex = 0, s_dyn_tl_fbo = 0;
static GLuint s_dyn_tl_prog = 0, s_dyn_seed_prog = 0, s_dyn_rs_prog = 0;
static GLint  s_dyn_tl_uHr = -1, s_dyn_tl_uS = -1;
static GLint  s_dyn_seed_uSrc = -1, s_dyn_seed_uS = -1;
static GLint  s_dyn_rs_uSrc = -1, s_dyn_rs_uOld = -1, s_dyn_rs_uNew = -1;
static GLint  s_dyn_rs_uOff = -1, s_dyn_rs_uSrcOff = -1;
static int    s_dyn_pending = 0;
static int    s_dyn_timing = -1;        /* PSX_DYNRES_TIMING: glFinish per part */
static GlDynresStats s_dyn_stats;

static const char *DYN_TL_FS =
    "#version 330\n"
    "uniform sampler2D u_hr;\n"
    "uniform int u_s;\n"
    "out vec4 frag;\n"
    "void main(){ frag = texelFetch(u_hr, ivec2(gl_FragCoord.xy) * u_s, 0); }\n";
static const char *DYN_SEED_FS =
    "#version 330\n"
    "uniform sampler2D u_src;\n"
    "uniform int u_s;\n"
    "out vec4 frag;\n"
    "void main(){ frag = texelFetch(u_src, ivec2(gl_FragCoord.xy) / u_s, 0); }\n";
/* Destination origin u_off (new px) in the target, source origin u_src_off
 * (old px) in u_src. Block b, offset r: old texel b*old + r*old/new, so r = 0
 * (the top-left, the pack's sample) is copied exactly. */
static const char *DYN_RESCALE_FS =
    "#version 330\n"
    "uniform sampler2D u_src;\n"
    "uniform int u_old;\n"
    "uniform int u_new;\n"
    "uniform ivec2 u_off;\n"
    "uniform ivec2 u_src_off;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  ivec2 d = ivec2(gl_FragCoord.xy) - u_off;\n"
    "  ivec2 b = d / u_new;\n"
    "  ivec2 r = d - b * u_new;\n"
    "  frag = texelFetch(u_src, u_src_off + b * u_old + (r * u_old) / u_new, 0);\n"
    "}\n";

static int dyn_resources(void) {
    if (!s_dyn_tl_prog) {
        s_dyn_tl_prog = build_program(PACK_VS, DYN_TL_FS);
        if (!s_dyn_tl_prog) return 0;
        s_dyn_tl_uHr = p_glGetUniformLocation(s_dyn_tl_prog, "u_hr");
        s_dyn_tl_uS = p_glGetUniformLocation(s_dyn_tl_prog, "u_s");
    }
    if (!s_dyn_seed_prog) {
        s_dyn_seed_prog = build_program(PACK_VS, DYN_SEED_FS);
        if (!s_dyn_seed_prog) return 0;
        s_dyn_seed_uSrc = p_glGetUniformLocation(s_dyn_seed_prog, "u_src");
        s_dyn_seed_uS = p_glGetUniformLocation(s_dyn_seed_prog, "u_s");
    }
    if (!s_dyn_rs_prog) {
        s_dyn_rs_prog = build_program(PACK_VS, DYN_RESCALE_FS);
        if (!s_dyn_rs_prog) return 0;
        s_dyn_rs_uSrc = p_glGetUniformLocation(s_dyn_rs_prog, "u_src");
        s_dyn_rs_uOld = p_glGetUniformLocation(s_dyn_rs_prog, "u_old");
        s_dyn_rs_uNew = p_glGetUniformLocation(s_dyn_rs_prog, "u_new");
        s_dyn_rs_uOff = p_glGetUniformLocation(s_dyn_rs_prog, "u_off");
        s_dyn_rs_uSrcOff = p_glGetUniformLocation(s_dyn_rs_prog, "u_src_off");
    }
    if (!s_dyn_tl_fbo) {
        s_dyn_tl_tex = make_tex(GL_RGBA8, VRAM_W, VRAM_H, GL_RGBA, GL_UNSIGNED_BYTE);
        if (!make_fbo(&s_dyn_tl_fbo, s_dyn_tl_tex, 0)) {
            glDeleteTextures(1, &s_dyn_tl_tex);
            s_dyn_tl_tex = s_dyn_tl_fbo = 0;
            return 0;
        }
        /* First touch now, not at the first step. */
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_dyn_tl_fbo);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    }
    return 1;
}

/* Steps are available: the full-VRAM regime above 1x, CPU VRAM not
 * authoritative over the GL surface. */
static int dyn_eligible(void) {
    return s_ctx && s_raster_ok && !s_hiw && !s_cpu_auth_dual && s_hr_scale > 1;
}

/* After init_gpu_raster (every context): the allocation is the ceiling. */
static void dyn_context_ready(void) {
    /* The old context took its objects with it. */
    s_dyn_tl_tex = s_dyn_tl_fbo = 0;
    s_dyn_tl_prog = s_dyn_seed_prog = s_dyn_rs_prog = 0;
    s_dyn_pending = 0;
    s_alloc_scale = 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++) s_dyn_wide_y0[i] = s_dyn_wide_y1[i] = 0;
    if (s_dyn_on && dyn_eligible()) {
        if (dyn_resources()) {
            s_alloc_scale = s_hr_scale;
            fprintf(stdout, "psxrecomp: GL dynamic resolution: levels 1x..%dx "
                    "(surfaces allocated at the level, ceiling %dx)\n", s_alloc_scale, s_alloc_scale);
        } else {
            fprintf(stdout, "psxrecomp: GL dynamic resolution unavailable (step "
                    "resources failed); the scale stays %dx\n", s_hr_scale);
        }
    }
}

void gl_renderer_set_dynamic_resolution(int on) {
    if (s_ctx) GL_RT_SYNC("set_dynamic_resolution");
    s_dyn_on = on ? 1 : 0;
    if (!s_ctx || !s_raster_ok) return;   /* dyn_context_ready decides */
    if (s_dyn_on) {
        /* Turned on while running: the surfaces are allocated at the current
         * scale, which becomes the ceiling. */
        if (!s_alloc_scale && dyn_eligible() && dyn_resources())
            s_alloc_scale = s_hr_scale;
    } else if (s_alloc_scale) {
        /* Back to the ceiling; the allocation stays (nothing reads it once
         * the level equals it). */
        s_dyn_pending = s_alloc_scale;
    }
}

int gl_renderer_dynamic_resolution_ceiling(void) {
    return (s_dyn_on && s_raster_ok) ? s_alloc_scale : 0;
}

static double dyn_ms(uint64_t t0, uint64_t t1) {
    return (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}
static uint64_t dyn_mark(void) {
    if (s_dyn_timing) glFinish();
    return SDL_GetPerformanceCounter();
}

/* Fullscreen-triangle pass of `prog` into dst_fbo over the viewport
 * (x, y, w, h), no scissor/blend/stencil. */
static void dyn_pass_begin(GLuint prog, GLuint dst_fbo, int x, int y, int w, int h,
                           GLuint src_tex) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, dst_fbo);
    glViewport(x, y, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glUseProgram(prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    p_glBindVertexArray(s_empty_vao);
}
static void dyn_pass_end(void) {
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
}

/* Draw native rect (nx, ny, nw, nh) of dst_fbo at snew from src_tex, whose
 * copy of that rect at sold starts at (sx, sy) texels. */
static void dyn_rescale_rect(GLuint src_tex, int sx, int sy, int sold, GLuint dst_fbo,
                             int nx, int ny, int nw, int nh, int snew) {
    dyn_pass_begin(s_dyn_rs_prog, dst_fbo, nx * snew, ny * snew, nw * snew, nh * snew,
                   src_tex);
    p_glUniform1i(s_dyn_rs_uSrc, 0);
    p_glUniform1i(s_dyn_rs_uOld, sold);
    p_glUniform1i(s_dyn_rs_uNew, snew);
    p_glUniform2i(s_dyn_rs_uOff, nx * snew, ny * snew);
    p_glUniform2i(s_dyn_rs_uSrcOff, sx, sy);
    dyn_pass_end();
}

/* Rescale native columns [cx0, cx1) x rows [ry0, ry1) of a surface in place,
 * in bands of rows through the scratch. Down: bands bottom-up (from ry0);
 * up: top-down. A band's destination rows [y0*snew, y1*snew) then never
 * reach a row a later band still has to read: for a step down every later
 * band's source starts at y >= y1, i.e. at y1*sold >= y1*snew; a step up is
 * the mirror image. Within a band the source is staged before it is drawn.
 * Columns ranges (up to two, e.g. both margins) share the band staging. */
static void dyn_rescale_in_place(GLuint fbo, int surf_w, const int (*cols)[2], int ncols,
                                 int ry0, int ry1, int sold, int snew) {
    const int BAND = 64;   /* native rows per band */
    if (ry1 <= ry0 || ncols <= 0) return;
    if (!scratch_ensure(surf_w * sold, BAND * sold)) return;
    int nb = (ry1 - ry0 + BAND - 1) / BAND;
    for (int k = 0; k < nb; k++) {
        int b = snew < sold ? k : nb - 1 - k;
        int y0 = ry0 + b * BAND, bh = (y0 + BAND <= ry1) ? BAND : ry1 - y0;
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
        glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, y0 * sold, surf_w * sold, (y0 + bh) * sold,
                            0, 0, surf_w * sold, bh * sold, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        for (int c = 0; c < ncols; c++)
            if (cols[c][1] > cols[c][0])
                dyn_rescale_rect(s_scratch_tex, cols[c][0] * sold, 0, sold, fbo,
                                 cols[c][0], y0, cols[c][1] - cols[c][0], bh, snew);
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}

/* Render thread: the displayed rect a recorded step keeps at full detail,
 * captured when the step was recorded (gpu.c's display state is the
 * emulation thread's). */
static int s_dyn_rth_disp_valid = 0, s_dyn_rth_disp[5];
static atomic_flag s_dyn_stats_lock = ATOMIC_FLAG_INIT;   /* s_dyn_stats: render thread writes */
static void dyn_stats_lock(void) { while (atomic_flag_test_and_set_explicit(&s_dyn_stats_lock, memory_order_acquire)) {} }
static void dyn_stats_unlock(void) { atomic_flag_clear_explicit(&s_dyn_stats_lock, memory_order_release); }
static int s_rthm_scale = 1;                /* emulation-side level (recording) */

static int dyn_apply(int snew) {
    /* Never in the windowed high-resolution mode (s_hiw): dyn_eligible keeps
     * s_alloc_scale 0 there; refuse here too rather than step its tiles. */
    if (!s_alloc_scale || !s_raster_ok || !s_ctx || s_hiw) { s_dyn_stats.refused++; return 0; }
    if (snew < 1) snew = 1;
    if (snew > s_alloc_scale) snew = s_alloc_scale;
    if (snew == s_hr_scale) return 1;
    if (s_pass_active) {   /* never inside a render pass: after its present */
        s_dyn_pending = snew;
        s_dyn_stats.deferred++;
        return 0;
    }
    if (s_dyn_timing < 0) {
        const char *e = getenv("PSX_DYNRES_TIMING");
        s_dyn_timing = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    const int sold = s_hr_scale;
    const uint64_t t0 = SDL_GetPerformanceCounter();
    /* 1. Land queued work: the old surface is complete. */
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    hiw_flush_queue();
    pack_flush();
    const uint64_t t1 = dyn_mark();
    /* 2a. The pack's samples, 1x. */
    dyn_pass_begin(s_dyn_tl_prog, s_dyn_tl_fbo, 0, 0, VRAM_W, VRAM_H, s_hr_tex);
    p_glUniform1i(s_dyn_tl_uHr, 0);
    p_glUniform1i(s_dyn_tl_uS, sold);
    dyn_pass_end();
    /* 2b. The displayed rect and the draw area (front and back buffer) at the
     *     old scale, side by side in the scratch. A rect the scratch cannot
     *     hold at the GPU limit is left to the reseed (1x detail until it is
     *     redrawn). */
    int qn = 0, qr[2][4];
    {
        GpuDisplayInfo di;
        if (rth_replaying()) {
            memset(&di, 0, sizeof di);
            di.disabled = !s_dyn_rth_disp_valid || s_dyn_rth_disp[0];
            di.display_x = (uint32_t)s_dyn_rth_disp[1]; di.display_y = (uint32_t)s_dyn_rth_disp[2];
            di.width = (uint32_t)s_dyn_rth_disp[3];     di.height = (uint32_t)s_dyn_rth_disp[4];
        } else {
            gpu_get_display_info(&di);
        }
        if (!di.disabled && di.width > 0 && di.height > 0) {
            qr[qn][0] = (int)di.display_x; qr[qn][1] = (int)di.display_y;
            qr[qn][2] = (int)di.width;     qr[qn][3] = (int)di.height;
            qn++;
        }
        int ax = s_area_x1, ay = s_area_y1;
        int aw = s_area_x2 - s_area_x1 + 1, ah = s_area_y2 - s_area_y1 + 1;
        if (aw > 0 && ah > 0 && !(qn && ax >= qr[0][0] && ay >= qr[0][1] &&
                                  ax + aw <= qr[0][0] + qr[0][2] &&
                                  ay + ah <= qr[0][1] + qr[0][3])) {
            qr[qn][0] = ax; qr[qn][1] = ay; qr[qn][2] = aw; qr[qn][3] = ah;
            qn++;
        }
        int sw = 0, sh = 0, keep = 0;
        for (int i = 0; i < qn; i++) {
            int *r = qr[i];
            if (r[0] < 0) { r[2] += r[0]; r[0] = 0; }
            if (r[1] < 0) { r[3] += r[1]; r[1] = 0; }
            if (r[0] + r[2] > VRAM_W) r[2] = VRAM_W - r[0];
            if (r[1] + r[3] > VRAM_H) r[3] = VRAM_H - r[1];
            if (r[2] <= 0 || r[3] <= 0) continue;
            int nsw = sw + r[2] * sold, nsh = r[3] * sold > sh ? r[3] * sold : sh;
            if (s_gl_max_dim > 0 && (nsw > s_gl_max_dim || nsh > s_gl_max_dim)) continue;
            sw = nsw; sh = nsh;
            if (keep != i) memcpy(qr[keep], r, sizeof qr[0]);
            keep++;
        }
        qn = keep;
        if (qn && !scratch_ensure(sw, sh)) qn = 0;
        int ox = 0;
        for (int i = 0; i < qn; i++) {
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_hr_fbo);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_scratch_fbo);
            glDisable(GL_SCISSOR_TEST);
            p_glBlitFramebuffer(qr[i][0] * sold, qr[i][1] * sold,
                                (qr[i][0] + qr[i][2]) * sold, (qr[i][1] + qr[i][3]) * sold,
                                ox, 0, ox + qr[i][2] * sold, qr[i][3] * sold,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);
            ox += qr[i][2] * sold;
        }
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    }
    const uint64_t t2 = dyn_mark();
    /* 3. The hr surface at the new level: a new allocation of exactly
     *    VRAM*snew (the old one is only read through the 1x image and the
     *    scratch from here on), reseeded whole. If the allocation fails, a
     *    step down reseeds the old surface's corner in place; a step up past
     *    the old allocation is refused. */
    GLuint ntex = 0, nrb = 0, nfbo = 0;
    if (!hr_alloc_surface(snew, &ntex, &nrb, &nfbo)) {
        if (snew > s_hr_alloc) {
            s_dyn_stats.refused++;
            fprintf(stdout, "psxrecomp: dynamic resolution %dx -> %dx refused (the "
                    "surface could not be allocated)\n", sold, snew);
            return 0;
        }
    } else {
        p_glDeleteFramebuffers(1, &s_hr_fbo);
        glDeleteTextures(1, &s_hr_tex);
        p_glDeleteRenderbuffers(1, &s_hr_rb);
        s_hr_fbo = nfbo; s_hr_tex = ntex; s_hr_rb = nrb;
        s_hr_alloc = snew;
        s_dyn_stats.hr_reallocs++;
    }
    /*    Reseed at the new scale. */
    dyn_pass_begin(s_dyn_seed_prog, s_hr_fbo, 0, 0, VRAM_W * snew, VRAM_H * snew,
                   s_dyn_tl_tex);
    p_glUniform1i(s_dyn_seed_uSrc, 0);
    p_glUniform1i(s_dyn_seed_uS, snew);
    dyn_pass_end();
    const uint64_t t3 = dyn_mark();
    /* 4. The saved rects back at full detail (where they overlap, both hold
     *    the same old pixels). */
    for (int i = 0, ox = 0; i < qn; i++) {
        dyn_rescale_rect(s_scratch_tex, ox, 0, sold, s_hr_fbo,
                         qr[i][0], qr[i][1], qr[i][2], qr[i][3], snew);
        ox += qr[i][2] * sold;
    }
    const uint64_t t4 = dyn_mark();
    /* 5. Native-wide surfaces: presentation-only, rescaled in place over the
     *    rows they have presented (all rows before the first present). While
     *    the centre is spliced from hr at every present (wide_blit_center),
     *    only the margins hold anything of their own. */
    if (g_wide_w > 0) {
        int cols[2][2], nc = 0;
        int native_w = g_wide_w - 2 * g_wide_off;
        if (s_wide_fast && !view_enabled && native_w > 0 && g_wide_off > 0) {
            cols[0][0] = 0;                     cols[0][1] = g_wide_off;
            cols[1][0] = g_wide_off + native_w; cols[1][1] = g_wide_w;
            nc = 2;
        } else {
            cols[0][0] = 0; cols[0][1] = g_wide_w;
            nc = 1;
        }
        for (int i = 0; i < WIDE_MAX_SURF; i++) {
            if (!s_wide_fbo[i]) continue;
            int y0 = s_dyn_wide_y0[i], y1 = s_dyn_wide_y1[i];
            if (y1 <= y0) { y0 = 0; y1 = VRAM_H; }
            /* Reallocated at the new level (s_wide_as): the same rows and
             * columns rescaled from the old surface into a cleared new one.
             * If the allocation fails, a step down rescales in place as
             * before and the surface keeps its larger allocation; a step up
             * past it drops the surface (recreated at the next target). */
            GLuint ntex = 0, nrb = 0, nfbo = 0;
            if (wide_alloc_surface(snew, &ntex, &nrb, &nfbo)) {
                for (int c = 0; c < nc; c++)
                    if (cols[c][1] > cols[c][0])
                        dyn_rescale_rect(s_wide_tex[i], cols[c][0] * sold, y0 * sold, sold,
                                         nfbo, cols[c][0], y0, cols[c][1] - cols[c][0],
                                         y1 - y0, snew);
                if (g_wide_cur == s_wide_fbo[i]) g_wide_cur = nfbo;
                p_glDeleteFramebuffers(1, &s_wide_fbo[i]);
                glDeleteTextures(1, &s_wide_tex[i]);
                p_glDeleteRenderbuffers(1, &s_wide_rb[i]);
                s_wide_fbo[i] = nfbo; s_wide_tex[i] = ntex; s_wide_rb[i] = nrb;
                s_wide_as[i] = snew;
                s_dyn_stats.wide_reallocs++;
            } else if (snew <= s_wide_as[i]) {
                dyn_rescale_in_place(s_wide_fbo[i], g_wide_w, (const int (*)[2])cols, nc,
                                     y0, y1, sold, snew);
            } else {
                if (g_wide_cur == s_wide_fbo[i]) g_wide_cur = 0;
                p_glDeleteFramebuffers(1, &s_wide_fbo[i]); s_wide_fbo[i] = 0;
                glDeleteTextures(1, &s_wide_tex[i]); s_wide_tex[i] = 0;
                p_glDeleteRenderbuffers(1, &s_wide_rb[i]); s_wide_rb[i] = 0;
                s_wide_base[i] = -1; s_wide_as[i] = 0;
            }
            wst_all(i);   /* colour only */
        }
    }
    /* 5b. The copy/stencil scratch back to its size for a surface allocated
     *     at snew (alloc_hr_targets): the step grew it to the old level's
     *     rects, and every tiled stencil rebuild renders into it. */
    {
        int hw = VRAM_W * snew, hh = VRAM_H * snew;
        int sw = snew <= 2 ? hw : (hw < GL_SCRATCH_TILE ? hw : GL_SCRATCH_TILE);
        int sh = snew <= 2 ? hh : (hh < GL_SCRATCH_TILE ? hh : GL_SCRATCH_TILE);
        if (s_scratch_w > sw || s_scratch_h > sh) {
            p_glActiveTexture(PSXGL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindTexture(GL_TEXTURE_2D, 0);
            s_scratch_w = sw; s_scratch_h = sh;
        }
    }
    const uint64_t t5 = dyn_mark();
    /* 6. The new scale. */
    s_hr_scale = s_out_scale = snew;
    s_shift_hr = s_shift_hi = 0.5f / (float)snew - 1.0f / 64.0f;
    p_glUseProgram(s_geo_prog);  p_glUniform1f(s_geo_uShift, s_shift_hr);
    p_glUseProgram(s_tex_prog);  p_glUniform1f(s_tex_uShift, s_shift_hr);
    p_glUseProgram(s_blit_prog);
    p_glUniform1f(p_glGetUniformLocation(s_blit_prog, "u_shift"), s_shift_hr);
    p_glUseProgram(0);
    /* Mask stencil: rebuilt from alpha, the deferred path; at once while
     * mask checking is on (no E6h transition will ask for it). */
    s_stencil_valid = 0;
    rect_add(&s_stencil_stale, 0, 0, VRAM_W - 1, VRAM_H - 1);
    if (s_mask_check) rebuild_mask_stencils();
    s_pb_valid = 0;
    pass_gens_invalidate();
    for (int r = 0; r < PRES_ROWS; r++) s_present_dirty[r] = ~0ull;
    const uint64_t t6 = SDL_GetPerformanceCounter();
    dyn_stats_lock();
    s_dyn_stats.steps++;
    s_dyn_stats.last_from = sold;
    s_dyn_stats.last_to = snew;
    s_dyn_stats.last_ms = dyn_ms(t0, t6);
    s_dyn_stats.last_prep_ms = dyn_ms(t0, t1);
    s_dyn_stats.last_seed_ms = dyn_ms(t1, t2) + dyn_ms(t2, t3);
    s_dyn_stats.last_rects_ms = dyn_ms(t3, t4);
    s_dyn_stats.last_wide_ms = dyn_ms(t4, t5);
    s_dyn_stats.last_rects = qn;
    dyn_stats_unlock();
    if (s_dyn_timing)
        fprintf(stdout, "psxrecomp: dynamic resolution %dx -> %dx in %.2f ms (drain %.2f, "
                "1x+rects %.2f, reseed %.2f, rects back %.2f, wide %.2f)\n", sold, snew,
                s_dyn_stats.last_ms, dyn_ms(t0, t1), dyn_ms(t1, t2), dyn_ms(t2, t3),
                dyn_ms(t3, t4), dyn_ms(t4, t5));
    return 1;
}

static void dyn_after_present(void) {
    /* Recording: the present was queued, not run; the step it would follow
     * is recorded after it (gl_renderer_step_internal_scale_now). */
    if (s_rth_on && !rt_on_render_thread() && !rt_held()) return;
    if (!s_dyn_pending) return;
    int s = s_dyn_pending;
    s_dyn_pending = 0;
    (void)dyn_apply(s);   /* inside a pass: re-queued */
}

/* Render thread: record the step in stream order (after the frame just
 * closed, before the next one's first draw); the emulation side's level
 * moves at once (s_rthm_scale), the backend's when the step is replayed.
 * s_alloc_scale only changes under a sync point. 1 when recorded. */
static int dyn_record_step(int scale) {
    if (!(s_rth_on && !rt_on_render_thread() && !rt_held())) return 0;
    if (gpu_display_is_depth24()) { gl_rth_acquire("depth24"); return 0; }
    if (scale == s_rthm_scale) return 1;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    int32_t a[6] = { scale, di.disabled ? 1 : 0, (int32_t)di.display_x,
                     (int32_t)di.display_y, (int32_t)di.width, (int32_t)di.height };
    if (!rth_rec_ints(RTH_DYN_STEP, 0, 6, a)) { gl_rth_acquire("dyn_step"); return 0; }
    s_rthm_scale = scale;
    return 1;
}

int gl_renderer_request_internal_scale(int scale) {
    if (!s_alloc_scale || !s_raster_ok) return 0;
    if (scale < 1) scale = 1;
    if (scale > s_alloc_scale) scale = s_alloc_scale;
    if (dyn_record_step(scale)) return 1;
    s_dyn_pending = scale == s_hr_scale ? 0 : scale;
    return 1;
}

int gl_renderer_step_internal_scale_now(int scale) {
    if (!s_alloc_scale || !s_raster_ok) return 0;
    if (scale > s_alloc_scale) scale = s_alloc_scale;
    if (scale < 1) scale = 1;
    if (dyn_record_step(scale)) return 1;
    s_dyn_pending = 0;
    return dyn_apply(scale) && s_hr_scale == scale;
}

void gl_renderer_dynres_stats(GlDynresStats *out) {
    if (!out) return;
    dyn_stats_lock();
    *out = s_dyn_stats;
    dyn_stats_unlock();
    /* While recording, the level the emulation thread has asked for. */
    const int rec = s_rth_on && !rt_on_render_thread() && !rt_held();
    out->level = !s_raster_ok ? 0 : rec ? s_rthm_scale : s_hr_scale;
    out->ceiling = gl_renderer_dynamic_resolution_ceiling();
    out->pending = rec ? 0 : s_dyn_pending;
}

void gl_renderer_host_ledger(GlHostLedger *out) {
    if (out) *out = s_dyn_ledger;
}

void gl_renderer_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                              int force_4_3) {
    present_vram_impl(disp_x, disp_y, w, h, linear, force_4_3);
    dyn_after_present();
}

int gl_renderer_present_wide_fbo(int disp_x, int disp_y, int disp_h, int linear) {
    int r = present_wide_fbo_impl(disp_x, disp_y, disp_h, linear);
    dyn_after_present();
    return r;
}

static const GpuRenderBackend GL_BACKEND = {
    .name = "opengl",
    .init = glb_init, .set_scale = glb_set_scale, .scale = glb_scale,
    .set_texture_filter = glb_set_texture_filter, .texture_filter = glb_texture_filter,
    .set_semi_transparency = glb_set_semi_transparency, .set_mask_bits = glb_set_mask_bits,
    .set_texture_window = glb_set_texture_window, .set_color_modulation = glb_set_color_modulation,
    .set_precise_triangle = glb_set_precise_triangle,
    .set_perspective_triangle = glb_set_perspective_triangle,
    .set_dither = glb_set_dither,
    .fill_rect = glb_fill_rect, .copy_rect = glb_copy_rect,
    .draw_flat_triangle = glb_draw_flat_triangle, .draw_gouraud_triangle = glb_draw_gouraud_triangle,
    .draw_textured_triangle = glb_draw_textured_triangle,
    .draw_shaded_textured_triangle = glb_draw_shaded_textured_triangle,
    .draw_flat_rect = glb_draw_flat_rect, .draw_textured_rect = glb_draw_textured_rect,
    .draw_textured_rect_scaled = glb_draw_textured_rect_scaled,
    .draw_line = glb_draw_line, .draw_shaded_line = glb_draw_shaded_line,
    .render_display = glb_render_display, .render_display_hires = glb_render_display_hires,
    .vram_write = glb_vram_write, .vram_read = glb_vram_read,
    .vram_transfer_in = glb_vram_transfer_in, .vram_transfer_out = glb_vram_transfer_out,
    .set_draw_area = glb_set_draw_area, .get_draw_area = glb_get_draw_area,
    .set_draw_offset = glb_set_draw_offset,
    .wide_configure = glb_wide_configure,
    .wide_set_view = glb_wide_set_view,
    .wide_set_target = glb_wide_set_target,
    .wide_disable_target = glb_wide_disable_target,
    .wide_clear = glb_wide_clear,
    .wide_clear_margins = glb_wide_clear_margins,
    .render_wide_display = glb_render_wide_display,
    .wide_dump_full = glb_wide_dump_full,
    .set_depth_triangle = glb_set_depth_triangle,
};

/* ==== Render thread: recording, replay, hand-off ===========================
 * See the state block near the top of the file and docs/RENDER_THREAD.md. */

/* Emulation-side mirrors of backend state the emulation thread must answer
 * without a sync point. They follow the recorded calls exactly as the backend
 * will apply them, and are re-read from the backend whenever the emulation
 * thread holds the context (rth_mirror_resync). */
static int s_rthm_wide_w = 0, s_rthm_wide_off = 0;
static int s_rthm_base[WIDE_MAX_SURF];
static int s_rthm_area[4] = { 0, 0, VRAM_W - 1, VRAM_H - 1 };
/* Last stream state sent (RTH_STATE); -1 forces the first send. */
static int s_rthe_flat_bd = -1, s_rthe_vp_w = -1, s_rthe_bg_full = -1;
static uint64_t s_rth_presents = 0, s_rth_presents_stale = 0;

#define RTHF_PRIM      0x0001u
#define RTHF_TAGGED    0x0002u
#define RTHF_BD_SHIFT  2          /* bits 2-3: psx_ws_prim_in_backdrop() 0..3 */

static void rth_mirror_resync(void) {
    s_rthm_scale = s_out_scale;
    s_rthm_wide_w = g_wide_w;
    s_rthm_wide_off = g_wide_off;
    s_rthm_cur = g_wide_cur != 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        s_rthm_base[i] = s_wide_fbo[i] ? s_wide_base[i] : -1;
    s_rthm_area[0] = s_area_x1; s_rthm_area[1] = s_area_y1;
    s_rthm_area[2] = s_area_x2; s_rthm_area[3] = s_area_y2;
}

/* wide_fbo_for() as it will run on the render thread: the same lookup, limit
 * check and slot allocation. Only a driver allocation failure can differ. */
static int rth_mirror_wide_for(int base_x) {
    if (s_rthm_wide_w <= 0) return 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_rthm_base[i] == base_x) return 1;
    if (s_gl_max_dim > 0 &&
        ((int64_t)s_rthm_wide_w * alloc_scale_for(s_rthm_scale) > s_gl_max_dim ||
         (int64_t)VRAM_H * alloc_scale_for(s_rthm_scale) > s_gl_max_dim))
        return 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_rthm_base[i] < 0) { s_rthm_base[i] = base_x; return 1; }
    return 0;
}
static int rth_mirror_wide_present_ok(int base_x) {
    if (s_rthm_wide_w <= 0) return 0;
    for (int i = 0; i < WIDE_MAX_SURF; i++)
        if (s_rthm_base[i] == base_x) return 1;
    return 0;
}

static int rth_rec_ints(uint16_t op, uint16_t flags, int n, const int32_t *v) {
    int32_t *p = (int32_t *)rt_cmd_begin(op, flags, (uint32_t)n * 4u);
    if (!p) return 0;
    memcpy(p, v, (size_t)n * 4u);
    rt_cmd_commit();
    return 1;
}

/* Native-wide stream state read by the fast-path gate (wide_fast_center_valid)
 * and the backdrop gate; sent whenever it changes, before the call that would
 * first observe the change. */
static void rth_send_state(void) {
    if (s_rthm_wide_w <= 0) return;
    int flat = gpu_ws_nw_flat_backdrop_enabled() ? 1 : 0;
    int vp = gpu_ws_netplay_local_viewport_width();
    int bg = gpu_ws_background_requires_full_composite() ? 1 : 0;
    if (flat == s_rthe_flat_bd && vp == s_rthe_vp_w && bg == s_rthe_bg_full) return;
    s_rthe_flat_bd = flat; s_rthe_vp_w = vp; s_rthe_bg_full = bg;
    RTH_REC(RTH_STATE, 0, flat, vp, bg);
}

/* Per-primitive widescreen tags, read by bd_prim_gate during replay. Taken
 * only when the backend would read them (native-wide on, stretch gate on). */
static uint16_t rth_prim_flags(void) {
    if (s_rthm_wide_w <= 0 || !g_ws_bd_stretch_on) return 0;
    uint16_t f = RTHF_PRIM;
    if (g_ws_bd_phase_mode != 0)
        f |= (uint16_t)((psx_ws_prim_in_backdrop() & 3) << RTHF_BD_SHIFT);
    else if (psx_ws_prim_is_tagged())
        f |= RTHF_TAGGED;
    return f;
}

static int gl_rth_eligible(void) {
    extern int psx_netplay_active(void);
    return s_raster_ok && s_ctx && !s_cpu_auth_dual && !s_depth24_skip_up &&
           !(s_hd_native_authority && s_rth_a0_open) &&
           !gpu_display_is_depth24() && !s_interp_enabled && !s_pass_active &&
           !psx_netplay_active() && !psx_openxr_session_active() &&
           gr_backend() == GR_BACKEND_OPENGL;
}

/* HD texture authority keeps native VRAM on the CPU raster, which runs on
 * the render thread's private copy: hand its result to gpu.c's array, the
 * way ensure_cpu reads the FBO back otherwise. Called only with the render
 * thread drained and not held, so the private copy is the whole native
 * VRAM: no A0 payload can be streaming (its header takes the context). */
static void rth_hd_publish_private(void) {
    if (!s_hd_native_authority) return;
    memcpy(s_rth_vram_pub, s_rth_vram_priv, (size_t)VRAM_W * VRAM_H * sizeof(uint16_t));
}

static void gl_rth_acquire(const char *reason) {
    if (!s_rth_on || rt_on_render_thread() || rt_held()) return;
    rt_acquire(reason);
    rth_hd_publish_private();
    s_vram = s_rth_vram_pub;
    rth_rebind_vram(s_rth_vram_pub);
}

static void gl_rth_release(void) {
    if (!s_rth_on || !rt_held()) return;
    /* Everything the emulation thread did is in gpu.c's array; the render
     * thread continues from a copy of it. */
    memcpy(s_rth_vram_priv, s_rth_vram_pub, (size_t)VRAM_W * VRAM_H * sizeof(uint16_t));
    s_vram = s_rth_vram_priv;
    rth_rebind_vram(s_rth_vram_priv);
    rth_mirror_resync();
    rt_release();
}

/* Record this call (1) or run it directly (0). Direct: no render thread, on
 * the render thread itself (replay), or while the emulation thread holds the
 * context. A depth24 display turns the call into a sync point first. */
static int rth_record_mode(void) {
    if (!s_rth_on || rt_on_render_thread() || rt_held()) return 0;
    if (gpu_display_is_depth24()) { gl_rth_acquire("depth24"); return 0; }
    rth_send_state();
    return 1;
}

/* Present records carry the host overlays the emulation thread rasterized. */
typedef struct RthOvHdr {
    int32_t needs_present, has, w[4], h[4];
    float   slide;
    int32_t args[8], nargs;
} RthOvHdr;

static int rth_record_present(uint16_t op, int n, const int32_t *v) {
    const uint32_t *px[4] = { NULL, NULL, NULL, NULL };
    int w[4] = { 0 }, h[4] = { 0 };
    RthOvHdr hd;
    memset(&hd, 0, sizeof hd);
    hd.needs_present = host_osd_needs_present();
    for (int i = 0; i < 4; i++) {
        if (ov_image(i, &px[i], &w[i], &h[i]) && px[i] && w[i] > 0 && h[i] > 0) {
            hd.has |= 1 << i; hd.w[i] = w[i]; hd.h[i] = h[i];
        }
    }
    hd.slide = psx_rewind_slide();
    hd.nargs = n;
    for (int i = 0; i < n && i < 8; i++) hd.args[i] = v[i];
    size_t bytes = sizeof hd;
    for (int i = 0; i < 4; i++)
        if (hd.has & (1 << i)) bytes += (size_t)hd.w[i] * hd.h[i] * 4u;
    uint8_t *p = (uint8_t *)rt_cmd_begin(op, 0, (uint32_t)bytes);
    if (!p) return 0;
    memcpy(p, &hd, sizeof hd);
    uint8_t *q = p + sizeof hd;
    for (int i = 0; i < 4; i++)
        if (hd.has & (1 << i)) {
            size_t n4 = (size_t)hd.w[i] * hd.h[i] * 4u;
            memcpy(q, px[i], n4);
            q += n4;
        }
    rt_cmd_commit();
    host_osd_present_done();
    { extern void psx_ws_dbg_gate_frame_snapshot(void); psx_ws_dbg_gate_frame_snapshot(); }
    s_rth_presents++;
    return 1;
}

static void rth_present_payload(uint16_t op, const uint8_t *p);
static void rth_replay_present(const RtCmd *c, const uint8_t *p) {
    static int skipped_last = 0;
    const int stale = rt_frames_ahead() >= 2 && !skipped_last;
    /* Frame generation schedules (or drops) presents itself. */
    if (s_fg_on && fg_on_present(c->op, p, c->payload, stale)) { skipped_last = 0; return; }
    if (stale) {
        /* Two later frames are already recorded, so the emulation thread is
         * at (or near) the in-flight bound waiting on us. Showing this frame
         * would only delay those; skip its present (its drawing has been
         * replayed and stays). One frame behind is the normal pipelined
         * state and still presents every frame, and never two presents in a
         * row are skipped, so a saturated render thread keeps showing at
         * least every other frame. */
        s_rth_presents_stale++;
        skipped_last = 1;
        return;
    }
    skipped_last = 0;
    rth_present_payload(c->op, p);
}

/* Present a recorded present's payload (header, overlay copies). */
static void rth_present_payload(uint16_t op, const uint8_t *p) {
    RthOvHdr hd;
    memcpy(&hd, p, sizeof hd);
    const uint8_t *q = p + sizeof hd;
    memset(&s_rth_ov, 0, sizeof s_rth_ov);
    s_rth_ov.valid = 1;
    s_rth_ov.needs_present = hd.needs_present;
    s_rth_ov.slide = hd.slide;
    for (int i = 0; i < 4; i++)
        if (hd.has & (1 << i)) {
            s_rth_ov.px[i] = (const uint32_t *)q;
            s_rth_ov.w[i] = hd.w[i];
            s_rth_ov.h[i] = hd.h[i];
            q += (size_t)hd.w[i] * hd.h[i] * 4u;
        }
    const int32_t *a = hd.args;
    if (op == RTH_PRESENT_VRAM)
        gl_renderer_present_vram(a[0], a[1], a[2], a[3], a[4], a[5]);
    else
        (void)gl_renderer_present_wide_fbo(a[0], a[1], a[2], a[3]);
    s_rth_ov.valid = 0;
}


/* ---- render-thread frame cost (dynamic resolution) -------------------------
 * docs/RENDER_THREAD.md, "Dynamic resolution". Measured on the render thread
 * per replayed guest frame, between its first record and its RTH_FRAME
 * marker (recorded at the frame boundary): CPU = wall time replaying it minus
 * the render thread's idle waits for records and its time in the swap (the
 * display's vsync); GPU = a GL_TIME_ELAPSED query around the same span, read
 * back when available (never waited on). cost = max(CPU, GPU). (GL_TIMESTAMP
 * counters read 0 on macOS.) Queries of one target cannot nest, so while this
 * measures, frame_perf's own TIME_ELAPSED brackets stand aside. A frame during
 * which the context went to the emulation thread (a sync point) is dropped:
 * its span holds the emulation thread's own drawing. Published as running
 * totals for the emulation thread (gl_renderer_render_thread_costs). */
#define RTHF_Q 8
/* Frame generation pauses a frame's measurement while it draws (the frame
 * then has up to RTHF_SEG queries, summed). */
#define RTHF_SEG 4
static _Atomic int s_rthf_want = 0;          /* host: measure (dynres active) */
static int      s_rthf_qok = -1;             /* render thread: queries usable */
static GLuint   s_rthf_q[RTHF_Q][RTHF_SEG];
static int      s_rthf_nseg[RTHF_Q];
static uint64_t s_rthf_slot_cpu[RTHF_Q];
static int      s_rthf_paused = 0;           /* a pause ended this frame's query */
static uint64_t s_rthf_pause_t0 = 0, s_rthf_pause_swap = 0, s_rthf_excl = 0;
static void     fg_note_real_cost(uint64_t cpu_ns, uint64_t gpu_ns, int has_gpu);   /* frame generation */
static unsigned s_rthf_head = 0, s_rthf_tail = 0;
static int      s_rthf_open = 0, s_rthf_taint = 0, s_rthf_has_q = 0;
static uint64_t s_rthf_t0 = 0, s_rthf_idle0 = 0, s_rthf_swap0 = 0;
static _Atomic uint64_t s_rthf_frames, s_rthf_cost_ns, s_rthf_cpu_ns, s_rthf_gpu_ns,
                        s_rthf_gpu_frames, s_rthf_dropped;

static uint64_t host_now_ns_rthf(void) {
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1.0e9 /
                      (double)SDL_GetPerformanceFrequency());
}

static void rthf_publish(uint64_t cpu, uint64_t gpu, int has_gpu) {
    uint64_t cost = has_gpu && gpu > cpu ? gpu : cpu;
    atomic_fetch_add(&s_rthf_cpu_ns, cpu);
    if (has_gpu) {
        atomic_fetch_add(&s_rthf_gpu_ns, gpu);
        atomic_fetch_add(&s_rthf_gpu_frames, 1);
    }
    atomic_fetch_add(&s_rthf_cost_ns, cost);
    atomic_fetch_add(&s_rthf_frames, 1);   /* last: the totals above are in */
    /* Frame generation budgets with the CPU time (see
     * fg_note_real_cost for why only the CPU time plans). */
    fg_note_real_cost(cpu, gpu, has_gpu);
}

/* Read every finished query pair, oldest first, without waiting. */
static void rthf_poll(void) {
    while (s_rthf_tail != s_rthf_head) {
        const unsigned i = s_rthf_tail % RTHF_Q;
        GLuint64 avail = 0, ns = 0, sum = 0;
        /* Queries complete in order: the last segment's result means all. */
        p_glGetQueryObjectui64v(s_rthf_q[i][s_rthf_nseg[i] - 1], GL_QUERY_RESULT_AVAILABLE,
                                &avail);
        if (!avail) break;
        for (int k = 0; k < s_rthf_nseg[i]; k++) {
            ns = 0;
            p_glGetQueryObjectui64v(s_rthf_q[i][k], GL_QUERY_RESULT, &ns);
            sum += ns;
        }
        rthf_publish(s_rthf_slot_cpu[i], (uint64_t)sum, 1);
        s_rthf_tail++;
    }
}

static void rthf_begin(void) {
    if (s_rthf_qok < 0) {
        s_rthf_qok = (p_glGenQueries && p_glBeginQuery && p_glEndQuery &&
                      p_glGetQueryObjectui64v &&
                      !(getenv("PSX_DYNRES_GPU_TIMER") && getenv("PSX_DYNRES_GPU_TIMER")[0] == '0')) ? 1 : 0;
        if (s_rthf_qok) p_glGenQueries(RTHF_Q * RTHF_SEG, &s_rthf_q[0][0]);
    }
    s_rthf_open = 1;
    s_rthf_taint = 0;
    s_rthf_paused = 0;
    s_rthf_excl = 0;
    s_rthf_t0 = host_now_ns_rthf();
    s_rthf_idle0 = rt_render_idle_ns();
    s_rthf_swap0 = s_rthf_swap_ns;
    s_rthf_has_q = s_rthf_qok && s_rthf_head - s_rthf_tail < RTHF_Q;
    if (s_rthf_has_q) {
        gl_perf_yield();   /* close frame_perf's open bracket, if any */
        const unsigned i = s_rthf_head % RTHF_Q;
        s_rthf_nseg[i] = 1;
        p_glBeginQuery(GL_TIME_ELAPSED, s_rthf_q[i][0]);
    }
}

/* Frame generation draws between a frame's records: its work is not the
 * frame's. Pause ends the frame's GPU query and starts excluding wall time
 * (and its swaps); resume opens the next query segment. */
static void rthf_pause(void) {
    if (!s_rthf_open || s_rthf_paused) return;
    s_rthf_paused = 1;
    s_rthf_pause_t0 = host_now_ns_rthf();
    s_rthf_pause_swap = s_rthf_swap_ns;
    if (s_rthf_has_q) p_glEndQuery(GL_TIME_ELAPSED);
}
static void rthf_resume(void) {
    if (!s_rthf_open || !s_rthf_paused) return;
    s_rthf_paused = 0;
    s_rthf_excl += host_now_ns_rthf() - s_rthf_pause_t0;
    s_rthf_swap_ns = s_rthf_pause_swap;   /* the generated frames' swaps */
    if (s_rthf_has_q) {
        const unsigned i = s_rthf_head % RTHF_Q;
        if (s_rthf_nseg[i] < RTHF_SEG) {
            p_glBeginQuery(GL_TIME_ELAPSED, s_rthf_q[i][s_rthf_nseg[i]++]);
        } else {
            /* Out of segments: the rest of the frame goes unmeasured. */
            s_rthf_taint = 1;
            p_glBeginQuery(GL_TIME_ELAPSED, s_rthf_q[i][RTHF_SEG - 1]);
        }
    }
}

static void rthf_end(void) {
    if (!s_rthf_open) return;
    if (s_rthf_paused) rthf_resume();
    s_rthf_open = 0;
    if (s_rthf_taint) {
        /* The query pair is still issued so the ring stays in order; the
         * frame is not published. */
        if (s_rthf_has_q) {
            p_glEndQuery(GL_TIME_ELAPSED);
            s_rthf_tail = ++s_rthf_head;   /* drop it and anything older */
        }
        atomic_fetch_add(&s_rthf_dropped, 1);
        return;
    }
    const uint64_t now = host_now_ns_rthf();
    const uint64_t idle = rt_render_idle_ns() - s_rthf_idle0;
    const uint64_t swap = s_rthf_swap_ns - s_rthf_swap0;
    const uint64_t span = now - s_rthf_t0;
    const uint64_t skip = idle + swap + s_rthf_excl;
    const uint64_t cpu = span > skip ? span - skip : 0;
    if (s_rthf_has_q) {
        const unsigned i = s_rthf_head % RTHF_Q;
        p_glEndQuery(GL_TIME_ELAPSED);
        s_rthf_slot_cpu[i] = cpu;
        s_rthf_head++;
    } else {
        rthf_publish(cpu, 0, 0);
    }
    if (s_rthf_qok > 0) rthf_poll();
}

void gl_renderer_render_thread_measure(int on) { atomic_store(&s_rthf_want, on ? 1 : 0); }
static int rthf_owns_timer(void) {
    return s_rth_on && (atomic_load_explicit(&s_rthf_want, memory_order_relaxed) || s_fg_on) &&
           s_rthf_qok > 0;
}

void gl_renderer_render_thread_costs(GlRthCosts *out) {
    if (!out) return;
    out->frames = atomic_load(&s_rthf_frames);
    out->cost_ns = atomic_load(&s_rthf_cost_ns);
    out->cpu_ns = atomic_load(&s_rthf_cpu_ns);
    out->gpu_ns = atomic_load(&s_rthf_gpu_ns);
    out->gpu_frames = atomic_load(&s_rthf_gpu_frames);
    out->dropped = atomic_load(&s_rthf_dropped);
}

/* ---- frame generation ([video] frame_generation) ---------------------------
 * docs/FRAME_GENERATION.md. Render thread only. Each game frame's draw list
 * (the replayed records between two display flips) is kept; with the last
 * two, the render thread draws in-between frames at the display's refresh
 * from data alone: the newer list redrawn into separate surfaces with each
 * matched triangle moved part of the way back to where the older frame had
 * it (frame_gen.c matches them). Unmatched, 2D and HUD primitives, fills and
 * every other record draw as the newer frame has them. Nothing here writes
 * the real surfaces, VRAM or guest state: the real frames are the same
 * images, presented in the same order, one game frame later at most (the
 * delay interpolation needs: the in-between frames before a real frame are
 * shown in the time it would have been shown alone). */
#include "frame_gen.h"

/* Captured record: header + payload copy. */
typedef struct { uint16_t op, flags; uint32_t bytes; } FgRec;
typedef struct {
    int semi_en, semi_mode, mask_set, mask_check, tw[4], mod[4];
    int area[4], off[2], wide_on, wide_base, view[4], rths[3];
} FgState;
typedef struct {
    uint8_t  *buf; size_t len, cap;
    uint32_t *off; uint32_t n, ncap;          /* record offsets into buf */
    FgPrimList prims;                          /* triangles, local coords */
    int32_t  *rec2prim; uint32_t r2p_cap;      /* record -> prim index, -1 */
    FgState   start;                           /* draw state at the list start */
    int       valid, wide, disp[4];            /* closed: display rect it shows */
    int       wide_w, wide_off, scale, linear, force43;
} FgList;
static int rp_is_car(const FgList *L, uint32_t pi);
/* Raw triangle captured while recording a list (absolute coords). */
typedef struct { uint32_t rec, key0, vid[3]; float x[3], y[3], p[3][3], h[3]; int area[4]; } FgRaw;

static int       s_fg_on = 0;                 /* configured (any thread reads) */
static int       s_fg_force = 0;              /* PSX_FRAME_GEN_FORCE: tests */
static _Atomic double s_fg_refresh_hz = 0.0, s_fg_guest_hz = 59.94;
static _Atomic int s_fg_late = 0;             /* emulation thread: a late frame */
static const char *_Atomic s_fg_hold_reason = NULL;
static _Atomic uint64_t s_fg_hold_until = 0;  /* rt_now_ns: no generation before */
static FgList    s_fg_l[4];                   /* older, newer, capturing, spare */
static int       s_fg_older = 0, s_fg_newer = 1, s_fg_cur = 2;
static int       s_fg_pa = -1, s_fg_pb = -1;  /* the pair a schedule draws from */
static FgRaw    *s_fg_raw = NULL; static uint32_t s_fg_nraw = 0, s_fg_rawcap = 0;
static int       s_fg_pc_valid = 0; static int32_t s_fg_pc[6];   /* pending PRECISE */
static int       s_fg_have_last = 0, s_fg_last_dx = 0, s_fg_last_dy = 0;
/* Display buffers seen in presents (x, y, w, h), and the one the capturing
 * list draws into: a list is one game frame's drawing into one buffer, closed
 * when drawing moves to another buffer (games flip at a VBlank after they
 * start the next frame, so a flip is not a list boundary). */
static int       s_fg_buf[4][4], s_fg_nbuf = 0, s_fg_cur_buf = -1;
static int       s_fg_vblanks = 0;
/* Presents per flip. Written by the render thread, read by the emulation
 * thread (gl_renderer_frame_gen_real_share): atomic. */
static _Atomic int s_fg_flip_vb = 1;
/* The camera fit of the pair a schedule draws from and each newer vertex's
 * placement (fg_cam_fit), positions at the current phase, and the pending
 * vertex sources of the next triangle (RTH_FG_SRC: ids, integer x/y,
 * camera-space x/y/z, H). */
static FgVert   *s_fg_verts = NULL; static uint32_t s_fg_verts_cap = 0;
static float    *s_fg_px = NULL, *s_fg_py = NULL; static uint32_t s_fg_pos_cap = 0;
static FgCamFit  s_fg_fit;
static float     s_fg_margin[FG_MAX_VIEWS][4];   /* fg_cam_place, per generated frame */
static int       s_fg_reproject = 0;            /* [video] frame_generation_method: reprojection (opt-in), else redraw */
static void      rp_snapshot_list(int li, const int disp[4]);
static void fg_blit2(GLuint src, GLuint dst, int sx, int sy, int dx, int dy, int w, int h);
static int       s_fg_replay_objects_only = 0;  /* fg_replay: cars only */
static int       s_fg_reprojected_now = 0;      /* fg_compose: the wide image is complete */
static int       s_fg_src_older = 0;          /* the older frame is redrawn (moving forward) */
static uint32_t fg_main_view(const FgCamFit *f) {
    uint32_t m = 0;
    for (uint32_t i = 1; i < f->nviews; i++) if (f->v[i].sources > f->v[m].sources) m = i;
    return m;
}
static int       s_fg_src_valid = 0; static int32_t s_fg_src[21];
static uint32_t  s_fg_src_seen = 0;
static uint64_t  s_fg_rejected = 0;           /* flips the verdict refused */
static const char *s_fg_reject_why = NULL;
static FgBreaker s_fg_brk;
static int       s_fg_brk_init = 0;
static uint64_t  s_fg_bp_seen = 0, s_fg_bp_ns_seen = 0;
static double    s_fg_real_ema = 0.0;   /* seconds */
/* A generated frame's cost: cold samples discarded, a blocking estimate
 * re-probed (frame_gen.h FgCost). s_fg_fit_s: what one may cost to fit. */
static FgCost    s_fg_cost;
static int       s_fg_cost_init = 0;
static double    s_fg_fit_s = 0.0;
static FgPace    s_fg_pace;                   /* emulation thread */
static _Atomic uint64_t s_fg_last_gen_ns = 0; /* the last generated frame (rt_now_ns) */
static uint64_t  s_fg_ignored_late = 0, s_fg_ignored_bp = 0;
static const char *s_fg_hold_why = NULL;
static double    s_fg_gen_gpu_ms = 0.0, s_fg_gen_cpu_ms = 0.0;   /* the last one measured */
/* Schedule after a flip: n generated frames, then the real one. */
static int       s_fg_pending = 0, s_fg_n = 0, s_fg_k = 0;
static uint64_t  s_fg_t0 = 0, s_fg_step_ns = 0, s_fg_flip_ns = 0;
/* Any-rate presents: one global grid at the display's refresh (or its
 * panel maximum under VRR), never faster, across game frames. */
static uint64_t  s_fg_due = 0, s_fg_last_present_ns = 0;
static void fg_note_present(uint64_t now) {
    s_fg_last_present_ns = now;
    s_fg_due = fg_next_due(s_fg_due, now, s_fg_step_ns);
}
static uint16_t  s_fg_real_op = 0;
static uint8_t  *s_fg_real_p = NULL; static size_t s_fg_real_cap = 0;
/* Generated-frame surfaces (the displayed buffer's hr rect and wide rows). */
static GLuint    s_fg_hr_tex = 0, s_fg_hr_rb = 0, s_fg_hr_fbo = 0;
static int       s_fg_hr_S = 0;
static GLuint    s_fg_w_tex = 0, s_fg_w_rb = 0, s_fg_w_fbo = 0;
static int       s_fg_w_S = 0, s_fg_w_w = 0;
static int       s_fg_drawing = 0, s_fg_presenting = 0;   /* generated draws / swap */
static int       s_fg_broken = 0;             /* the context was handed away */
static int       s_fg_capture_real = 0;       /* gl_swap_with_osd keeps the image */
static GLuint    s_fg_real_tex = 0, s_fg_real_fbo = 0;
static int       s_fg_real_w = 0, s_fg_real_h = 0, s_fg_real_kept = 0;
static int       s_fg_partial = 0;            /* the capturing list is incomplete */
static GLuint    s_fg_q[4]; static int s_fg_qok = -1; static unsigned s_fg_qh = 0, s_fg_qt = 0;
static uint64_t  s_fg_q_cpu[4];
/* Statistics (render thread writes; the debug server reads, racy by design). */
/* Measured generated frames (GPU time or CPU wall, the larger) for dynamic
 * resolution's load: their summed cost and how many were measured. */
static _Atomic uint64_t s_fg_gen_cost_ns = 0, s_fg_gen_measured = 0;
/* Counts are consumed by the emulation thread's production controller, not
 * just diagnostics. The sequence publishes each measured count/cost pair
 * coherently (one RT writer); all payload fields are atomic as well. */
static _Atomic unsigned s_fg_gen_cost_seq = 0;
static _Atomic uint64_t s_fg_generated = 0, s_fg_real_presents = 0;
static uint64_t  s_fg_flips = 0,
                 s_fg_flushed = 0, s_fg_skipped_plan = 0, s_fg_dups = 0;
static int       s_fg_last_n = 0, s_fg_last_slots = 0;
static const char *s_fg_noplan_why = "";
static uint64_t s_fg_end_ahead = 0, s_fg_end_bp = 0, s_fg_end_phase = 0;
static uint64_t s_fg_np_brk = 0, s_fg_np_stale = 0, s_fg_np_held = 0, s_fg_np_room = 0;   /* why a game frame's in-betweens stopped early */   /* why the last flip planned nothing */
static double    s_fg_last_match_ms = 0.0;

static double fg_now_s(void) { return (double)rt_now_ns() * 1e-9; }

static FgCost *fg_cost(void) {
    if (!s_fg_cost_init) { fg_cost_init(&s_fg_cost, 2.0, 16.0); s_fg_cost_init = 1; }
    s_fg_cost.clamp_spikes = s_fg_reproject;
    return &s_fg_cost;
}

/* Breaker trips only count while generation is what could have caused
 * them: a generated frame in the last half second. */
static int fg_recently_generated(void) {
    const uint64_t g = atomic_load(&s_fg_last_gen_ns);
    return g && rt_now_ns() - g < 500000000ull;
}

/* Render thread: every swap's wall time. A swap waits for the compositor
 * (and on a busy WindowServer for its round trip); each generated frame adds
 * one, so the plan counts it. */
/* Written by the render thread, read by the emulation thread
 * (gl_renderer_frame_gen_real_share): atomic, as is the published copy of
 * the generated-frame cost estimate (s_fg_cost.ema, render thread only). */
static _Atomic double s_fg_swap_ema = 0.0;
static _Atomic double s_fg_cost_ema_pub = 0.0;
static void fg_note_swap(uint64_t ns) {
    const double c = (double)ns * 1e-9, e = atomic_load(&s_fg_swap_ema);
    atomic_store(&s_fg_swap_ema, e > 0.0 ? e * 0.9 + c * 0.1 : c);
}

/* A real VBlank frame's render-thread cost, for the plan: its CPU time. Its
 * GPU span is kept for the debug output only: on macOS it also counts the
 * GPU waiting for records, so with generation off it reads most of the
 * VBlank (R4 1P: 10 ms for ~3 ms of work) and would leave no room. GPU
 * overload shows as a queue that backs up instead: the breaker, and the
 * ceiling that lowers the plan by one per such trip (FgCeiling). */
static double s_fg_real_gpu_ema = 0.0;
static void fg_note_real_cost(uint64_t cpu_ns, uint64_t gpu_ns, int has_gpu) {
    const double c = (double)cpu_ns * 1e-9;
    s_fg_real_ema = s_fg_real_ema > 0.0 ? s_fg_real_ema * 0.9 + c * 0.1 : c;
    if (has_gpu) {
        const double g = (double)gpu_ns * 1e-9;
        s_fg_real_gpu_ema = s_fg_real_gpu_ema > 0.0 ? s_fg_real_gpu_ema * 0.9 + g * 0.1 : g;
    }
}

static void fg_list_clear(FgList *l) {
    l->len = 0; l->n = 0; l->valid = 0;
    fg_prims_reset(&l->prims);
}

static void fg_state_capture(FgState *st) {
    st->semi_en = s_semi_en; st->semi_mode = s_semi_mode;
    st->mask_set = s_mask_set; st->mask_check = s_mask_check;
    st->tw[0] = s_tw_mask_x; st->tw[1] = s_tw_mask_y; st->tw[2] = s_tw_off_x; st->tw[3] = s_tw_off_y;
    st->mod[0] = s_mod_r; st->mod[1] = s_mod_g; st->mod[2] = s_mod_b; st->mod[3] = s_mod_raw;
    st->area[0] = s_area_x1; st->area[1] = s_area_y1; st->area[2] = s_area_x2; st->area[3] = s_area_y2;
    st->off[0] = s_off_x; st->off[1] = s_off_y;
    st->wide_on = g_wide_cur != 0; st->wide_base = g_wide_cur_base;
    st->view[0] = view_enabled; st->view[1] = view_shift;
    st->view[2] = view_pad_left; st->view[3] = view_pad_right;
    st->rths[0] = s_rths_flat_bd; st->rths[1] = s_rths_vp_w; st->rths[2] = s_rths_bg_full;
}

/* Restore draw state without the setters' side effects (stencil rebuilds,
 * wide-surface allocation); batches were flushed by the caller. */
static void fg_state_apply(const FgState *st, GLuint wide_cur) {
    s_semi_en = st->semi_en; s_semi_mode = st->semi_mode;
    s_mask_set = st->mask_set; s_mask_check = st->mask_check;
    s_tw_mask_x = st->tw[0]; s_tw_mask_y = st->tw[1]; s_tw_off_x = st->tw[2]; s_tw_off_y = st->tw[3];
    s_mod_r = st->mod[0]; s_mod_g = st->mod[1]; s_mod_b = st->mod[2]; s_mod_raw = st->mod[3];
    s_area_x1 = st->area[0]; s_area_y1 = st->area[1]; s_area_x2 = st->area[2]; s_area_y2 = st->area[3];
    s_off_x = st->off[0]; s_off_y = st->off[1];
    g_wide_cur = wide_cur; g_wide_cur_base = st->wide_base;
    view_enabled = st->view[0]; view_shift = st->view[1];
    view_pad_left = st->view[2]; view_pad_right = st->view[3];
    s_rths_flat_bd = st->rths[0]; s_rths_vp_w = st->rths[1]; s_rths_bg_full = st->rths[2];
    s_pc_valid = 0; s_pq_valid = 0; s_projected_uv_valid = 0; s_pz_valid = 0;
}

static int fg_list_append(FgList *l, uint16_t op, uint16_t flags, const void *p, uint32_t bytes) {
    size_t need = l->len + sizeof(FgRec) + ((bytes + 3u) & ~3u);
    if (need > l->cap) {
        size_t nc = l->cap ? l->cap : (size_t)1 << 20;
        while (nc < need) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(l->buf, nc);
        if (!nb) return -1;
        l->buf = nb; l->cap = nc;
    }
    if (l->n == l->ncap) {
        uint32_t nc = l->ncap ? l->ncap * 2u : 8192u;
        uint32_t *no = (uint32_t *)realloc(l->off, (size_t)nc * sizeof *no);
        if (!no) return -1;
        l->off = no; l->ncap = nc;
    }
    FgRec h = { op, flags, bytes };
    memcpy(l->buf + l->len, &h, sizeof h);
    if (bytes) memcpy(l->buf + l->len + sizeof h, p, bytes);
    l->off[l->n] = (uint32_t)l->len;
    l->len = need;
    return (int)l->n++;
}

static int fg_raw_add(const FgRaw *r) {
    if (s_fg_nraw == s_fg_rawcap) {
        uint32_t nc = s_fg_rawcap ? s_fg_rawcap * 2u : 4096u;
        FgRaw *nr = (FgRaw *)realloc(s_fg_raw, (size_t)nc * sizeof *nr);
        if (!nr) return 0;
        s_fg_raw = nr; s_fg_rawcap = nc;
    }
    s_fg_raw[s_fg_nraw++] = *r;
    return 1;
}

/* Which records a generated frame redraws: draw state and drawing, never
 * transfers, copies, presents, steps or diagnostics. */
static int fg_op_kept(uint16_t op) {
    switch (op) {
    case RTH_SEMI: case RTH_MASK: case RTH_TWIN: case RTH_MOD: case RTH_PRECISE:
    case RTH_PERSP: case RTH_DEPTH: case RTH_DITHER: case RTH_FILL: case RTH_FLAT_TRI: case RTH_GOURAUD_TRI:
    case RTH_TEX_TRI: case RTH_SHADED_TEX_TRI: case RTH_FLAT_RECT: case RTH_TEX_RECT:
    case RTH_TEX_RECT_SCALED: case RTH_LINE: case RTH_SHADED_LINE: case RTH_AREA:
    case RTH_OFFSET: case RTH_WIDE_VIEW: case RTH_WIDE_TARGET: case RTH_WIDE_DISABLE:
    case RTH_WIDE_CLEAR: case RTH_WIDE_CLEAR_MARGINS: case RTH_PROJ_TRI: case RTH_STATE:
        return 1;
    default:
        return 0;
    }
}

/* Positions of a triangle record (absolute native px) and its key words. */
static int fg_tri_geom(uint16_t op, const void *payload, const int32_t *pc,
                       float x[3], float y[3], uint32_t *key) {
    const int32_t *v = (const int32_t *)payload;
    int xi[3], k = 0;
    int32_t kw[12];
    switch (op) {
    case RTH_FLAT_TRI:
        for (int i = 0; i < 3; i++) { x[i] = (float)v[2 * i]; y[i] = (float)v[2 * i + 1]; }
        kw[k++] = op; kw[k++] = v[6];
        break;
    case RTH_GOURAUD_TRI:
        for (int i = 0; i < 3; i++) { x[i] = (float)v[3 * i]; y[i] = (float)v[3 * i + 1]; }
        kw[k++] = op;
        break;
    case RTH_TEX_TRI:
        for (int i = 0; i < 3; i++) {
            x[i] = (float)v[4 * i]; y[i] = (float)v[4 * i + 1];
            kw[k++] = v[4 * i + 2] | (v[4 * i + 3] << 16);
        }
        kw[k++] = op; kw[k++] = v[12]; kw[k++] = v[13]; kw[k++] = v[14];
        break;
    case RTH_SHADED_TEX_TRI:
        for (int i = 0; i < 3; i++) {
            x[i] = (float)v[5 * i]; y[i] = (float)v[5 * i + 1];
            kw[k++] = v[5 * i + 2] | (v[5 * i + 3] << 16);
        }
        kw[k++] = op; kw[k++] = v[15]; kw[k++] = v[16]; kw[k++] = v[17]; kw[k++] = v[18];
        break;
    case RTH_PROJ_TRI: {
        PSXProjectedVertex vx[3];
        memcpy(vx, payload, sizeof vx);
        const int32_t *a = (const int32_t *)((const uint8_t *)payload + sizeof vx);
        for (int i = 0; i < 3; i++) {
            x[i] = (float)vx[i].x; y[i] = (float)vx[i].y;
            kw[k++] = (int32_t)floor(vx[i].u * 16.0) | ((int32_t)floor(vx[i].v * 16.0) << 16);
        }
        kw[k++] = op; kw[k++] = a[0]; kw[k++] = a[1]; kw[k++] = a[2]; kw[k++] = a[3];
        pc = NULL;
        break;
    }
    default:
        return 0;
    }
    (void)xi;
    if (pc)   /* a sub-pixel position set for this triangle (16.16) */
        for (int i = 0; i < 3; i++) {
            x[i] = (float)pc[2 * i] / 65536.0f;
            y[i] = (float)pc[2 * i + 1] / 65536.0f;
        }
    *key = fg_hash(FG_HASH_INIT, kw, k);
    return 1;
}

static int fg_buf_of(int x, int y) {
    for (int i = 0; i < s_fg_nbuf; i++)
        if (x >= s_fg_buf[i][0] && x < s_fg_buf[i][0] + s_fg_buf[i][2] &&
            y >= s_fg_buf[i][1] && y < s_fg_buf[i][1] + s_fg_buf[i][3]) return i;
    return -1;
}
static void fg_list_close(FgList *l, const int disp[4]);
static void fg_list_open(void);

/* Reprojection's source: each closed list's finished image, copied from the
 * real surface when the list closes (the game then draws its next frames
 * into the buffers; R4 even flips to a buffer before drawing into it). */
static GLuint s_rp_ls_tex[2], s_rp_ls_fbo[2];   /* the two newest closed lists */
static int    s_rp_ls_w[2], s_rp_ls_h[2], s_rp_ls_S[2], s_rp_ls_li[2] = { -1, -1 }, s_rp_ls_new = 0;
static uint8_t *s_rp_needle = NULL; static uint32_t s_rp_needle_cap = 0, s_rp_needle_n = 0;
static int rp_snap_slot(int li) {
    if (li < 0) return -1;
    for (int k = 0; k < 2; k++) if (s_rp_ls_li[k] == li) return k;
    return -1;
}
static void rp_snapshot_list(int li, const int disp[4]) {
    if (li < 0 || li >= 4 || !s_fg_reproject || !s_raster_ok || !s_fg_on) return;
    const int k = s_rp_ls_new ^ 1;
    s_rp_ls_li[k] = -1;
    const int S = s_hr_scale;
    int iw = -1;
    if (g_wide_w > 0)
        for (int i = 0; i < WIDE_MAX_SURF; i++)
            if (s_wide_fbo[i] && s_wide_base[i] == disp[0]) { iw = i; break; }
    const int dy = disp[1] < 0 ? 0 : disp[1];
    const int dh = disp[1] + disp[3] > VRAM_H ? VRAM_H - dy : disp[1] + disp[3] - dy;
    const int W = iw >= 0 ? g_wide_w : disp[2];
    if (W <= 0 || dh <= 0) return;
    const int w = W * S, h = dh * S;
    if (s_rp_ls_w[k] != w || s_rp_ls_h[k] != h) {
        if (s_rp_ls_fbo[k]) { p_glDeleteFramebuffers(1, &s_rp_ls_fbo[k]); glDeleteTextures(1, &s_rp_ls_tex[k]); }
        s_rp_ls_fbo[k] = s_rp_ls_tex[k] = 0; s_rp_ls_w[k] = s_rp_ls_h[k] = 0;
        fg_cost_cold(fg_cost(), 3);
        s_rp_ls_tex[k] = make_tex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
        if (!make_fbo(&s_rp_ls_fbo[k], s_rp_ls_tex[k], 0)) {
            glDeleteTextures(1, &s_rp_ls_tex[k]); s_rp_ls_tex[k] = 0; s_rp_ls_fbo[k] = 0; return;
        }
        s_rp_ls_w[k] = w; s_rp_ls_h[k] = h;
    }
    flush_line_batch(); flush_flat_batch(); flush_tex_batch();
    fg_blit2(iw >= 0 ? s_wide_fbo[iw] : s_hr_fbo, s_rp_ls_fbo[k],
             iw >= 0 ? 0 : disp[0] * S, dy * S, 0, 0, w, h);
    /* The wide surface's centre may be filled from the hr surface only at
     * present (wide_blit_center's fast path): take it from there too. */
    if (iw >= 0 && wide_fast_center_valid() && !view_enabled) {
        if (s_hiw) return;
        const int native_w = g_wide_w - 2 * g_wide_off;
        if (native_w > 0)
            fg_blit2(s_hr_fbo, s_rp_ls_fbo[k], disp[0] * S, dy * S, g_wide_off * S, 0,
                     native_w * S, h);
    }
    s_rp_ls_S[k] = S;
    s_rp_ls_li[k] = li;
    s_rp_ls_new = k;
}

/* Close the capturing list as the frame of its buffer; it becomes the newer
 * frame, the newer one the older. */
static void fg_rotate(void) {
    fg_list_close(&s_fg_l[s_fg_cur], s_fg_buf[s_fg_cur_buf]);
    if (s_fg_partial) { s_fg_l[s_fg_cur].valid = 0; s_fg_partial = 0; }
    s_fg_older = s_fg_newer; s_fg_newer = s_fg_cur;
    /* A slot no frame in use holds (a schedule pins its pair). */
    int free_slot = -1;
    for (int k = 0; k < 4 && free_slot < 0; k++)
        if (k != s_fg_older && k != s_fg_newer &&
            !(s_fg_pending && (k == s_fg_pa || k == s_fg_pb))) free_slot = k;
    if (free_slot < 0) {
        fg_flush();
        for (int k = 0; k < 4 && free_slot < 0; k++)
            if (k != s_fg_older && k != s_fg_newer) free_slot = k;
    }
    s_fg_cur = free_slot;
    fg_list_open();
}

/* Called for every replayed record (gl_rth_exec), before it executes. */
static void fg_capture(const RtCmd *c, const void *payload) {
    if (c->op == RTH_FG_SRC) {
        memcpy(s_fg_src, payload, sizeof s_fg_src);
        s_fg_src_valid = 1;
        s_fg_src_seen++;
        return;
    }
    if (!fg_op_kept(c->op)) return;
    /* Which buffer a draw goes to: a fill's rect, else the draw area. */
    int tb = -1;
    switch (c->op) {
    case RTH_FILL: {
        const int32_t *v = (const int32_t *)payload;
        tb = fg_buf_of(v[0], v[1]);
        break;
    }
    case RTH_FLAT_TRI: case RTH_GOURAUD_TRI: case RTH_TEX_TRI: case RTH_SHADED_TEX_TRI:
    case RTH_PROJ_TRI: case RTH_FLAT_RECT: case RTH_TEX_RECT: case RTH_TEX_RECT_SCALED:
    case RTH_LINE: case RTH_SHADED_LINE:
        tb = fg_buf_of(s_area_x1, s_area_y1);
        break;
    default: break;
    }
    if (tb >= 0 && tb != s_fg_cur_buf) {
        if (s_fg_cur_buf >= 0) {
            fg_rotate();   /* drawing moved on: the list so far is that buffer's frame */
        } else {
            /* The first buffer seen: what was captured belongs to no frame. */
            fg_list_open();
            s_fg_partial = 0;
        }
        s_fg_cur_buf = tb;
    }
    FgList *l = &s_fg_l[s_fg_cur];
    int r = fg_list_append(l, c->op, c->flags, payload, c->payload);
    if (r < 0) return;
    if (c->op == RTH_PRECISE) {
        const int32_t *v = (const int32_t *)payload;
        s_fg_pc_valid = v[0] != 0;
        if (s_fg_pc_valid) memcpy(s_fg_pc, v + 1, sizeof s_fg_pc);
        return;
    }
    FgRaw raw;
    if (fg_tri_geom(c->op, payload, s_fg_pc_valid ? s_fg_pc : NULL, raw.x, raw.y, &raw.key0)) {
        raw.rec = (uint32_t)r;
        /* The identities belong to this triangle only if it is drawn where
         * they were looked up (a culled triangle leaves its record behind). */
        for (int k = 0; k < 3; k++) raw.vid[k] = 0;
        if (s_fg_src_valid) {
            int at = 1;
            for (int k = 0; k < 3; k++)
                if (fabsf(raw.x[k] - (float)s_fg_src[3 + 2 * k]) > 1.5f ||
                    fabsf(raw.y[k] - (float)s_fg_src[4 + 2 * k]) > 1.5f) at = 0;
            if (at) for (int k = 0; k < 3; k++) {
                raw.vid[k] = (uint32_t)s_fg_src[k];
                for (int c = 0; c < 3; c++) raw.p[k][c] = (float)s_fg_src[9 + 3 * k + c];
                raw.h[k] = (float)s_fg_src[18 + k];
            }
        }
        raw.area[0] = s_area_x1; raw.area[1] = s_area_y1;
        raw.area[2] = s_area_x2; raw.area[3] = s_area_y2;
        (void)fg_raw_add(&raw);
    }
    switch (c->op) {   /* a precise override describes one triangle */
    case RTH_FLAT_TRI: case RTH_GOURAUD_TRI: case RTH_TEX_TRI:
    case RTH_SHADED_TEX_TRI: case RTH_PROJ_TRI:
        s_fg_pc_valid = 0;
        s_fg_src_valid = 0;
        break;
    default: break;
    }
}

static int fg_area_meets(const int *area, const int *disp) {
    return area[2] >= disp[0] && area[0] < disp[0] + disp[2] &&
           area[3] >= disp[1] && area[1] < disp[1] + disp[3];
}

/* Close the capturing list as the frame of buffer disp (x, y, w, h). */
static void fg_list_close(FgList *l, const int disp[4]) {
    l->valid = 1;
    memcpy(l->disp, disp, sizeof l->disp);
    l->wide_w = g_wide_w; l->wide_off = g_wide_off; l->scale = s_out_scale;
    if (l->r2p_cap < l->n) {
        int32_t *nr = (int32_t *)realloc(l->rec2prim, (size_t)l->n * sizeof *nr);
        if (!nr) { l->valid = 0; return; }
        l->rec2prim = nr; l->r2p_cap = l->n;
    }
    for (uint32_t i = 0; i < l->n; i++) l->rec2prim[i] = -1;
    fg_prims_reset(&l->prims);
    for (uint32_t i = 0; i < s_fg_nraw; i++) {
        const FgRaw *r = &s_fg_raw[i];
        if (!fg_area_meets(r->area, disp)) continue;
        FgPrim p;
        p.rec = r->rec;
        const int32_t aw[4] = { r->area[0] - disp[0], r->area[1] - disp[1],
                                r->area[2] - disp[0], r->area[3] - disp[1] };
        p.key = fg_hash(r->key0, aw, 4);
        p.view = fg_hash(FG_HASH_INIT, aw, 4);
        for (int k = 0; k < 4; k++) p.area[k] = (float)aw[k];
        for (int k = 0; k < 3; k++) {
            p.vid[k] = r->vid[k];
            memcpy(p.p[k], r->p[k], sizeof p.p[k]);
            p.h[k] = r->h[k];
        }
        for (int k = 0; k < 3; k++) { p.x[k] = r->x[k] - (float)disp[0]; p.y[k] = r->y[k] - (float)disp[1]; }
        if (!fg_prims_add(&l->prims, &p)) { l->valid = 0; return; }
        l->rec2prim[r->rec] = (int32_t)(l->prims.n - 1);
    }
}

static void fg_list_open(void) {
    FgList *l = &s_fg_l[s_fg_cur];
    fg_list_clear(l);
    fg_state_capture(&l->start);
    s_fg_nraw = 0;
    s_fg_pc_valid = 0;
    s_fg_src_valid = 0;
}

static void fg_invalidate(void) {
    for (int i = 0; i < 4; i++) s_fg_l[i].valid = 0;
    s_fg_have_last = 0;
    s_fg_cur_buf = -1;
    s_fg_partial = 0;
    s_fg_broken = 0;
    fg_list_open();
}

/* A new surface's first touch (the driver backing its storage) happens here,
 * when it is allocated, not inside the first generated frame; that frame's
 * measured cost is discarded as well (cold). */
static void fg_surface_warm(GLuint fbo) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClearStencil(0);
    glClearDepth(1.0);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glFlush();
    fg_cost_cold(fg_cost(), 2);
}

/* Surfaces for the generated image, at the current level. */
static int fg_surfaces_ensure(int wide) {
    const int S = s_hr_scale;
    if (s_fg_hr_S != S) {
        if (s_fg_hr_fbo) {
            p_glDeleteFramebuffers(1, &s_fg_hr_fbo); glDeleteTextures(1, &s_fg_hr_tex);
            p_glDeleteRenderbuffers(1, &s_fg_hr_rb);
            s_fg_hr_fbo = s_fg_hr_tex = s_fg_hr_rb = 0;
        }
        s_fg_hr_S = 0;
        if (!hr_alloc_surface(S, &s_fg_hr_tex, &s_fg_hr_rb, &s_fg_hr_fbo)) return 0;
        s_fg_hr_S = S;
        fg_surface_warm(s_fg_hr_fbo);
    }
    if (wide && (s_fg_w_S != S || s_fg_w_w != g_wide_w)) {
        if (s_fg_w_fbo) {
            p_glDeleteFramebuffers(1, &s_fg_w_fbo); glDeleteTextures(1, &s_fg_w_tex);
            p_glDeleteRenderbuffers(1, &s_fg_w_rb);
            s_fg_w_fbo = s_fg_w_tex = s_fg_w_rb = 0;
        }
        s_fg_w_S = 0;
        if (!wide_alloc_surface(S, &s_fg_w_tex, &s_fg_w_rb, &s_fg_w_fbo)) return 0;
        s_fg_w_S = S; s_fg_w_w = g_wide_w;
        fg_surface_warm(s_fg_w_fbo);
    }
    return 1;
}

static void fg_surfaces_free(void) {
    if (s_fg_hr_fbo) { p_glDeleteFramebuffers(1, &s_fg_hr_fbo); glDeleteTextures(1, &s_fg_hr_tex);
                       p_glDeleteRenderbuffers(1, &s_fg_hr_rb); }
    if (s_fg_w_fbo) { p_glDeleteFramebuffers(1, &s_fg_w_fbo); glDeleteTextures(1, &s_fg_w_tex);
                      p_glDeleteRenderbuffers(1, &s_fg_w_rb); }
    s_fg_hr_fbo = s_fg_hr_tex = s_fg_hr_rb = 0; s_fg_hr_S = 0;
    s_fg_w_fbo = s_fg_w_tex = s_fg_w_rb = 0; s_fg_w_S = 0; s_fg_w_w = 0;
    if (s_fg_real_fbo) { p_glDeleteFramebuffers(1, &s_fg_real_fbo); glDeleteTextures(1, &s_fg_real_tex); }
    s_fg_real_fbo = s_fg_real_tex = 0; s_fg_real_w = s_fg_real_h = 0; s_fg_real_kept = 0;
}

/* Colour only: a depth-stencil blit costs the CPU milliseconds on macOS
 * (most of a generated frame at high levels). The stencil is the mask-bit
 * mirror of alpha; the caller marks it stale and it is rebuilt from the
 * copied alpha only if a generated draw checks the mask. */
static void fg_blit2(GLuint src, GLuint dst, int sx, int sy, int dx, int dy, int w, int h) {
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, src);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, dst);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}
static void fg_blit(GLuint src, GLuint dst, int x, int y, int w, int h) {
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, src);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, dst);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(x, y, x + w, y + h, x, y, x + w, y + h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
}

static void fg_draw_tri_at(uint16_t op, const int32_t *v, const void *payload,
                           const float x[3], const float y[3]) {
    int32_t w[19];
    int xi[3], yi[3], integral = 1;
    for (int i = 0; i < 3; i++) {
        xi[i] = (int)floorf(x[i] + 0.5f); yi[i] = (int)floorf(y[i] + 0.5f);
        if ((float)xi[i] != x[i] || (float)yi[i] != y[i]) integral = 0;
    }
    if (op == RTH_PROJ_TRI) {
        PSXProjectedVertex vx[3];
        memcpy(vx, payload, sizeof vx);
        const int32_t *a = (const int32_t *)((const uint8_t *)payload + sizeof vx);
        for (int i = 0; i < 3; i++) { vx[i].x = x[i]; vx[i].y = y[i]; }
        gl_renderer_draw_projected_triangle(vx, (uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2],
                                            a[3], a[4], a[5]);
        return;
    }
    /* A PGXP depth triangle (s_pz_valid) is 3D only with sub-pixel
     * positions, as in the real frame: keep them even when integral. */
    if (!integral || s_pz_valid) {
        glb_set_precise_triangle(1, (int32_t)lrintf(x[0] * 65536.0f), (int32_t)lrintf(y[0] * 65536.0f),
                                 (int32_t)lrintf(x[1] * 65536.0f), (int32_t)lrintf(y[1] * 65536.0f),
                                 (int32_t)lrintf(x[2] * 65536.0f), (int32_t)lrintf(y[2] * 65536.0f));
    }
    switch (op) {
    case RTH_FLAT_TRI:
        glb_draw_flat_triangle(xi[0], yi[0], xi[1], yi[1], xi[2], yi[2], (uint16_t)v[6]);
        break;
    case RTH_GOURAUD_TRI:
        glb_draw_gouraud_triangle(xi[0], yi[0], (uint16_t)v[2], xi[1], yi[1], (uint16_t)v[5],
                                  xi[2], yi[2], (uint16_t)v[8]);
        break;
    case RTH_TEX_TRI:
        memcpy(w, v, 15 * sizeof *w);
        for (int i = 0; i < 3; i++) { w[4 * i] = xi[i]; w[4 * i + 1] = yi[i]; }
        glb_draw_textured_triangle(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                                   w[8], w[9], w[10], w[11],
                                   (uint16_t)w[12], (uint16_t)w[13], (uint16_t)w[14]);
        break;
    case RTH_SHADED_TEX_TRI:
        memcpy(w, v, 19 * sizeof *w);
        for (int i = 0; i < 3; i++) { w[5 * i] = xi[i]; w[5 * i + 1] = yi[i]; }
        glb_draw_shaded_textured_triangle(w[0], w[1], w[2], w[3], (uint32_t)w[4],
                                          w[5], w[6], w[7], w[8], (uint32_t)w[9],
                                          w[10], w[11], w[12], w[13], (uint32_t)w[14],
                                          (uint16_t)w[15], (uint16_t)w[16], (uint16_t)w[17], w[18]);
        break;
    default: break;
    }
}

static int fg_rect_meets(int x, int y, int w, int h, const int *disp) {
    return x < disp[0] + disp[2] && x + w > disp[0] && y < disp[1] + disp[3] && y + h > disp[1];
}

/* Redraw list b at phase t (0 = a's positions, 1 = b's) into the surfaces
 * currently bound as hr / the displayed wide surface. */
static void fg_replay(const FgList *a, const FgList *b, double t, GLuint gen_wide) {
    /* b: the redrawn frame; a: the other one (its HUD motion only). */
    const int *disp = b->disp;
    /* Every vertex's position at this phase (the newer frame from an
     * in-between camera, frame_gen.h). */
    int placed = 0;
    memset(s_fg_margin, 0, sizeof s_fg_margin);
    if (s_fg_replay_objects_only) {
        /* Reprojected: only cars are drawn, each corner at its own
         * in-between position (fg_cam_place's object motion), computed
         * per drawn triangle below; and the HUD needles, moved like
         * fg_hud_lerp moves them (the restore put the older frame's dial
         * under the newer frame's needle). */
        if (a && s_rp_needle && s_rp_needle_n == b->prims.n) {
            if (s_fg_pos_cap < b->prims.n) {
                float *nx = (float *)realloc(s_fg_px, (size_t)b->prims.n * 3 * sizeof *nx);
                if (nx) s_fg_px = nx;
                float *ny = (float *)realloc(s_fg_py, (size_t)b->prims.n * 3 * sizeof *ny);
                if (ny) s_fg_py = ny;
                if (nx && ny) s_fg_pos_cap = b->prims.n;
            }
            if (s_fg_pos_cap >= b->prims.n) {
                for (uint32_t j = 0; j < b->prims.n; j++)
                    for (int k = 0; k < 3; k++) {
                        s_fg_px[3 * j + k] = b->prims.v[j].x[k];
                        s_fg_py[3 * j + k] = b->prims.v[j].y[k];
                    }
                fg_hud_lerp(&b->prims, &a->prims, 1.0 - t, s_fg_px, s_fg_py, 24.0f);
                placed = 2;
            }
        }
    } else if (s_fg_verts && s_fg_verts_cap >= b->prims.n) {
        if (s_fg_pos_cap < b->prims.n) {
            float *nx = (float *)realloc(s_fg_px, (size_t)b->prims.n * 3 * sizeof *nx);
            if (nx) s_fg_px = nx;
            float *ny = (float *)realloc(s_fg_py, (size_t)b->prims.n * 3 * sizeof *ny);
            if (ny) s_fg_py = ny;
            if (nx && ny) s_fg_pos_cap = b->prims.n;
        }
        if (s_fg_pos_cap >= b->prims.n) {
            fg_cam_place(&b->prims, &s_fg_fit, s_fg_verts, t, s_fg_px, s_fg_py, s_fg_margin);
            if (a) fg_hud_lerp(&b->prims, &a->prims, 1.0 - t, s_fg_px, s_fg_py, 24.0f);
            placed = 1;
        }
    }
    const float ddx = (float)(b->disp[0]), ddy = (float)(b->disp[1]);
    fg_state_apply(&b->start, (b->start.wide_on && b->start.wide_base == disp[0]) ? gen_wide : 0);
    int pc_pending = 0, drawn = 0; int32_t pc[7];
    int seen_world = 0;
    for (uint32_t r = 0; r < b->n; r++) {
        FgRec h;
        memcpy(&h, b->buf + b->off[r], sizeof h);
        const void *pl = b->buf + b->off[r] + sizeof h;
        const int32_t *v = (const int32_t *)pl;
        s_rthx_prim = (h.flags & RTHF_PRIM) != 0;
        s_rthx_tagged = (h.flags & RTHF_TAGGED) != 0;
        s_rthx_backdrop = (h.flags >> RTHF_BD_SHIFT) & 3;
        const int area[4] = { s_area_x1, s_area_y1, s_area_x2, s_area_y2 };
        const int in = !s_fg_replay_objects_only && fg_area_meets(area, disp);
        switch (h.op) {
        case RTH_SEMI:    glb_set_semi_transparency(v[0], v[1]); break;
        case RTH_DITHER:  glb_set_dither(v[0]); break;
        case RTH_MASK:    glb_set_mask_bits(v[0], v[1]); break;
        case RTH_TWIN:    glb_set_texture_window((uint32_t)v[0]); break;
        case RTH_MOD:     glb_set_color_modulation(v[0], v[1], v[2], v[3]); break;
        case RTH_PRECISE: pc_pending = v[0] != 0; memcpy(pc, v, sizeof pc); break;
        case RTH_PERSP: {
            float q[3];
            memcpy(q, &v[1], sizeof q);
            glb_set_perspective_triangle(v[0], q[0], q[1], q[2]);
            break;
        }
        case RTH_DEPTH: {   /* PGXP depth (G1.14): as the real frame */
            float z[3];
            memcpy(z, &v[1], sizeof z);
            glb_set_depth_triangle(v[0], z[0], z[1], z[2]);
            break;
        }
        case RTH_AREA:    glb_set_draw_area(v[0], v[1], v[2], v[3]); break;
        case RTH_OFFSET:  glb_set_draw_offset(v[0], v[1]); break;
        case RTH_STATE:   s_rths_flat_bd = v[0]; s_rths_vp_w = v[1]; s_rths_bg_full = v[2]; break;
        case RTH_WIDE_VIEW: glb_wide_set_view(v[0], v[1], v[2], v[3]); break;
        case RTH_WIDE_TARGET:
            flush_flat_batch(); flush_tex_batch();
            g_wide_cur = (v[0] == disp[0]) ? gen_wide : 0;
            g_wide_cur_base = v[0];
            break;
        case RTH_WIDE_DISABLE: glb_wide_disable_target(); break;
        case RTH_WIDE_CLEAR:
            if (v[0] == disp[0] && gen_wide && !s_fg_replay_objects_only) glb_wide_clear(v[0], v[1], v[2], (uint16_t)v[3]);
            break;
        case RTH_WIDE_CLEAR_MARGINS:
            if (v[0] == disp[0] && gen_wide && !s_fg_replay_objects_only)
                glb_wide_clear_margins(v[0], v[1], v[2], (uint16_t)v[3], v[4]);
            break;
        case RTH_FILL:
            /* The clears before anything is drawn are not redrawn: the
             * surface starts as the newer real frame, so what the in-between
             * camera uncovers (the road below the receding near edge) shows
             * that frame's pixels instead of the clear colour. Fills after
             * drawing (panels, letterbox bands) are part of the picture. */
            if (drawn && !s_fg_replay_objects_only && fg_rect_meets(v[0], v[1], v[2], v[3], disp))
                glb_fill_rect(v[0], v[1], v[2], v[3], (uint16_t)v[4]);
            break;
        case RTH_FLAT_RECT:
            drawn |= in;
            if (in) glb_draw_flat_rect(v[0], v[1], v[2], v[3], (uint16_t)v[4]);
            break;
        case RTH_TEX_RECT:
            drawn |= in;
            if (in) glb_draw_textured_rect(v[0], v[1], v[2], v[3], v[4], v[5],
                                           (uint16_t)v[6], (uint16_t)v[7], (uint16_t)v[8]);
            break;
        case RTH_TEX_RECT_SCALED:
            drawn |= in;
            if (in) glb_draw_textured_rect_scaled(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                                                  (uint16_t)v[8], (uint16_t)v[9], (uint16_t)v[10]);
            break;
        case RTH_LINE:
            drawn |= in;
            if (in) glb_draw_line(v[0], v[1], v[2], v[3], (uint16_t)v[4]);
            break;
        case RTH_SHADED_LINE:
            drawn |= in;
            if (in) glb_draw_shaded_line(v[0], v[1], (uint16_t)v[2], v[3], v[4], (uint16_t)v[5]);
            break;
        case RTH_FLAT_TRI: case RTH_GOURAUD_TRI: case RTH_TEX_TRI:
        case RTH_SHADED_TEX_TRI: case RTH_PROJ_TRI: {
            const int pi = b->rec2prim ? b->rec2prim[r] : -1;
            if (pi < 0) {   /* outside the display: drawn into VRAM already */
                pc_pending = 0;
                precise_consumed();
                break;
            }
            const FgPrim *pb = &b->prims.v[pi];
            float x[3], y[3];
            const int needle = s_fg_replay_objects_only && placed == 2 && s_rp_needle[pi];
            if (s_fg_replay_objects_only && !needle) {
                /* Before its view's first world triangle: the backdrop (a sky
                 * dome moves as an object too), which the warp keeps. */
                int vw = -1;
                for (int k = 0; k < 3 && vw < 0; k++) vw = s_fg_verts[3 * pi + k].view;
                const int vk = vw >= 0 && vw < FG_MAX_VIEWS ? vw : FG_MAX_VIEWS;
                if (!(seen_world & (1 << vk))) {
                    for (int k = 0; k < 3; k++)
                        if (s_fg_verts[3 * pi + k].mode == FG_PLACE_CAMERA) seen_world |= 1 << vk;
                    pc_pending = 0;
                    break;
                }
            }
            if (s_fg_replay_objects_only && !needle) {
                /* Cars: every corner moves as an object, and small (a sky
                 * dome is an object to the fit too, but it is the backdrop
                 * the warp keeps). */
                /* Cars are textured; an untextured gradient is backdrop. */
                int obj = (h.op == RTH_TEX_TRI || h.op == RTH_SHADED_TEX_TRI || h.op == RTH_PROJ_TRI) &&
                          rp_is_car(b, (uint32_t)pi);
                if (obj) {
                    const float bw = fmaxf(pb->x[0], fmaxf(pb->x[1], pb->x[2])) - fminf(pb->x[0], fminf(pb->x[1], pb->x[2]));
                    const float bh = fmaxf(pb->y[0], fmaxf(pb->y[1], pb->y[2])) - fminf(pb->y[0], fminf(pb->y[1], pb->y[2]));
                    if (bw > 96.0f || bh > 64.0f) obj = 0;
                }
                if (!obj) { pc_pending = 0; break; }
            }
            if (needle) {
                for (int k = 0; k < 3; k++) { x[k] = s_fg_px[3 * pi + k]; y[k] = s_fg_py[3 * pi + k]; }
            } else if (s_fg_replay_objects_only) {
                int gone = 0;
                for (int k = 0; k < 3; k++) {
                    const FgVert *fv = &s_fg_verts[3 * pi + k];
                    const float *pn = pb->p[k];
                    float pt[3];
                    for (int c = 0; c < 3; c++) pt[c] = fv->a[c] + (pn[c] - fv->a[c]) * (float)t;
                    if (pn[2] < 1.0f || pt[2] < 1.0f) { gone = 1; break; }
                    x[k] = pb->x[k] + pb->h[k] * (pt[0] / pt[2] - pn[0] / pn[2]);
                    y[k] = pb->y[k] + pb->h[k] * (pt[1] / pt[2] - pn[1] / pn[2]);
                }
                if (gone) { pc_pending = 0; break; }
            } else if (placed) {
                int gone = 0;
                for (int k = 0; k < 3; k++) {
                    x[k] = s_fg_px[3 * pi + k]; y[k] = s_fg_py[3 * pi + k];
                    gone |= isnan(x[k]);
                }
                if (gone) {   /* crossed the camera plane: not in this frame */
                    pc_pending = 0;
                    precise_consumed();
                    break;
                }
            } else {
                for (int k = 0; k < 3; k++) { x[k] = pb->x[k]; y[k] = pb->y[k]; }
            }
            for (int k = 0; k < 3; k++) { x[k] += ddx; y[k] += ddy; }
            drawn = 1;
            fg_draw_tri_at(h.op, v, pl, x, y);
            precise_consumed();
            pc_pending = 0;
            break;
        }
        default: break;
        }
        (void)pc_pending;
    }
    s_rthx_prim = 0;
    flush_line_batch();
    flush_flat_batch();
    flush_tex_batch();
    hiw_flush_queue();
}

/* The generated image composed like a real present (no hold-last, no
 * interpolation capture, no present bookkeeping), then swapped. */
static void fg_compose(const FgList *b, GLuint wide_fbo, GLuint wide_tex, int linear,
                       int force_4_3) {
    int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    int lx, ly, lw, lh;
    if (b->wide) {
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
        /* A reprojected frame warped the composed wide image itself. */
        if (!s_fg_reprojected_now) wide_blit_center(wide_fbo, b->disp[0], b->disp[1], b->disp[3]);
    } else if (force_4_3) {
        letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    } else {
        letterbox_rect(ww, wh, &lx, &ly, &lw, &lh);
    }
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    if (lx != 0 || ly != 0 || lw != ww || lh != wh) {
        glClearColor(0.f, 0.f, 0.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    if (b->wide) {
        present_target_quad(wide_tex, (float)g_wide_w, (float)VRAM_H, 0, b->disp[1], g_wide_w,
                            b->disp[3], linear, lx, ly, lw, lh, 1, 1, s_out_scale);
    } else {
        present_bezel(ww, wh, lx, ly, lw, lh);
        present_target_quad(s_hr_tex, (float)VRAM_W, (float)VRAM_H, b->disp[0], b->disp[1],
                            b->disp[2], b->disp[3], linear, lx, ly, lw, lh, 1, 1, s_out_scale);
    }
}

/* Draw and present the frame at phase t between the two lists. Returns 1
 * when a frame was presented. `swap` 0 leaves the composed image in the
 * back buffer (tests). */

/* ---- frame generation: reprojection (opt-in) ------------------------------
 * An in-between frame warps the newer real frame's finished image by the
 * in-between camera instead of drawing the scene again. Once per game frame
 * a small image of the newer frame's camera depth is rasterized from its
 * recorded triangles (1/z, what kind of pixel, which view; painter order, as
 * the game drew them): world pixels (every vertex placed by the camera fit)
 * carry their depth; cars, 2D, HUD, sprites, fills and small views (the
 * mirror) are "keep". Each generated frame is then one draw of a grid with a
 * vertex per native pixel: a world vertex is unprojected with its depth,
 * moved by the view's in-between camera and projected again (nearest wins),
 * and samples the real frame at full resolution. Grid cells over a depth
 * edge or a keep pixel are not drawn: there the copy of the real frame
 * underneath shows. Cars are then drawn again at their own in-between
 * position from the recorded list (cheap); the HUD stays as drawn.
 * Only with [video] frame_generation_method = "reprojection" (or
 * PSX_FRAME_GEN_METHOD=reprojection); redraw, the default, runs and
 * allocates none of this. The object rules below (cars, needles, small
 * views) were tuned on R4 (docs/FRAME_GENERATION.md). */
static GLuint s_rp_zprog = 0, s_rp_wprog = 0, s_rp_vao = 0, s_rp_vbo = 0, s_rp_evao = 0;
static GLuint s_rp_ztex = 0, s_rp_zfbo = 0;
static int    s_rp_zw = 0, s_rp_zh = 0;
static float *s_rp_v = NULL; static size_t s_rp_vcap = 0;
static uint64_t s_rp_key = 0;   /* the game frame the depth image is of */
static uint64_t s_rp_frames = 0, s_rp_fallback = 0;
static GLint  s_rp_z_uSize = -1;
static GLint  s_rp_w_uZ, s_rp_w_uZsize, s_rp_w_uZs, s_rp_w_uGrid, s_rp_w_uSize, s_rp_w_uOx,
              s_rp_w_uView, s_rp_w_uM, s_rp_w_uSrc, s_rp_w_uSrcOrg, s_rp_w_uS, s_rp_w_uDbg;
static GLint  s_rp_r_uSrc, s_rp_r_uZ, s_rp_r_uZsize, s_rp_r_uOrg, s_rp_r_uSrcOrg, s_rp_r_uS,
              s_rp_r_uPrev, s_rp_r_uHasPrev, s_rp_r_uDbg;
/* Diagnostics, read once with the programs: PSX_RP_DEBUG=1 (warp grid) / 2
 * (pixel kinds), PSX_RP_NOPREV (HUD needles over the newer frame's dial). */
static int    s_rp_dbg = 0, s_rp_noprev = 0;
static GLuint s_rp_last_dst = 0;   /* tests: the surface the last warp drew into */
enum { RP_ZS = 2 };   /* depth texels per native pixel */

static const char *RP_Z_VS =
    "#version 330\n"
    "layout(location=0) in vec4 a;\n"
    "layout(location=1) in float av;\n"
    "uniform vec2 u_size;\n"
    "out float v_iz; flat out float v_id; flat out float v_view;\n"
    "void main(){ v_iz=a.z; v_id=a.w; v_view=av;\n"
    "  gl_Position=vec4(a.x/u_size.x*2.0-1.0, a.y/u_size.y*2.0-1.0, 0.0, 1.0); }\n";
static const char *RP_Z_FS =
    "#version 330\n"
    "in float v_iz; flat in float v_id; flat in float v_view; out vec4 frag;\n"
    "void main(){ frag=vec4(v_iz, v_id, v_view, 1.0); }\n";
static const char *RP_W_VS =
    "#version 330\n"
    "uniform sampler2D u_z; uniform ivec2 u_zsize; uniform float u_zs;\n"
    "uniform ivec2 u_grid; uniform vec2 u_size; uniform vec2 u_ox;\n"
    "uniform vec4 u_view[4]; uniform vec4 u_M[12];\n"
    "out vec2 v_src; out float v_bad;\n"
    "vec4 zat(ivec2 g){ return texelFetch(u_z, clamp(ivec2(vec2(g)*u_zs), ivec2(0), u_zsize-1), 0); }\n"
    "void main(){\n"
    "  int id=gl_VertexID, cell=id/6, c=id-cell*6;\n"
    "  int cw=u_grid.x-1; ivec2 base=ivec2(cell-(cell/cw)*cw, cell/cw);\n"
    "  ivec2 o = (c==0)?ivec2(0,0):(c==1||c==3)?ivec2(1,0):(c==2||c==5)?ivec2(0,1):ivec2(1,1);\n"
    "  ivec2 gp=base+o; vec2 p=vec2(gp);\n"
    "  float zmin=1e30, zmax=0.0; float bad=0.0; float v0=-1.0;\n"
    "  for(int k=0;k<4;k++){ vec4 q=zat(base+ivec2(k&1,k>>1));\n"
    "    if(q.y<0.5||q.y>1.5||q.x<=0.0){bad=1.0;} else { float Z=1.0/q.x; zmin=min(zmin,Z); zmax=max(zmax,Z);\n"
    "      if(v0<0.0) v0=q.z; else if(q.z!=v0) bad=1.0; } }\n"
    "  if(zmax>zmin*1.06+4.0) bad=1.0;\n"
    "  vec4 z=zat(gp); vec2 np=p; float depth=1.0;\n"
    "  if(bad<0.5){\n"
    "    int vi=int(z.z+0.5); vec4 V=u_view[vi]; float Z=1.0/z.x; vec2 rel=p-u_ox;\n"
    "    vec3 P=vec3((rel.x-V.x)*Z/V.z,(rel.y-V.y)*Z/V.z,Z);\n"
    "    vec4 r0=u_M[vi*3], r1=u_M[vi*3+1], r2=u_M[vi*3+2];\n"
    "    vec3 Pt=vec3(dot(r0.xyz,P)+r0.w, dot(r1.xyz,P)+r1.w, dot(r2.xyz,P)+r2.w);\n"
    "    Pt.z=max(Pt.z, max(16.0, 0.25*Z));\n"
    "    np=vec2(V.z*Pt.x/Pt.z+V.x, V.z*Pt.y/Pt.z+V.y)+u_ox;\n"
    "    depth=clamp(Pt.z/262144.0, 0.0, 1.0);\n"
    "  }\n"
    "  v_bad=bad; v_src=p;\n"
    "  gl_Position=vec4(np.x/u_size.x*2.0-1.0, np.y/u_size.y*2.0-1.0, depth*2.0-1.0, 1.0);\n"
    "}\n";
static const char *RP_W_FS =
    "#version 330\n"
    "uniform sampler2D u_src; uniform vec2 u_srcorg; uniform float u_S; uniform int u_dbg;\n"
    "in vec2 v_src; in float v_bad; out vec4 frag;\n"
    "void main(){ if(v_bad>0.0) discard;\n"
    "  frag=texelFetch(u_src, ivec2((u_srcorg+v_src)*u_S), 0);\n"
    "  if(u_dbg==1) frag=vec4(fract(v_src/32.0),0.5,1.0); }\n";

static const char *RP_R_VS =
    "#version 330\n"
    "void main(){ vec2 q=vec2((gl_VertexID==1)?3.0:-1.0, (gl_VertexID==2)?3.0:-1.0);\n"
    "  gl_Position=vec4(q,0.0,1.0); }\n";
static const char *RP_R_FS =
    "#version 330\n"
    "uniform sampler2D u_src; uniform sampler2D u_z; uniform sampler2D u_prev; uniform int u_has_prev;\n"
    "uniform ivec2 u_zsize; uniform vec2 u_org; uniform vec2 u_srcorg; uniform float u_S; uniform int u_dbg; out vec4 frag;\n"
    "void main(){ vec2 p=(gl_FragCoord.xy-u_org)/u_S;\n"
    "  vec4 z=texelFetch(u_z, clamp(ivec2(p*2.0), ivec2(0), u_zsize-1), 0);\n"
    "  ivec2 t=ivec2(gl_FragCoord.xy-u_org+u_srcorg*u_S);\n"    "  if(u_dbg==2){ frag=vec4(z.y==1.0?0.0:1.0, z.y==2.0?1.0:0.0, z.y==3.0?1.0:(z.y==0.0?0.5:0.0), 1.0); return; }\n"
    "  if(z.y>2.5){ frag = u_has_prev==1 ? texelFetch(u_prev, t, 0) : texelFetch(u_src, t, 0); return; }\n"
    "  if(z.y>0.5) discard;\n"
    "  frag=texelFetch(u_src, t, 0); }\n";
static GLuint s_rp_rprog = 0;
static int rp_resources(void) {
    if (s_rp_wprog) return 1;
    fg_cost_cold(fg_cost(), 4);   /* compiling the programs is not a frame's cost */
    s_rp_rprog = build_program(RP_R_VS, RP_R_FS);
    s_rp_zprog = build_program(RP_Z_VS, RP_Z_FS);
    s_rp_wprog = build_program(RP_W_VS, RP_W_FS);
    if (!s_rp_zprog || !s_rp_wprog || !s_rp_rprog || !p_glUniform4fv) { s_fg_reproject = 0; return 0; }
    s_rp_z_uSize = p_glGetUniformLocation(s_rp_zprog, "u_size");
    s_rp_w_uZ = p_glGetUniformLocation(s_rp_wprog, "u_z");
    s_rp_w_uZsize = p_glGetUniformLocation(s_rp_wprog, "u_zsize");
    s_rp_w_uZs = p_glGetUniformLocation(s_rp_wprog, "u_zs");
    s_rp_w_uGrid = p_glGetUniformLocation(s_rp_wprog, "u_grid");
    s_rp_w_uSize = p_glGetUniformLocation(s_rp_wprog, "u_size");
    s_rp_w_uOx = p_glGetUniformLocation(s_rp_wprog, "u_ox");
    s_rp_w_uView = p_glGetUniformLocation(s_rp_wprog, "u_view");
    s_rp_w_uM = p_glGetUniformLocation(s_rp_wprog, "u_M");
    s_rp_w_uSrc = p_glGetUniformLocation(s_rp_wprog, "u_src");
    s_rp_w_uSrcOrg = p_glGetUniformLocation(s_rp_wprog, "u_srcorg");
    s_rp_w_uS = p_glGetUniformLocation(s_rp_wprog, "u_S");
    s_rp_w_uDbg = p_glGetUniformLocation(s_rp_wprog, "u_dbg");
    s_rp_r_uSrc = p_glGetUniformLocation(s_rp_rprog, "u_src");
    s_rp_r_uZ = p_glGetUniformLocation(s_rp_rprog, "u_z");
    s_rp_r_uZsize = p_glGetUniformLocation(s_rp_rprog, "u_zsize");
    s_rp_r_uOrg = p_glGetUniformLocation(s_rp_rprog, "u_org");
    s_rp_r_uSrcOrg = p_glGetUniformLocation(s_rp_rprog, "u_srcorg");
    s_rp_r_uS = p_glGetUniformLocation(s_rp_rprog, "u_S");
    s_rp_r_uPrev = p_glGetUniformLocation(s_rp_rprog, "u_prev");
    s_rp_r_uHasPrev = p_glGetUniformLocation(s_rp_rprog, "u_has_prev");
    s_rp_r_uDbg = p_glGetUniformLocation(s_rp_rprog, "u_dbg");
    {
        const char *e = getenv("PSX_RP_DEBUG");
        s_rp_dbg = e ? atoi(e) : 0;
        e = getenv("PSX_RP_NOPREV");
        s_rp_noprev = e && *e && *e != '0';
    }
    p_glGenVertexArrays(1, &s_rp_vao);
    p_glBindVertexArray(s_rp_vao);
    p_glGenBuffers(1, &s_rp_vbo);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_rp_vbo);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(4 * sizeof(float)));
    p_glEnableVertexAttribArray(1);
    p_glGenVertexArrays(1, &s_rp_evao);
    p_glBindVertexArray(0);
    return 1;
}

static int rp_push(size_t *n, float x, float y, float iz, float id, float view) {
    if ((*n + 1) * 5 > s_rp_vcap) {
        size_t cap = s_rp_vcap ? s_rp_vcap * 2 : 5 * 65536;
        float *nv = (float *)realloc(s_rp_v, cap * sizeof *nv);
        if (!nv) return 0;
        s_rp_v = nv; s_rp_vcap = cap;
    }
    float *v = s_rp_v + *n * 5;
    v[0] = x; v[1] = y; v[2] = iz; v[3] = id; v[4] = view;
    (*n)++;
    return 1;
}
static int rp_push_quad(size_t *n, float x0, float y0, float x1, float y1) {
    const float q[6][2] = { {x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1} };
    for (int i = 0; i < 6; i++) if (!rp_push(n, q[i][0], q[i][1], 0.0f, 0.0f, 0.0f)) return 0;
    return 1;
}

/* Projection centre and distance per view, from its camera-placed vertices
 * (x = cx + h X / Z). */
static void rp_view_params(const FgList *L, float out[4][4]) {
    double sx[FG_MAX_VIEWS] = {0}, sy[FG_MAX_VIEWS] = {0}, sh[FG_MAX_VIEWS] = {0};
    uint32_t cnt[FG_MAX_VIEWS] = {0};
    for (uint32_t j = 0; j < L->prims.n; j++) {
        const FgPrim *p = &L->prims.v[j];
        for (int k = 0; k < 3; k++) {
            const FgVert *fv = &s_fg_verts[3 * j + k];
            if (fv->mode != FG_PLACE_CAMERA || fv->view < 0 || fv->view >= FG_MAX_VIEWS) continue;
            if (p->p[k][2] < 1.0f || p->h[k] <= 0.0f) continue;
            const int v = fv->view;
            sx[v] += p->x[k] - p->h[k] * p->p[k][0] / p->p[k][2];
            sy[v] += p->y[k] - p->h[k] * p->p[k][1] / p->p[k][2];
            sh[v] += p->h[k];
            cnt[v]++;
        }
    }
    for (int v = 0; v < FG_MAX_VIEWS; v++) {
        out[v][0] = cnt[v] ? (float)(sx[v] / cnt[v]) : 0.0f;
        out[v][1] = cnt[v] ? (float)(sy[v] / cnt[v]) : 0.0f;
        out[v][2] = cnt[v] ? (float)(sh[v] / cnt[v]) : 1.0f;
        out[v][3] = cnt[v] ? 1.0f : 0.0f;
    }
}

/* A triangle the fit called an object is a car only when it really moved
 * against the world: corners whose older position is what the camera alone
 * predicts (near road subdivided past the fit's pixel tolerance) are world. */
static int rp_is_car(const FgList *L, uint32_t pi) {
    const FgPrim *p = &L->prims.v[pi];
    int obj = 0;
    for (int k = 0; k < 3; k++) {
        const FgVert *fv = &s_fg_verts[3 * pi + k];
        if (fv->mode != FG_PLACE_OBJECT) return 0;
        const int vi = fv->view;
        float A[9], b[3];
        if (vi < 0 || !fg_view_affine(&s_fg_fit, vi, 0.0, A, b)) return 0;
        const float *P = p->p[k];
        float q[3];
        for (int r = 0; r < 3; r++) q[r] = A[3 * r] * P[0] + A[3 * r + 1] * P[1] + A[3 * r + 2] * P[2] + b[r];
        const float dx = q[0] - fv->a[0], dy = q[1] - fv->a[1], dz = q[2] - fv->a[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz), z = fabsf(P[2]) + 1.0f;
        if (d > 0.04f * z) obj = 1;
    }
    return obj;
}

/* Draws the in-between frame of the newer list L at phase t into dst_fbo
 * (which holds a copy of the real frame already). src_tex is the real
 * surface; the region is (x0, y0, W, H) native px, rel x = x - ox. */
static double s_rp_t_snap = 0, s_rp_t_z = 0, s_rp_t_w = 0;
static uint32_t s_rp_zn = 0;
static int fg_reproject(const FgList *L, const FgList *O, double t, GLuint dst_fbo, GLuint src_fbo,
                        int x0, int y0, int W, int H, float ox, int S) {
    if (!rp_resources() || W <= 0 || H <= 0) return 0;
    float vp[4][4];
    rp_view_params(L, vp);
    float big = 0.0f;
    for (uint32_t vi = 0; vi < s_fg_fit.nviews; vi++) {
        const float *ar = s_fg_fit.v[vi].area;
        const float a = (ar[2] - ar[0]) * (ar[3] - ar[1]);
        if (a > big) big = a;
    }
    GLint prev_vp[4]; glGetIntegerv(GL_VIEWPORT, prev_vp);
    const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST), was_blend = glIsEnabled(GL_BLEND),
                    was_stencil = glIsEnabled(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST);
    /* 1. The depth image and a snapshot of the real frame, once per game
     * frame (the game draws on into its buffers while the in-between
     * frames are shown; the snapshot is what they are made of). */
    const int zw = W * RP_ZS, zh = H * RP_ZS;
    const uint64_t key = s_fg_flips * 4096u + (uint64_t)(L - s_fg_l) * 1024u + (uint64_t)W;
    const int li = (int)(L - s_fg_l);
    const int sk = rp_snap_slot(li);
    if (sk < 0 || s_rp_ls_S[sk] != S || s_rp_ls_w[sk] != W * S || s_rp_ls_h[sk] != H * S) goto fail;
    GLuint s_rp_snap_fbo = s_rp_ls_fbo[sk], s_rp_snap_tex = s_rp_ls_tex[sk];
    /* The older list's snapshot: what lies under the newer frame's needle. */
    const int ok_ = O ? rp_snap_slot((int)(O - s_fg_l)) : -1;
    const int ok = (ok_ >= 0 && s_rp_ls_w[ok_] == W * S && s_rp_ls_h[ok_] == H * S) ? ok_ : -1;
    (void)src_fbo;
    /* The picture under the warp: the snapshot. */
    fg_blit2(s_rp_snap_fbo, dst_fbo, 0, 0, x0 * S, y0 * S, W * S, H * S);
    const uint64_t qz = host_now_ns_rthf();
    if (key != s_rp_key || zw != s_rp_zw || zh != s_rp_zh) {
        if (zw != s_rp_zw || zh != s_rp_zh) {
            if (s_rp_zfbo) { p_glDeleteFramebuffers(1, &s_rp_zfbo); glDeleteTextures(1, &s_rp_ztex); }
            fg_cost_cold(fg_cost(), 3);
            s_rp_ztex = make_tex(0x8814 /* GL_RGBA32F */, zw, zh, GL_RGBA, GL_FLOAT);
            if (!make_fbo(&s_rp_zfbo, s_rp_ztex, 0)) { s_rp_zfbo = 0; s_rp_zw = s_rp_zh = 0; goto fail; }
            s_rp_zw = zw; s_rp_zh = zh;
        }
        size_t n = 0;
        if (s_rp_needle_cap < L->prims.n) {
            uint8_t *nn = (uint8_t *)realloc(s_rp_needle, L->prims.n);
            if (!nn) goto fail;
            s_rp_needle = nn; s_rp_needle_cap = L->prims.n;
        }
        fg_hud_match(&L->prims, O ? &O->prims : NULL, 24.0f, s_rp_needle);
        s_rp_needle_n = L->prims.n;
        for (uint32_t r = 0; r < L->n; r++) {
            FgRec h;
            memcpy(&h, L->buf + L->off[r], sizeof h);
            const int32_t *v = (const int32_t *)(L->buf + L->off[r] + sizeof h);
            switch (h.op) {
            case RTH_FLAT_TRI: case RTH_GOURAUD_TRI: case RTH_TEX_TRI:
            case RTH_SHADED_TEX_TRI: case RTH_PROJ_TRI: {
                const int pi = L->rec2prim ? L->rec2prim[r] : -1;
                if (pi < 0) break;
                const FgPrim *p = &L->prims.v[pi];
                int world = 1, view = -1;
                const int carlike = rp_is_car(L, (uint32_t)pi);
                for (int k = 0; k < 3; k++) {
                    const FgVert *fv = &s_fg_verts[3 * pi + k];
                    const int placed_world = fv->mode == FG_PLACE_CAMERA ||
                                             (fv->mode == FG_PLACE_OBJECT && !carlike);
                    if (!placed_world || p->p[k][2] < 1.0f) world = 0;
                    else if (view < 0) view = fv->view;
                    else if (view != fv->view) world = 0;
                }
                if (world && (view < 0 || view >= FG_MAX_VIEWS || !vp[view][3] ||
                              !s_fg_fit.v[view].ok)) world = 0;
                if (world) {
                    const float *ar = s_fg_fit.v[view].area;
                    if ((ar[2] - ar[0]) * (ar[3] - ar[1]) < 0.15f * big) world = 0;
                }
                /* Not world: 2 for 3D the fit moves on its own (cars, the sky
                 * dome; they stay where the warp leaves them), 0 for 2D,
                 * HUD and small views (put back over the warp). */
                int obj = 0;
                if (!world)
                    for (int k = 0; k < 3; k++) obj |= p->vid[k] != 0;
                if (!world && obj && view >= 0 && view < FG_MAX_VIEWS) {
                    const float *ar = s_fg_fit.v[view].area;
                    if ((ar[2] - ar[0]) * (ar[3] - ar[1]) < 0.15f * big) obj = 0;
                }
                const float id = world ? 1.0f : obj ? 2.0f : s_rp_needle[pi] ? 3.0f : 0.0f;
                for (int k = 0; k < 3; k++)
                    if (!rp_push(&n, (p->x[k] + ox) * RP_ZS, p->y[k] * RP_ZS,
                                 world ? 1.0f / p->p[k][2] : 0.0f, id,
                                 world ? (float)view : 0.0f)) goto fail;
                break;
            }
            case RTH_FLAT_RECT: case RTH_TEX_RECT: case RTH_TEX_RECT_SCALED:
                if (!rp_push_quad(&n, ((float)(v[0] - L->disp[0]) + ox) * RP_ZS,
                                  (float)(v[1] - L->disp[1]) * RP_ZS,
                                  ((float)(v[0] - L->disp[0] + v[2]) + ox) * RP_ZS,
                                  (float)(v[1] - L->disp[1] + v[3]) * RP_ZS)) goto fail;
                break;
            default: break;
            }
        }
        /* Overlay, last: small 2D pieces (HUD) and small views (the
         * mirror) of this list and of the older one. A game can draw its HUD
         * or mirror into a buffer after the list for it closed; the older
         * list has them at the same places. Big 2D (a backdrop drawn first)
         * stays out. */
        for (int pass = 0; pass < 2; pass++) {
            const FgList *Q = pass ? O : L;
            if (!Q || !Q->valid) continue;
            const float qox = Q == L ? ox : ox + (float)(Q->wide_off - L->wide_off);
            for (uint32_t r = 0; r < Q->n; r++) {
                FgRec h;
                memcpy(&h, Q->buf + Q->off[r], sizeof h);
                const int32_t *v = (const int32_t *)(Q->buf + Q->off[r] + sizeof h);
                if (h.op == RTH_FLAT_RECT || h.op == RTH_TEX_RECT || h.op == RTH_TEX_RECT_SCALED) {
                    if (v[2] > 160 || v[3] > 120) continue;
                    if (!rp_push_quad(&n, ((float)(v[0] - Q->disp[0]) + qox) * RP_ZS,
                                      (float)(v[1] - Q->disp[1]) * RP_ZS,
                                      ((float)(v[0] - Q->disp[0] + v[2]) + qox) * RP_ZS,
                                      (float)(v[1] - Q->disp[1] + v[3]) * RP_ZS)) goto fail;
                    continue;
                }
                if (h.op != RTH_FLAT_TRI && h.op != RTH_GOURAUD_TRI && h.op != RTH_TEX_TRI &&
                    h.op != RTH_SHADED_TEX_TRI && h.op != RTH_PROJ_TRI) continue;
                const int pi = Q->rec2prim ? Q->rec2prim[r] : -1;
                if (pi < 0) continue;
                const FgPrim *p = &Q->prims.v[pi];
                const int twod = !p->vid[0] && !p->vid[1] && !p->vid[2];
                const float bw = fmaxf(p->x[0], fmaxf(p->x[1], p->x[2])) - fminf(p->x[0], fminf(p->x[1], p->x[2]));
                const float bh = fmaxf(p->y[0], fmaxf(p->y[1], p->y[2])) - fminf(p->y[0], fminf(p->y[1], p->y[2]));
                int smallview = 0;
                {
                    const float aw = p->area[2] - p->area[0], ah = p->area[3] - p->area[1];
                    float bigv = 0.0f;
                    for (uint32_t vi = 0; vi < s_fg_fit.nviews; vi++) {
                        const float *ar = s_fg_fit.v[vi].area;
                        const float av = (ar[2] - ar[0]) * (ar[3] - ar[1]);
                        if (av > bigv) bigv = av;
                    }
                    smallview = aw > 0.0f && ah > 0.0f && aw * ah < 0.15f * bigv;
                }
                if (!(twod && bw <= 160.0f && bh <= 120.0f) && !smallview) continue;
                if (Q == L && twod && s_rp_needle[pi]) continue;   /* needles keep id 3 */
                for (int k = 0; k < 3; k++)
                    if (!rp_push(&n, (p->x[k] + qox) * RP_ZS, p->y[k] * RP_ZS, 0.0f, 0.0f, 0.0f)) goto fail;
            }
        }
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_rp_zfbo);
        glViewport(0, 0, zw, zh);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (n) {
            p_glUseProgram(s_rp_zprog);
            p_glUniform2f(s_rp_z_uSize, (float)zw, (float)zh);
            p_glBindVertexArray(s_rp_vao);
            p_glBindBuffer(PSXGL_ARRAY_BUFFER, s_rp_vbo);
            p_glBufferData(PSXGL_ARRAY_BUFFER, (GLsizeiptr)(n * 5 * sizeof(float)), s_rp_v, PSXGL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n);
        }
        s_rp_key = key;
        s_rp_zn = (uint32_t)n;
        s_rp_t_z = (double)(host_now_ns_rthf() - qz) * 1e-6;
    }
    const uint64_t qw = host_now_ns_rthf();
    /* 2. The warp. */
    {
        float M[12][4]; memset(M, 0, sizeof M);
        for (uint32_t vi = 0; vi < s_fg_fit.nviews && vi < FG_MAX_VIEWS; vi++) {
            float A[9], b[3];
            if (!fg_view_affine(&s_fg_fit, (int)vi, t, A, b)) continue;
            for (int r = 0; r < 3; r++) {
                M[vi * 3 + r][0] = A[3 * r]; M[vi * 3 + r][1] = A[3 * r + 1];
                M[vi * 3 + r][2] = A[3 * r + 2]; M[vi * 3 + r][3] = b[r];
            }
        }
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, dst_fbo);
        glViewport(x0 * S, y0 * S, W * S, H * S);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0 * S, y0 * S, W * S, H * S);
        glClearDepth(1.0);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        p_glUseProgram(s_rp_wprog);
        /* The renderer's own bindings on units 1 and 2 (its programs sample
         * them), put back below. */
        GLint keep_tex[3] = { 0, 0, 0 };
        for (int u = 1; u <= 2; u++) {
            p_glActiveTexture(PSXGL_TEXTURE0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &keep_tex[u]);
        }
        p_glActiveTexture(PSXGL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, s_rp_ztex);
        p_glActiveTexture(PSXGL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_rp_snap_tex);
        p_glUniform1i(s_rp_w_uSrc, 0);
        p_glUniform1i(s_rp_w_uZ, 1);
        p_glUniform2i(s_rp_w_uZsize, zw, zh);
        p_glUniform1f(s_rp_w_uZs, (float)RP_ZS);
        p_glUniform2i(s_rp_w_uGrid, W + 1, H + 1);
        p_glUniform2f(s_rp_w_uSize, (float)W, (float)H);
        p_glUniform2f(s_rp_w_uOx, ox, 0.0f);
        p_glUniform4fv(s_rp_w_uView, 4, &vp[0][0]);
        p_glUniform4fv(s_rp_w_uM, 12, &M[0][0]);
        p_glUniform2f(s_rp_w_uSrcOrg, 0.0f, 0.0f);
        p_glUniform1f(s_rp_w_uS, (float)S);
        p_glUniform1i(s_rp_w_uDbg, s_rp_dbg);
        p_glBindVertexArray(s_rp_evao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(W * H * 6));
        glDisable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        /* The warp's depth (nearest wins, its own scale) is not the scene's:
         * the cars drawn next test only against each other, as on a fresh
         * frame (with PGXP depth they would test LEQUAL against it). */
        glClear(GL_DEPTH_BUFFER_BIT);
        /* 2D and HUD pixels back over the warp, where the real frame has them. */
        p_glUseProgram(s_rp_rprog);
        p_glUniform1i(s_rp_r_uSrc, 0);
        p_glUniform1i(s_rp_r_uZ, 1);
        p_glUniform2i(s_rp_r_uZsize, zw, zh);
        p_glUniform2f(s_rp_r_uOrg, (float)(x0 * S), (float)(y0 * S));
        p_glUniform2f(s_rp_r_uSrcOrg, 0.0f, 0.0f);
        p_glUniform1f(s_rp_r_uS, (float)S);
        p_glActiveTexture(PSXGL_TEXTURE0 + 2);
        /* Without an older snapshot u_prev is unused, but a sampler must
         * still have a complete texture (Apple's GL warns otherwise). */
        glBindTexture(GL_TEXTURE_2D, ok >= 0 ? s_rp_ls_tex[ok] : s_rp_snap_tex);
        p_glActiveTexture(PSXGL_TEXTURE0);
        p_glUniform1i(s_rp_r_uPrev, 2);
        p_glUniform1i(s_rp_r_uHasPrev, ok >= 0 && !s_rp_noprev ? 1 : 0);
        p_glUniform1i(s_rp_r_uDbg, s_rp_dbg);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        s_rp_t_w = (double)(host_now_ns_rthf() - qw) * 1e-6;
        for (int u = 2; u >= 1; u--) {   /* the renderer's bindings back */
            p_glActiveTexture(PSXGL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, (GLuint)keep_tex[u]);
        }
        p_glActiveTexture(PSXGL_TEXTURE0);
    }
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    if (was_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (was_blend) glEnable(GL_BLEND);
    if (was_stencil) glEnable(GL_STENCIL_TEST);
    s_rp_frames++;
    s_rp_last_dst = dst_fbo;
    return 1;
fail:
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    if (was_scissor) glEnable(GL_SCISSOR_TEST);
    if (was_blend) glEnable(GL_BLEND);
    if (was_stencil) glEnable(GL_STENCIL_TEST);
    s_rp_fallback++;
    return 0;
}

/* Everything reprojection allocated (programs, snapshots, depth image,
 * buffers): released when Smooth motion goes off or redraw is chosen, so
 * neither keeps any of it. Context thread. */
static void rp_free(void) {
    if (!s_ctx) return;
    for (int k = 0; k < 2; k++) {
        if (s_rp_ls_fbo[k]) p_glDeleteFramebuffers(1, &s_rp_ls_fbo[k]);
        if (s_rp_ls_tex[k]) glDeleteTextures(1, &s_rp_ls_tex[k]);
        s_rp_ls_fbo[k] = s_rp_ls_tex[k] = 0;
        s_rp_ls_w[k] = s_rp_ls_h[k] = s_rp_ls_S[k] = 0; s_rp_ls_li[k] = -1;
    }
    if (s_rp_zfbo) p_glDeleteFramebuffers(1, &s_rp_zfbo);
    if (s_rp_ztex) glDeleteTextures(1, &s_rp_ztex);
    s_rp_zfbo = s_rp_ztex = 0; s_rp_zw = s_rp_zh = 0; s_rp_key = 0;
    if (s_rp_vbo) p_glDeleteBuffers(1, &s_rp_vbo);
    if (s_rp_vao) p_glDeleteVertexArrays(1, &s_rp_vao);
    if (s_rp_evao) p_glDeleteVertexArrays(1, &s_rp_evao);
    s_rp_vbo = s_rp_vao = s_rp_evao = 0;
    if (s_rp_zprog) p_glDeleteProgram(s_rp_zprog);
    if (s_rp_wprog) p_glDeleteProgram(s_rp_wprog);
    if (s_rp_rprog) p_glDeleteProgram(s_rp_rprog);
    s_rp_zprog = s_rp_wprog = s_rp_rprog = 0;
    free(s_rp_v); s_rp_v = NULL; s_rp_vcap = 0;
    free(s_rp_needle); s_rp_needle = NULL; s_rp_needle_cap = s_rp_needle_n = 0;
    s_rp_last_dst = 0;
}

/* Tests: reprojection holds nothing (no program, surface or buffer). */
static int rp_allocated(void) {
    int any = s_rp_zprog || s_rp_wprog || s_rp_rprog || s_rp_vao || s_rp_vbo || s_rp_evao ||
              s_rp_zfbo || s_rp_ztex || s_rp_v || s_rp_needle;
    for (int k = 0; k < 2; k++) any |= s_rp_ls_fbo[k] || s_rp_ls_tex[k];
    return any;
}

static int fg_generate(double t, int swap) {
    if (s_fg_pa < 0 || s_fg_pb < 0) return 0;
    FgList *a = &s_fg_l[s_fg_pa], *b = &s_fg_l[s_fg_pb];
    if (!a->valid || !b->valid || s_hiw) return 0;
    if (b->wide && (g_wide_w <= 0 || g_wide_w != b->wide_w)) return 0;
    int iw = -1;
    if (b->wide) {
        for (int i = 0; i < WIDE_MAX_SURF; i++)
            if (s_wide_fbo[i] && s_wide_base[i] == b->disp[0]) { iw = i; break; }
        if (iw < 0) return 0;
    }
    /* Land the real stream's queued work on the real surfaces first. */
    flush_line_batch();
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    hiw_flush_queue();
    pack_flush();
    if (!fg_surfaces_ensure(b->wide)) return 0;
    const int S = s_hr_scale;
    /* Real bookkeeping the generated draws would touch. */
    FgState real;
    fg_state_capture(&real);
    const GLuint real_wide_cur = g_wide_cur;
    const int real_pc = s_pc_valid, real_pq = s_pq_valid, real_pz = s_pz_valid;
    /* PGXP depth (G1.14): the generated frame tests depth like the real one,
     * from its own clear (the surface's depth is not the real frame's), and
     * leaves the real stream's depth bookkeeping and counters untouched. */
    const int real_dneed = s_depth_need_clear, real_dused = s_depth_used;
    const float real_davg = s_depth_last_avg;
    const uint64_t real_dtris = s_depth_tris, real_dclears = s_depth_clears;
    s_pz_valid = 0;
    s_depth_need_clear = 1;
    DirtyRect pack = s_pack_dirty, sten = s_stencil_stale, cpu = s_cpu_dirty;
    const int sten_valid = s_stencil_valid, gpu_dirty = s_gpu_dirty;
    uint64_t pres_dirty[PRES_ROWS];
    memcpy(pres_dirty, s_present_dirty, sizeof pres_dirty);
    int wst[4] = { 0, 0, 0, 0 };
    if (iw >= 0) { wst[0] = s_wst_x0[iw]; wst[1] = s_wst_y0[iw]; wst[2] = s_wst_x1[iw]; wst[3] = s_wst_y1[iw]; }
    /* The displayed buffer as the real frame has it, then swap the surfaces. */
    const int dy = b->disp[1] < 0 ? 0 : b->disp[1];
    const int dh = b->disp[1] + b->disp[3] > VRAM_H ? VRAM_H - dy : b->disp[1] + b->disp[3] - dy;
    /* The redrawn frame (the older one when moving forward) is drawn where
     * its own buffer is, over the newer real frame's image: what the
     * in-between camera uncovers shows that. */
    const FgList *L = s_fg_src_older ? a : b;
    const double tl = s_fg_src_older ? 1.0 - t : t;
    const int ly = L->disp[1] < 0 ? 0 : L->disp[1];
    const int lh = L->disp[1] + L->disp[3] > VRAM_H ? VRAM_H - ly : L->disp[1] + L->disp[3] - ly;
    if (L->disp[0] != b->disp[0] || lh != dh || L->disp[2] != b->disp[2]) return 0;
    /* Reprojected frames start from the list's snapshot instead (below). */
    const int will_rp = s_fg_reproject && L == b && s_fg_fit.ok && rp_snap_slot((int)(L - s_fg_l)) >= 0;
    if (!will_rp) {
        fg_blit2(s_hr_fbo, s_fg_hr_fbo, b->disp[0] * S, dy * S, L->disp[0] * S, ly * S, b->disp[2] * S, dh * S);
        if (iw >= 0) fg_blit2(s_wide_fbo[iw], s_fg_w_fbo, 0, dy * S, 0, ly * S, g_wide_w * S, dh * S);
    }
    GLuint t0 = s_hr_tex, f0 = s_hr_fbo, r0 = s_hr_rb;
    s_hr_tex = s_fg_hr_tex; s_hr_fbo = s_fg_hr_fbo; s_hr_rb = s_fg_hr_rb;
    GLuint wt = 0, wf = 0, wr = 0; int was = 0;
    if (iw >= 0) {
        wt = s_wide_tex[iw]; wf = s_wide_fbo[iw]; wr = s_wide_rb[iw]; was = s_wide_as[iw];
        s_wide_tex[iw] = s_fg_w_tex; s_wide_fbo[iw] = s_fg_w_fbo; s_wide_rb[iw] = s_fg_w_rb;
        s_wide_as[iw] = S;
    }
    /* The copied stencil is stale: rebuilt from alpha if a draw needs it. */
    s_stencil_valid = 0;
    rect_add(&s_stencil_stale, L->disp[0], ly, L->disp[0] + L->disp[2] - 1, ly + lh - 1);
    if (iw >= 0) wst_add(iw, 0, ly, g_wide_w, ly + lh);
    s_fg_drawing = 1;
    int reproj = 0;
    if (will_rp) {
        flush_line_batch(); flush_flat_batch(); flush_tex_batch();
        reproj = iw >= 0
            ? fg_reproject(L, a, tl, s_fg_w_fbo, wf, 0, ly, g_wide_w, lh, (float)L->wide_off, S)
            : fg_reproject(L, a, tl, s_fg_hr_fbo, f0, L->disp[0], ly, L->disp[2], lh, 0.0f, S);
    }
    if (will_rp && !reproj) {   /* no snapshot after all: the copies the redraw needs */
        fg_blit2(f0, s_fg_hr_fbo, b->disp[0] * S, dy * S, L->disp[0] * S, ly * S, b->disp[2] * S, dh * S);
        if (iw >= 0) fg_blit2(wf, s_fg_w_fbo, 0, dy * S, 0, ly * S, g_wide_w * S, dh * S);
    }
    /* Reprojected: only the cars are drawn again (their own motion). */
    s_fg_replay_objects_only = reproj;
    gpu_hd_textures_suppress_dumps(1);   /* presentation only: dumps see real frames */
    fg_replay(s_fg_src_older ? b : a, L, tl, iw >= 0 ? s_fg_w_fbo : 0);
    gpu_hd_textures_suppress_dumps(0);
    s_fg_replay_objects_only = 0;
    flush_line_batch();
    flush_flat_batch();
    flush_tex_batch();
    /* The strips along a view's edges the picture moved away from: the
     * newer frame drew nothing beyond the edge, so they show it. On the
     * native-wide surface native x maps to x + wide_off. */
    {
        const int ox = iw >= 0 ? L->wide_off : 0, sw = iw >= 0 ? g_wide_w : L->disp[2];
        const GLuint src = iw >= 0 ? wf : f0, dst = iw >= 0 ? s_fg_w_fbo : s_fg_hr_fbo;
        const int bx0 = iw >= 0 ? 0 : L->disp[0], sdy = dy - ly;   /* source rows: the newer frame's */
        for (uint32_t vi = 0; vi < s_fg_fit.nviews; vi++) {
            const float *ar = s_fg_fit.v[vi].area, *m = s_fg_margin[vi];
            int x1 = (int)floorf(ar[0]) + ox, x2 = (int)ceilf(ar[2]) + 1 + ox;
            int y1 = L->disp[1] + (int)ar[1], y2 = L->disp[1] + (int)ar[3] + 1;
            if (x1 < 0) x1 = 0;
            if (x2 > sw) x2 = sw;
            if (y1 < ly) y1 = ly;
            if (y2 > ly + lh) y2 = ly + lh;
            if (x2 <= x1 || y2 <= y1) continue;
            const int w = x2 - x1, h = y2 - y1;
            int ml = m[0] > 0 ? (int)ceilf(m[0]) + 1 : 0, mt = m[1] > 0 ? (int)ceilf(m[1]) + 1 : 0;
            int mr = m[2] > 0 ? (int)ceilf(m[2]) + 1 : 0, mb = m[3] > 0 ? (int)ceilf(m[3]) + 1 : 0;
            if (ml > w) ml = w;
            if (mr > w) mr = w;
            if (mt > h) mt = h;
            if (mb > h) mb = h;
            if (ml) fg_blit2(src, dst, (bx0 + x1) * S, (y1 + sdy) * S, (bx0 + x1) * S, y1 * S, ml * S, h * S);
            if (mr) fg_blit2(src, dst, (bx0 + x2 - mr) * S, (y1 + sdy) * S, (bx0 + x2 - mr) * S, y1 * S, mr * S, h * S);
            if (mt) fg_blit2(src, dst, (bx0 + x1) * S, (y1 + sdy) * S, (bx0 + x1) * S, y1 * S, w * S, mt * S);
            if (mb) fg_blit2(src, dst, (bx0 + x1) * S, (y2 - mb + sdy) * S, (bx0 + x1) * S, (y2 - mb) * S, w * S, mb * S);
        }
    }
    s_fg_reprojected_now = reproj;
    fg_compose(L, iw >= 0 ? s_fg_w_fbo : 0, iw >= 0 ? s_fg_w_tex : 0, b->linear, b->force43);
    s_fg_reprojected_now = 0;
    s_fg_drawing = 0;
    if (swap) {
        s_fg_presenting = 1;
        gl_swap_with_osd();
        s_fg_presenting = 0;
    }
    /* Back to the real surfaces and state. */
    s_hr_tex = t0; s_hr_fbo = f0; s_hr_rb = r0;
    if (iw >= 0) {
        s_wide_tex[iw] = wt; s_wide_fbo[iw] = wf; s_wide_rb[iw] = wr; s_wide_as[iw] = was;
        s_wst_x0[iw] = wst[0]; s_wst_y0[iw] = wst[1]; s_wst_x1[iw] = wst[2]; s_wst_y1[iw] = wst[3];
    }
    fg_state_apply(&real, real_wide_cur);
    s_pc_valid = real_pc; s_pq_valid = real_pq; s_pz_valid = real_pz;
    s_depth_need_clear = real_dneed; s_depth_used = real_dused; s_depth_last_avg = real_davg;
    s_depth_tris = real_dtris; s_depth_clears = real_dclears;
    s_pack_dirty = pack; s_stencil_stale = sten; s_cpu_dirty = cpu;
    s_stencil_valid = sten_valid; s_gpu_dirty = gpu_dirty;
    memcpy(s_present_dirty, pres_dirty, sizeof pres_dirty);
    sw_set_semi_transparency(s_semi_en, s_semi_mode);
    sw_set_mask_bits(s_mask_set, s_mask_check);
    sw_set_draw_area(s_area_x1, s_area_y1, s_area_x2, s_area_y2);
    sw_set_draw_offset(s_off_x, s_off_y);
    sw_set_color_modulation(s_mod_r, s_mod_g, s_mod_b, s_mod_raw);
    sw_set_texture_window((uint32_t)(s_tw_mask_x | (s_tw_mask_y << 5) | (s_tw_off_x << 10) |
                                     (s_tw_off_y << 15)));
    return 1;
}

/* ---- frame generation: schedule (render thread) ---- */
static void rth_present_payload(uint16_t op, const uint8_t *p);   /* below */

/* Short first hold, doubling on repeats within 2 s of the last hold's end,
 * up to 8 s: one hiccup costs a fraction of a second of generation. */
/* Reprojected in-between frames cost a millisecond: an overload is the
 * real frames' own, so the hold is short (0.1 s, at most 1 s). */
/* s_fg_brk_init: 1 + the method the breaker was set up for; a method switch
 * starts that method's breaker afresh. */
#define FG_BRK_INIT() do { if (s_fg_brk_init != 1 + s_fg_reproject) { \
        if (s_fg_reproject) fg_breaker_init(&s_fg_brk, 0.1, 1.0, 1.0); \
        else fg_breaker_init(&s_fg_brk, 0.5, 8.0, 2.0); s_fg_brk_init = 1 + s_fg_reproject; } } while (0)
static FgCeiling s_fg_ceil;
static int       s_fg_ceil_init = 0;
static FgCeiling *fg_ceil(void) {
    if (!s_fg_ceil_init) { fg_ceiling_init(&s_fg_ceil, 15, 2.0); s_fg_ceil_init = 1; }
    return &s_fg_ceil;
}
static uint32_t s_fg_trip_late = 0, s_fg_trip_bp = 0, s_fg_trip_behind = 0;
static void fg_trip(const char *why) {
    FG_BRK_INIT();
    if (why[0] == 'g') s_fg_trip_late++;
    else s_fg_trip_bp++;
    const double now = fg_now_s();
    fg_breaker_trip(&s_fg_brk, now, why);
    fg_ceiling_trip(fg_ceil(), s_fg_pending ? s_fg_n : s_fg_last_n, now);
}
/* The render thread a frame behind (a stale present, two frames queued):
 * proportional, not a stop. 2P races show stale presents with generation
 * off as well, so it lowers the plan's ceiling by one (recovering one per
 * 2 s) and the real frame shows at once; the breaker is for the guest
 * slipping or stalling on the queue. */
static void fg_overload(void) {
    s_fg_trip_behind++;
    fg_ceiling_trip(fg_ceil(), s_fg_pending ? s_fg_n : s_fg_last_n, fg_now_s());
}

static void fg_gen_cost_publish(uint64_t cost_ns) {
    atomic_fetch_add(&s_fg_gen_cost_seq, 1);
    atomic_fetch_add(&s_fg_gen_cost_ns, cost_ns);
    atomic_fetch_add(&s_fg_gen_measured, 1);
    atomic_fetch_add(&s_fg_gen_cost_seq, 1);
}

static void fg_gen_cost_poll(void) {
    while (s_fg_qt != s_fg_qh) {
        const unsigned i = s_fg_qt % 4u;
        GLuint64 avail = 0, ns = 0;
        p_glGetQueryObjectui64v(s_fg_q[i], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) break;
        p_glGetQueryObjectui64v(s_fg_q[i], GL_QUERY_RESULT, &ns);
        double c = (double)(ns > s_fg_q_cpu[i] ? ns : s_fg_q_cpu[i]) * 1e-9;
        fg_cost_add(fg_cost(), c, s_fg_fit_s);
        atomic_store(&s_fg_cost_ema_pub, fg_cost()->ema);
        fg_gen_cost_publish((uint64_t)(c * 1e9));
        s_fg_gen_gpu_ms = (double)ns * 1e-6;
        s_fg_gen_cpu_ms = (double)s_fg_q_cpu[i] * 1e-6;
        s_fg_qt++;
    }
}

/* One generated frame at phase t, timed (GPU query, CPU wall minus swap). */
static void fg_generate_timed(double t) {
    rthf_pause();
    /* (Re)allocation at a new level is not the generated frame's cost. */
    if (s_fg_pb >= 0) (void)fg_surfaces_ensure(s_fg_l[s_fg_pb].wide);
    if (s_fg_reproject) (void)rp_resources();   /* compiling is not the frame's cost */
    if (s_fg_qok < 0) {
        s_fg_qok = (p_glGenQueries && p_glBeginQuery && p_glEndQuery && p_glGetQueryObjectui64v) ? 1 : 0;
        if (s_fg_qok) p_glGenQueries(4, s_fg_q);
    }
    const int q = s_fg_qok > 0 && s_fg_qh - s_fg_qt < 4u;
    if (q) { gl_perf_yield(); p_glBeginQuery(GL_TIME_ELAPSED, s_fg_q[s_fg_qh % 4u]); }
    const uint64_t c0 = host_now_ns_rthf(), sw0 = s_rthf_swap_ns;
    /* The overlay of the real frame these lead up to. */
    if (s_fg_real_p) {
        RthOvHdr hd;
        memcpy(&hd, s_fg_real_p, sizeof hd);
        const uint8_t *qq = s_fg_real_p + sizeof hd;
        memset(&s_rth_ov, 0, sizeof s_rth_ov);
        s_rth_ov.valid = 1;
        s_rth_ov.needs_present = hd.needs_present;
        s_rth_ov.slide = hd.slide;
        for (int i = 0; i < 4; i++)
            if (hd.has & (1 << i)) {
                s_rth_ov.px[i] = (const uint32_t *)qq;
                s_rth_ov.w[i] = hd.w[i]; s_rth_ov.h[i] = hd.h[i];
                qq += (size_t)hd.w[i] * hd.h[i] * 4u;
            }
    }
    /* Timed without the swap: it waits for the compositor, not for work. */
    const int ok = fg_generate(t, 0);
    const uint64_t cpu = host_now_ns_rthf() - c0 - (s_rthf_swap_ns - sw0);
    if (q) {
        p_glEndQuery(GL_TIME_ELAPSED);
        s_fg_q_cpu[s_fg_qh % 4u] = cpu;
        s_fg_qh++;
    }
    if (ok) {
        s_fg_presenting = 1;
        gl_swap_with_osd();
        s_fg_presenting = 0;
    }
    s_rth_ov.valid = 0;
    if (ok) { s_fg_generated++; atomic_store(&s_fg_last_gen_ns, rt_now_ns()); }
    rthf_resume();
}

/* Keep the composed back buffer (a real frame) for later. 1 when kept. */
static int fg_keep_real(void) {
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (ww <= 0 || wh <= 0) return 0;
    if (!s_fg_real_fbo || s_fg_real_w != ww || s_fg_real_h != wh) {
        if (s_fg_real_fbo) { p_glDeleteFramebuffers(1, &s_fg_real_fbo); glDeleteTextures(1, &s_fg_real_tex); }
        s_fg_real_fbo = s_fg_real_tex = 0;
        s_fg_real_tex = make_tex(GL_RGBA8, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE);
        if (!make_fbo(&s_fg_real_fbo, s_fg_real_tex, 0)) {
            glDeleteTextures(1, &s_fg_real_tex);
            s_fg_real_tex = s_fg_real_fbo = 0;
            return 0;
        }
        s_fg_real_w = ww; s_fg_real_h = wh;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_fg_real_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(0, 0, ww, wh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    s_fg_real_kept = 1;
    return 1;
}

static void fg_present_real(void) {
    s_fg_pending = 0;
    if (!s_fg_real_kept) return;   /* nothing changed: the present was skipped */
    s_fg_real_kept = 0;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_fg_real_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    if (ww != s_fg_real_w || wh != s_fg_real_h) {
        glViewport(0, 0, ww, wh);
        glClearColor(0.f, 0.f, 0.f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    p_glBlitFramebuffer(0, 0, s_fg_real_w, s_fg_real_h, 0, 0, s_fg_real_w, s_fg_real_h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    gl_swap_now(0);
    s_fg_real_presents++;
    fg_note_present(rt_now_ns());
}

/* Present the waiting real frame now (generated frames still due are
 * dropped): a newer present, a sync point, the queue backing up. */
static void fg_flush(void) {
    if (!s_fg_pending) return;
    if (s_fg_k <= s_fg_n) s_fg_flushed++;
    fg_present_real();
}

static uint64_t fg_tick(void *user, uint64_t now) {
    (void)user;
    if (!s_fg_pending) return 0;
    /* Behind: a later frame is complete already, or the guest waited on us. */
    /* A whole frame waiting behind this one: the real frame goes now. Two,
     * or the guest waited on the queue: the breaker as well. */
    const uint64_t bp = rt_backpressure_events();
    const int ahead = rt_frames_ahead();
    if (!s_fg_force && (ahead >= 1 || bp != s_fg_bp_seen)) {
        /* Backpressure itself is judged at the flip, by how long the guest
         * waited (fg_on_present); here it only shows the real frame now. */
        if (bp == s_fg_bp_seen && ahead >= 2 && fg_recently_generated()) fg_overload();
        if (s_fg_k <= s_fg_n) { if (bp != s_fg_bp_seen) s_fg_end_bp++; else s_fg_end_ahead++; }
        s_fg_bp_seen = bp;
        fg_flush();
        return 0;
    }
    const uint64_t due = s_fg_due;
    if (now < due) return due;
    if (s_fg_k <= s_fg_n) {
        /* The phase comes from the host clock: the in-between frame shows
         * one display interval from now, so any refresh (also one that is no
         * multiple of the game's 30 Hz) and a late tick both get the camera
         * where it is when the frame is seen. Past the end of the game
         * frame the rest are dropped and the real frame goes. */
        const double t = fg_clock_phase(now - s_fg_t0, s_fg_step_ns, s_fg_flip_ns);
        if (t > 0.0) {
            fg_generate_timed(t);
            s_fg_k++;
            fg_note_present(now);
            return s_fg_due;
        }
        s_fg_flushed++;
        s_fg_end_phase++;
    }
    fg_present_real();
    return 0;
}

/* A present record replayed with frame generation on. Returns 1 when it was
 * handled (scheduled or dropped), 0 to present it as usual. */
static int fg_on_present(uint16_t op, const uint8_t *p, uint32_t bytes, int stale) {
    /* The game frame's arrival: its in-between frames are timed from here,
     * not from when its composition finished, because the next game frame
     * arrives one game frame after this one whatever the composition cost
     * (timed from the end, the last in-between frame of every game frame
     * collided with the next one and was dropped). */
    const uint64_t arrive_ns = rt_now_ns();
    RthOvHdr hd;
    memcpy(&hd, p, sizeof hd);
    const int32_t *a = hd.args;
    const int wide = op == RTH_PRESENT_WIDE;
    int disp[4];
    if (wide) {
        const int nw = g_wide_w - 2 * g_wide_off;
        disp[0] = a[0]; disp[1] = a[1]; disp[2] = nw > 0 ? nw : 320; disp[3] = a[2];
    } else {
        disp[0] = a[0]; disp[1] = a[1]; disp[2] = a[2]; disp[3] = a[3];
    }
    if (s_fg_broken) {   /* frames drawn without the render thread in between */
        s_fg_broken = 0;
        for (int i = 0; i < 4; i++) if (i != s_fg_cur) s_fg_l[i].valid = 0;
        s_fg_have_last = 0;
        s_fg_partial = 1;   /* the list being captured misses records */
    }
    {   /* Learn the display buffers. */
        int k = 0;
        while (k < s_fg_nbuf && memcmp(s_fg_buf[k], disp, sizeof s_fg_buf[k]) != 0) k++;
        if (k == s_fg_nbuf) {
            /* A buffer at the same origin with a new size (a mode change, a
             * save state's first frame) replaces the old entry: buffers are
             * looked up by the point drawn to, and a stale entry found first
             * would close every list with the old size, so no two frames
             * ever matched. */
            for (int j = 0; j < s_fg_nbuf; j++)
                if (s_fg_buf[j][0] == disp[0] && s_fg_buf[j][1] == disp[1]) {
                    memcpy(s_fg_buf[j], disp, sizeof s_fg_buf[j]);
                    k = j;
                    break;
                }
        }
        if (k == s_fg_nbuf) {
            if (s_fg_nbuf == 4) { memmove(s_fg_buf[0], s_fg_buf[1], sizeof s_fg_buf[0] * 3); s_fg_nbuf = 3; s_fg_cur_buf = -1; }
            memcpy(s_fg_buf[s_fg_nbuf++], disp, sizeof s_fg_buf[0]);
        }
    }
    const int flip = !s_fg_have_last || disp[0] != s_fg_last_dx || disp[1] != s_fg_last_dy;
    s_fg_vblanks++;
    if (!flip) {
        /* The same game frame again (a 30 Hz game's second VBlank): while
         * its in-between frames are being shown, that is what it shows. */
        s_fg_dups++;
        return s_fg_pending ? 1 : 0;
    }
    if (s_fg_pending) fg_flush();
    s_fg_flips++;
    {
        const int vb = s_fg_have_last ? s_fg_vblanks : 1;
        atomic_store(&s_fg_flip_vb, vb > 4 ? 4 : vb);
    }
    s_fg_vblanks = 0;
    s_fg_have_last = 1; s_fg_last_dx = disp[0]; s_fg_last_dy = disp[1];
    /* A game that flips right after drawing: the capturing list is this
     * buffer's frame, complete. (One that flips later has moved on to the
     * other buffer already, which closed it.) */
    if (s_fg_cur_buf >= 0 && s_fg_buf[s_fg_cur_buf][0] == disp[0] &&
        s_fg_buf[s_fg_cur_buf][1] == disp[1]) {
        fg_rotate();
        s_fg_cur_buf = -1;
    }
    /* The newest closed list is this buffer's frame; the one before it the
     * previous game frame. */
    FgList *A = &s_fg_l[s_fg_older], *B = &s_fg_l[s_fg_newer];
    if (B->valid && (B->disp[0] != disp[0] || B->disp[1] != disp[1])) B = A = NULL;
    if (B) { B->wide = wide; B->linear = wide ? a[3] : a[4]; B->force43 = wide ? 0 : a[5]; }
    const double now = fg_now_s();
    /* Guest lateness and backpressure trip the breaker only while generation
     * runs: otherwise they are not generation's doing. */
    const int gen_recent = fg_recently_generated();
    if (atomic_exchange(&s_fg_late, 0)) {
        /* A reprojected in-between frame costs the render thread about a
         * millisecond: a late guest frame is not its doing unless they
         * got expensive. */
        const int cheap = s_fg_reproject && s_fg_gen_cpu_ms < 3.0;
        if (gen_recent && !cheap) fg_trip("guest late"); else s_fg_ignored_late++;
    }
    const char *why = atomic_exchange(&s_fg_hold_reason, NULL);
    if (why) s_fg_hold_why = why;
    const int held = rt_now_ns() < atomic_load(&s_fg_hold_until);
    /* The guest waiting on the queue trips the breaker when the wait since
     * the last flip was a stall (a quarter of the game frame), not the short
     * waits a 2P race shows with generation off as well. */
    const uint64_t bp = rt_backpressure_events();
    s_fg_bp_seen = bp;
    {
        RtStats rs;
        rt_get_stats(&rs);
        const uint64_t bns = rs.backpressure_ns + rs.ring_full_ns;
        const double waited = (double)(bns - s_fg_bp_ns_seen) * 1e-9;
        const double ghz0 = atomic_load(&s_fg_guest_hz);
        s_fg_bp_ns_seen = bns;
        if (waited > 0.25 * (double)s_fg_flip_vb / (ghz0 > 1.0 ? ghz0 : 59.94)) {
            if (gen_recent) fg_trip("queue backed up"); else s_fg_ignored_bp++;
        } else if (waited > 0.0) {
            s_fg_ignored_bp++;
        }
    }
    if (stale && gen_recent) fg_overload();
    FG_BRK_INIT();
    const double ghz = atomic_load(&s_fg_guest_hz), rhz = atomic_load(&s_fg_refresh_hz);
    const double flip_s = (double)s_fg_flip_vb / (ghz > 1.0 ? ghz : 59.94);
    const int slots = fg_slots(flip_s, rhz);   /* as the clock shows them */
    s_fg_last_slots = slots;
    /* What one generated frame may cost (its swap included) to fit. */
    const double real_s = s_fg_real_ema * (double)s_fg_flip_vb + s_fg_swap_ema;
    s_fg_fit_s = 0.85 * flip_s - real_s - s_fg_swap_ema;
    if (s_fg_qok > 0) fg_gen_cost_poll();   /* once per game frame: a poll flushes */
    int n = 0;
    s_fg_noplan_why = !B ? "newer list is another buffer" : !A->valid ? "older list invalid"
        : !B->valid ? "newer list invalid" : (A->disp[2] != B->disp[2] || A->disp[3] != B->disp[3])
        ? "display size changed" : A->wide_w != B->wide_w ? "wide width changed"
        : A->scale <= 0 ? "no scale" : "";
    if (A && B && A->valid && B->valid && (A->disp[2] != B->disp[2] || A->disp[3] != B->disp[3])) {
        static char why[96];
        snprintf(why, sizeof why, "display size changed %dx%d@%d,%d -> %dx%d@%d,%d",
                 A->disp[2], A->disp[3], A->disp[0], A->disp[1], B->disp[2], B->disp[3], B->disp[0], B->disp[1]);
        s_fg_noplan_why = why;
    }
    if (A && B && A->valid && B->valid && A->disp[2] == B->disp[2] &&
        A->disp[3] == B->disp[3] && A->wide_w == B->wide_w && A->scale > 0) {
        /* The surfaces exist (and are warm) before a frame is planned on them. */
        if (!s_hiw) (void)fg_surfaces_ensure(B->wide);
        if (s_fg_force) n = slots > 1 ? slots - 1 : 1;
        else if (fg_breaker_open(&s_fg_brk, now) && !stale && !held) {
            const double est = fg_cost_estimate(fg_cost(), now, s_fg_fit_s);
            atomic_store(&s_fg_cost_ema_pub, fg_cost()->ema);
            n = fg_plan(flip_s, fg_plan_hz(flip_s, rhz), real_s, est > 0.0 ? est + s_fg_swap_ema : 0.0, 0.85,
                        fg_ceiling_get(fg_ceil(), now));
        }
        if (n == 0) {
            s_fg_skipped_plan++;
            if (!fg_breaker_open(&s_fg_brk, now)) s_fg_np_brk++;
            else if (stale) s_fg_np_stale++;
            else if (held) s_fg_np_held++;
            else s_fg_np_room++;
        }
    }
    s_fg_last_n = n;
    if (n <= 0) return 0;
    /* Match the two frames' triangles. */
    const uint64_t m0 = rt_now_ns();
    /* Which frame is redrawn: the one the in-between camera sees more of.
     * Moving forward, the older frame seen from further on spreads past
     * the screen edges (nothing missing); the newer frame seen from further
     * back would shrink away from them. Otherwise the newer frame. */
    const uint32_t nmax = A->prims.n > B->prims.n ? A->prims.n : B->prims.n;
    if (s_fg_verts_cap < nmax) {
        FgVert *nv = (FgVert *)realloc(s_fg_verts, (size_t)nmax * 3 * sizeof *nv);
        if (!nv) return 0;
        s_fg_verts = nv; s_fg_verts_cap = nmax;
    }
    FgCamParams cp;
    fg_cam_defaults(&cp);
    cp.keep_partial = s_fg_reproject;
    cp.freeze_small_views = s_fg_reproject;
    /* Reprojection redraws whatever moves on its own (a rival car filling
     * half of a split-screen view), so a third of the pairs is enough. */
    if (s_fg_reproject) cp.min_inliers = 0.3f;
    FgList *src = A;
    if (s_fg_reproject) {   /* the newer frame's image is warped: fit for it */
        src = B;
        fg_cam_fit(&A->prims, &B->prims, &cp, &s_fg_fit, s_fg_verts);
    } else if (!fg_cam_fit(&B->prims, &A->prims, &cp, &s_fg_fit, s_fg_verts) ||
        s_fg_fit.v[fg_main_view(&s_fg_fit)].t[2] <= 0.0) {
        src = B;
        fg_cam_fit(&A->prims, &B->prims, &cp, &s_fg_fit, s_fg_verts);
    }
    s_fg_src_older = src == A;
    /* A full-width view on the native-wide surface reaches its edges, not
     * the native draw area's. */
    if (src->wide)
        for (uint32_t vi = 0; vi < s_fg_fit.nviews; vi++) {
            float *ar = s_fg_fit.v[vi].area;
            if (ar[0] <= 0.0f && ar[2] >= (float)(src->disp[2] - 1)) {
                ar[0] -= (float)src->wide_off;
                ar[2] += (float)src->wide_off;
            }
        }
    s_fg_last_match_ms = (double)(rt_now_ns() - m0) * 1e-6;
    /* The verdict: a frame whose correspondence is not trustworthy is not
     * generated; the real frame shows at its own time. A forced run (the
     * fixture's synthetic scenes carry no GTE identities) skips it. */
    /* Geometry the in-between camera passes through (a tunnel ceiling, a
     * bridge overhead) has no correct in-between image without clipping at
     * the near plane: such a game frame shows its real frames. */
    if (s_fg_fit.ok && s_fg_pos_cap < src->prims.n) {
        float *nx = (float *)realloc(s_fg_px, (size_t)src->prims.n * 3 * sizeof *nx);
        if (nx) s_fg_px = nx;
        float *ny = (float *)realloc(s_fg_py, (size_t)src->prims.n * 3 * sizeof *ny);
        if (ny) s_fg_py = ny;
        if (nx && ny) s_fg_pos_cap = src->prims.n;
    }
    if (s_fg_fit.ok && s_fg_pos_cap >= src->prims.n) {   /* the phase furthest from the redrawn frame */
        fg_cam_place(&src->prims, &s_fg_fit, s_fg_verts, 1.0 / (double)(n + 1), s_fg_px, s_fg_py, NULL);
        if (s_fg_fit.clamped) { s_fg_fit.ok = 0; s_fg_fit.why = "camera passes through geometry"; }
    }
    if (!s_fg_fit.ok && !s_fg_force) {
        s_fg_rejected++;
        s_fg_reject_why = s_fg_fit.why;
        s_fg_last_n = 0;
        return 0;
    }
    /* The real frame waits for the generated ones before it. */
    if (s_fg_real_cap < bytes) {
        uint8_t *nb = (uint8_t *)realloc(s_fg_real_p, bytes);
        if (!nb) return 0;
        s_fg_real_p = nb; s_fg_real_cap = bytes;
    }
    memcpy(s_fg_real_p, p, bytes);
    s_fg_real_op = op;
    /* The real frame, composed and kept now. */
    s_fg_real_kept = 0;
    s_fg_capture_real = 1;
    rth_present_payload(op, p);
    s_fg_capture_real = 0;
    /* Reprojection's source: the surface as this present showed it (the
     * game draws on into its buffers while the in-between frames run). */
    if (s_fg_reproject && B) rp_snapshot_list((int)(B - s_fg_l), B->disp);
    s_fg_pa = s_fg_older; s_fg_pb = s_fg_newer;
    s_fg_n = n; s_fg_k = 1;
    s_fg_t0 = arrive_ns;
    s_fg_flip_ns = (uint64_t)(flip_s * 1e9);
    s_fg_step_ns = (uint64_t)(fg_step_s(flip_s, rhz, n) * 1e9);
    if (s_fg_due < s_fg_t0 || s_fg_due > s_fg_t0 + s_fg_step_ns) s_fg_due = s_fg_t0;
    s_fg_pending = 1;
    s_fg_bp_seen = bp;
    uint64_t next = fg_tick(NULL, rt_now_ns());
    if (next) rt_tick_at(next);
    return 1;
}

/* ---- frame generation: public ---- */
void gl_renderer_set_frame_generation(int on) {
    GL_RT_SYNC("frame_generation");
    s_fg_on = on ? 1 : 0;
    const char *e = getenv("PSX_FRAME_GEN_FORCE");
    s_fg_force = e && e[0] == '1';
    if (!s_fg_on) {
        if (s_ctx) { fg_surfaces_free(); rp_free(); }
        fg_invalidate();
    } else {
        fg_invalidate();
    }
}
int gl_renderer_frame_generation(void) { return s_fg_on; }
/* [video] frame_generation_method (PSX_FRAME_GEN_METHOD, resolved by the
 * frontend): redraw, the default, draws the whole recorded list again per
 * generated frame; reprojection (a title opts in) warps the newer real frame.
 * Redraw allocates and runs nothing of reprojection's. */
void gl_renderer_set_frame_generation_method(int method) {
    GL_RT_SYNC("frame_generation_method");
    const int rp = method == GL_FG_METHOD_REPROJECTION;
    if (rp == s_fg_reproject) return;
    s_fg_reproject = rp;
    if (!rp) rp_free();
    fg_invalidate();
}
int gl_renderer_frame_generation_method(void) {
    return s_fg_reproject ? GL_FG_METHOD_REPROJECTION : GL_FG_METHOD_REDRAW;
}

void gl_renderer_fg_source(const uint32_t id[3], const int32_t pc[9], const int32_t h[3],
                           const int32_t x[3], const int32_t y[3]) {
    if (!s_fg_on || !rth_record_mode()) return;
    RTH_REC(RTH_FG_SRC, 0, (int32_t)id[0], (int32_t)id[1], (int32_t)id[2],
            x[0], y[0], x[1], y[1], x[2], y[2],
            pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7], pc[8], h[0], h[1], h[2]);
}

/* The share of a game frame the real frame may use so Smooth motion still
 * fills every display interval: (0.85 flip - slots-1 generated frames and
 * their swaps) / flip, never below 0.35. 1 while generation is off or its
 * cost is unknown. Dynamic resolution budgets the real frames with it, so
 * the scale drops until the in-between frames fit (they draw at the same
 * scale, so they get cheaper too). */
double gl_renderer_frame_gen_real_share(void) {
    if (!s_fg_on || !rth_record_mode()) return 1.0;
    const double ghz = atomic_load(&s_fg_guest_hz), rhz = atomic_load(&s_fg_refresh_hz);
    const int vb = atomic_load(&s_fg_flip_vb);
    const double flip_s = (double)vb / (ghz > 1.0 ? ghz : 59.94);
    if (flip_s <= 0.0 || rhz <= 0.0 || vb < 2) return 1.0;
    /* The frames the clock actually shows (fg_slots): 59.94 Hz content on a
     * 60 Hz panel is 2.002 intervals, one in-between frame, not two. */
    const int slots = fg_slots(flip_s, rhz);
    const double ema = atomic_load(&s_fg_cost_ema_pub);
    const double g = ema > 0.0 ? ema + atomic_load(&s_fg_swap_ema) : 0.0;
    if (slots < 2 || g <= 0.0) return 1.0;
    double share = (0.85 * flip_s - (double)(slots - 1) * g) / flip_s;
    if (share < 0.35) share = 0.35;
    if (share > 1.0) share = 1.0;
    return share;
}

void gl_renderer_frame_gen_configure(double refresh_hz, double guest_hz) {
    atomic_store(&s_fg_refresh_hz, refresh_hz);
    if (guest_hz > 1.0) atomic_store(&s_fg_guest_hz, guest_hz);
}

/* Not a breaker trip: generation pauses while the real frames are under
 * pressure (dynamic resolution over budget or stepping down) for the caller's
 * short tail, without the breaker's escalating holds. */
void gl_renderer_frame_gen_hold(GlFgHold kind, double secs) {
    if (!s_fg_on) return;
    const char *reason = kind == GL_FG_HOLD_OVER_BUDGET ? "dynres over budget" : "dynres stepped down";
    /* Reprojected in-between frames cost about a millisecond of the GPU
     * each: real frames over budget (heavy geometry, dynamic resolution at
     * its floor) are not a reason to stop them. A step down still pauses
     * them briefly while its surfaces settle. */
    if (s_fg_reproject && kind == GL_FG_HOLD_OVER_BUDGET) return;
    if (s_fg_reproject && secs > 0.05) secs = 0.05;
    if (secs <= 0.0) secs = 0.1;
    atomic_store(&s_fg_hold_reason, reason);
    const uint64_t until = rt_now_ns() + (uint64_t)(secs * 1e9);
    if (until > atomic_load(&s_fg_hold_until)) atomic_store(&s_fg_hold_until, until);
}

/* Emulation thread, once per frame boundary: the guest is late when it has
 * slipped two intervals behind its own VBlank schedule. One long frame made
 * up by short ones (scheduling jitter, a burst after a wait) is not. */
static void fg_note_guest_frame(void) {
    if (!s_fg_on) return;
    const double ghz = atomic_load(&s_fg_guest_hz);
    if (ghz <= 1.0) return;
    if (fg_pace_note(&s_fg_pace, (double)rt_now_ns() * 1e-9, 1.0 / ghz, 2.0 / ghz))
        atomic_store(&s_fg_late, 1);
}

void gl_renderer_frame_gen_counts(uint64_t *generated, uint64_t *real_presents) {
    if (generated) *generated = s_fg_generated;
    if (real_presents) *real_presents = s_fg_real_presents;
}

void gl_renderer_frame_gen_costs(uint64_t *generated, uint64_t *measured, uint64_t *cost_ns) {
    unsigned before, after;
    uint64_t count, cost;
    do {
        before = atomic_load(&s_fg_gen_cost_seq);
        if (before & 1u) continue;
        count = atomic_load(&s_fg_gen_measured);
        cost = atomic_load(&s_fg_gen_cost_ns);
        after = atomic_load(&s_fg_gen_cost_seq);
        if (before == after) break;
    } while (1);
    if (generated) *generated = atomic_load(&s_fg_generated);
    if (measured) *measured = count;
    if (cost_ns) *cost_ns = cost;
}

int gl_renderer_frame_gen_json(char *out, int cap) {
    const double now = fg_now_s();
    const int open = s_fg_brk_init ? fg_breaker_open(&s_fg_brk, now) : 1;
    return snprintf(out, (size_t)cap,
        "\"enabled\":%d,\"active\":%d,\"forced\":%d,\"generated\":%llu,\"real\":%llu,"
        "\"flips\":%llu,\"dups\":%llu,\"flushed\":%llu,\"not_planned\":%llu,"
        "\"last_n\":%d,\"slots\":%d,\"flip_vblanks\":%d,\"refresh_hz\":%.1f,"
        "\"real_ms\":%.3f,\"gen_ms\":%.3f,\"gen_cpu_ms\":%.3f,\"gen_gpu_ms\":%.3f,\"swap_ms\":%.3f,"
        "\"match_ms\":%.3f,"
        "\"prims\":%u,\"views\":%u,\"pairs\":%u,\"inliers\":%u,\"src_triangles\":%u,"
        "\"breaker_s\":%.2f,\"trips\":%u,\"last_trip\":\"%s\",\"held_s\":%.2f,"
        "\"last_hold\":\"%s\",\"swaps\":%llu,\"gen_samples\":%u,\"gen_cold\":%u,"
        "\"gen_probes\":%u,\"ignored_late\":%llu,\"ignored_bp\":%llu,"
        "\"real_cpu_ms\":%.3f,\"real_gpu_ms\":%.3f,\"ceiling\":%d,"
        "\"trips_late\":%u,\"trips_backed_up\":%u,\"trips_behind\":%u,"
        "\"place_camera\":%u,\"place_object\":%u,\"place_neighbour\":%u,"
        "\"place_unchanged\":%u,\"cam_angle_deg\":%.3f,\"cam_shift\":%.1f,"
        "\"clamped\":%u,\"guessed\":%u,\"verdict_ok\":%d,\"rejected\":%llu,\"reject_why\":\"%s\",\"noplan_why\":\"%s\",\"rp_ms\":[%.3f,%.3f,%.3f],\"rp_zn\":%u,\"reprojected\":%llu,\"reproject_fallback\":%llu,\"reproject\":%d,\"np\":[%llu,%llu,%llu,%llu],\"end_ahead\":%llu,\"end_bp\":%llu,\"end_phase\":%llu",
        s_fg_on, s_fg_on && s_rth_on && open, s_fg_force,
        (unsigned long long)s_fg_generated, (unsigned long long)s_fg_real_presents,
        (unsigned long long)s_fg_flips, (unsigned long long)s_fg_dups,
        (unsigned long long)s_fg_flushed, (unsigned long long)s_fg_skipped_plan,
        s_fg_last_n, s_fg_last_slots, s_fg_flip_vb, atomic_load(&s_fg_refresh_hz),
        s_fg_real_ema * 1e3, fg_cost()->ema * 1e3, s_fg_gen_cpu_ms, s_fg_gen_gpu_ms,
        s_fg_swap_ema * 1e3,
        s_fg_last_match_ms,
        s_fg_fit.prims, s_fg_fit.nviews, s_fg_fit.v[0].pairs, s_fg_fit.v[0].inliers, s_fg_src_seen,
        open ? 0.0 : s_fg_brk.until - now, s_fg_brk.trips,
        s_fg_brk.reason ? s_fg_brk.reason : "",
        (double)(atomic_load(&s_fg_hold_until) > rt_now_ns()
                     ? atomic_load(&s_fg_hold_until) - rt_now_ns() : 0) * 1e-9,
        s_fg_hold_why ? s_fg_hold_why : "", (unsigned long long)s_swaps_total,
        fg_cost()->samples, fg_cost()->discarded, fg_cost()->probes,
        (unsigned long long)s_fg_ignored_late, (unsigned long long)s_fg_ignored_bp,
        s_fg_real_ema * 1e3, s_fg_real_gpu_ema * 1e3, fg_ceil()->cap,
        s_fg_trip_late, s_fg_trip_bp, s_fg_trip_behind,
        s_fg_fit.camera, s_fg_fit.object, s_fg_fit.neighbour, s_fg_fit.unchanged,
        2.0 * acos(fmin(1.0, fabs(s_fg_fit.v[0].q[0]))) * 57.29578,
        sqrt(s_fg_fit.v[0].t[0] * s_fg_fit.v[0].t[0] + s_fg_fit.v[0].t[1] * s_fg_fit.v[0].t[1] +
             s_fg_fit.v[0].t[2] * s_fg_fit.v[0].t[2]),
        s_fg_fit.clamped, s_fg_fit.guessed, s_fg_fit.ok,
        (unsigned long long)s_fg_rejected, s_fg_reject_why ? s_fg_reject_why : "",
        s_fg_noplan_why ? s_fg_noplan_why : "",
        s_rp_t_snap, s_rp_t_z, s_rp_t_w, s_rp_zn,
        (unsigned long long)s_rp_frames, (unsigned long long)s_rp_fallback, s_fg_reproject,
        (unsigned long long)s_fg_np_brk, (unsigned long long)s_fg_np_stale,
        (unsigned long long)s_fg_np_held, (unsigned long long)s_fg_np_room,
        (unsigned long long)s_fg_end_ahead, (unsigned long long)s_fg_end_bp,
        (unsigned long long)s_fg_end_phase);
}

/* ---- the recording vtable ------------------------------------------------ */
#define RTH_DIRECT_OR(rec) do { if (rth_record_mode()) { rec; return; } } while (0)
static void rtb_init(uint16_t *vram) {
    GL_RT_SYNC("init");
    if (s_rth_on) s_rth_vram_pub = vram;
    s_rth_a0_open = 0;   /* a reset abandons an incomplete A0 */
    glb_init(vram);
    if (s_hd_authority_pending) gl_renderer_set_hd_texture_mode(1);
    if (s_rth_on && !rt_held()) rth_rebind_vram(s_rth_vram_priv);
}
static void rtb_set_scale(int sc) { GL_RT_SYNC("set_scale"); glb_set_scale(sc); }
/* The scale changes under a sync point, or by a recorded dynamic-resolution
 * step: while recording, the level as of the recording position. */
static int  rtb_scale(void) {
    return (s_rth_on && !rt_on_render_thread() && !rt_held()) ? s_rthm_scale : s_out_scale;
}
static void rtb_set_texture_filter(int b) { GL_RT_SYNC("texture_filter"); glb_set_texture_filter(b); }
static int  rtb_texture_filter(void) { return s_tex_filter; }
static void rtb_set_dither(int on) {
    RTH_DIRECT_OR(RTH_REC(RTH_DITHER, 0, on ? 1 : 0)); glb_set_dither(on); }
static void rtb_set_semi_transparency(int e, int m) {
    RTH_DIRECT_OR(RTH_REC(RTH_SEMI, 0, e, m)); glb_set_semi_transparency(e, m); }
static void rtb_set_mask_bits(int sb, int cb) {
    RTH_DIRECT_OR(RTH_REC(RTH_MASK, 0, sb, cb)); glb_set_mask_bits(sb, cb); }
static void rtb_set_texture_window(uint32_t r) {
    RTH_DIRECT_OR(RTH_REC(RTH_TWIN, 0, (int32_t)r)); glb_set_texture_window(r); }
static void rtb_set_color_modulation(int r, int g, int b, int raw) {
    RTH_DIRECT_OR(RTH_REC(RTH_MOD, 0, r, g, b, raw)); glb_set_color_modulation(r, g, b, raw); }
static void rtb_set_precise_triangle(int en, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                     int32_t x2, int32_t y2) {
    RTH_DIRECT_OR(RTH_REC(RTH_PRECISE, 0, en, x0, y0, x1, y1, x2, y2));
    glb_set_precise_triangle(en, x0, y0, x1, y1, x2, y2);
}
static void rtb_set_depth_triangle(int en, float z0, float z1, float z2) {
    if (rth_record_mode()) {
        int32_t v[4] = { en, 0, 0, 0 };
        memcpy(&v[1], &z0, 4); memcpy(&v[2], &z1, 4); memcpy(&v[3], &z2, 4);
        rth_rec_ints(RTH_DEPTH, 0, 4, v);
        return;
    }
    glb_set_depth_triangle(en, z0, z1, z2);
}
static void rtb_set_perspective_triangle(int en, float q0, float q1, float q2) {
    if (rth_record_mode()) {
        int32_t v[4] = { en, 0, 0, 0 };
        memcpy(&v[1], &q0, 4); memcpy(&v[2], &q1, 4); memcpy(&v[3], &q2, 4);
        rth_rec_ints(RTH_PERSP, 0, 4, v);
        return;
    }
    glb_set_perspective_triangle(en, q0, q1, q2);
}
static void rtb_fill_rect(int x, int y, int w, int h, uint16_t c) {
    RTH_DIRECT_OR(RTH_REC(RTH_FILL, 0, x, y, w, h, c)); glb_fill_rect(x, y, w, h, c); }
static void rtb_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    RTH_DIRECT_OR(RTH_REC(RTH_COPY, 0, sx, sy, dx, dy, w, h)); glb_copy_rect(sx, sy, dx, dy, w, h); }
static void rtb_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    RTH_DIRECT_OR(RTH_REC(RTH_FLAT_TRI, rth_prim_flags(), x0, y0, x1, y1, x2, y2, c));
    glb_draw_flat_triangle(x0, y0, x1, y1, x2, y2, c);
}
static void rtb_draw_gouraud_triangle(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                                      int x2, int y2, uint16_t c2) {
    RTH_DIRECT_OR(RTH_REC(RTH_GOURAUD_TRI, rth_prim_flags(), x0, y0, c0, x1, y1, c1, x2, y2, c2));
    glb_draw_gouraud_triangle(x0, y0, c0, x1, y1, c1, x2, y2, c2);
}
static void rtb_draw_textured_triangle(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1,
                                       int x2, int y2, int u2, int v2,
                                       uint16_t cx, uint16_t cy, uint16_t tp) {
    RTH_DIRECT_OR(RTH_REC(RTH_TEX_TRI, rth_prim_flags(), x0, y0, u0, v0, x1, y1, u1, v1,
                          x2, y2, u2, v2, cx, cy, tp));
    glb_draw_textured_triangle(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2, cx, cy, tp);
}
static void rtb_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0, uint32_t c0,
                                              int x1, int y1, int u1, int v1, uint32_t c1,
                                              int x2, int y2, int u2, int v2, uint32_t c2,
                                              uint16_t cx, uint16_t cy, uint16_t tp, int raw) {
    RTH_DIRECT_OR(RTH_REC(RTH_SHADED_TEX_TRI, rth_prim_flags(),
                          x0, y0, u0, v0, (int32_t)c0, x1, y1, u1, v1, (int32_t)c1,
                          x2, y2, u2, v2, (int32_t)c2, cx, cy, tp, raw));
    glb_draw_shaded_textured_triangle(x0, y0, u0, v0, c0, x1, y1, u1, v1, c1,
                                      x2, y2, u2, v2, c2, cx, cy, tp, raw);
}
static void rtb_draw_flat_rect(int x, int y, int w, int h, uint16_t c) {
    RTH_DIRECT_OR(RTH_REC(RTH_FLAT_RECT, rth_prim_flags(), x, y, w, h, c));
    glb_draw_flat_rect(x, y, w, h, c);
}
static void rtb_draw_textured_rect(int x, int y, int w, int h, int u, int v,
                                   uint16_t cx, uint16_t cy, uint16_t tp) {
    RTH_DIRECT_OR(RTH_REC(RTH_TEX_RECT, rth_prim_flags(), x, y, w, h, u, v, cx, cy, tp));
    glb_draw_textured_rect(x, y, w, h, u, v, cx, cy, tp);
}
static void rtb_draw_textured_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1, int v1,
                                          uint16_t cx, uint16_t cy, uint16_t tp) {
    RTH_DIRECT_OR(RTH_REC(RTH_TEX_RECT_SCALED, rth_prim_flags(), x, y, w, h, u0, v0, u1, v1,
                          cx, cy, tp));
    glb_draw_textured_rect_scaled(x, y, w, h, u0, v0, u1, v1, cx, cy, tp);
}
static void rtb_draw_line(int x0, int y0, int x1, int y1, uint16_t c) {
    RTH_DIRECT_OR(RTH_REC(RTH_LINE, rth_prim_flags(), x0, y0, x1, y1, c));
    glb_draw_line(x0, y0, x1, y1, c);
}
static void rtb_draw_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1) {
    RTH_DIRECT_OR(RTH_REC(RTH_SHADED_LINE, rth_prim_flags(), x0, y0, c0, x1, y1, c1));
    glb_draw_shaded_line(x0, y0, c0, x1, y1, c1);
}
static int rtb_render_display(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    GL_RT_SYNC("render_display"); return glb_render_display(o, p, dx, dy, dw, dh); }
static int rtb_render_display_hires(uint32_t *o, int p, int dx, int dy, int dw, int dh) {
    GL_RT_SYNC("render_display_hires"); return glb_render_display_hires(o, p, dx, dy, dw, dh); }
static void rtb_vram_write(int x, int y, uint16_t px) {
    RTH_DIRECT_OR(RTH_REC(RTH_VRAM_WRITE, 0, x, y, px)); glb_vram_write(x, y, px); }
static uint16_t rtb_vram_read(int x, int y) { GL_RT_SYNC("vram_read"); return glb_vram_read(x, y); }
static void rtb_vram_transfer_in(int x, int y, int w, int h, const uint16_t *d) {
    if (rth_record_mode()) {
        size_t n = (w > 0 && h > 0) ? (size_t)w * (size_t)h : 0;
        int32_t *p = (int32_t *)rt_cmd_begin(RTH_XFER_IN, 0, (uint32_t)(16u + n * 2u));
        if (p) {
            p[0] = x; p[1] = y; p[2] = w; p[3] = h;
            if (n) memcpy(p + 4, d, n * 2u);
            rt_cmd_commit();
            return;
        }
        gl_rth_acquire("oversize_upload");
    }
    glb_vram_transfer_in(x, y, w, h, d);
}
static void hd_note_exec(int op, int x, int y, int w, int h, int sx, int sy) {
    switch (op) {
    case GR_HD_NOTE_TRACK_UPLOAD: gpu_hd_textures_track_upload(x, y, w, h, NULL); break;
    case GR_HD_NOTE_BEGIN_UPLOAD: gpu_hd_textures_begin_upload(x, y, w, h); break;
    case GR_HD_NOTE_BEGIN_COPY:   gpu_hd_textures_begin_copy(sx, sy, x, y, w, h); break;
    case GR_HD_NOTE_END_COPY:     gpu_hd_textures_end_copy(); break;
    default:                      gpu_hd_textures_invalidate(x, y, w, h); break;
    }
}
/* After the upload/draw it follows, in command order, against s_vram. */
static void rtb_vram_upload_open(int open) {
    s_rth_a0_open = open;
    if (!open && s_hd_authority_pending) gl_renderer_set_hd_texture_mode(1);
    /* gpu.c streams the payload into its array and reads it back for
     * mask-checked words: under HD authority take the context so that array
     * holds every earlier native draw, and keep it until the commit
     * (eligibility). Also when a savestate restores a state mid-A0. */
    if (open && s_hd_native_authority) GL_RT_SYNC("hd_upload");
}
static void rtb_hd_texture_note(int op, int x, int y, int w, int h, int sx, int sy) {
    RTH_DIRECT_OR(RTH_REC(RTH_HD_NOTE, 0, op, x, y, w, h, sx, sy)); hd_note_exec(op, x, y, w, h, sx, sy); }
static void rtb_vram_transfer_out(int x, int y, int w, int h, uint16_t *d) {
    GL_RT_SYNC("vram_transfer_out"); glb_vram_transfer_out(x, y, w, h, d); }
static void rtb_set_draw_area(int x1, int y1, int x2, int y2) {
    if (rth_record_mode()) {
        s_rthm_area[0] = x1; s_rthm_area[1] = y1; s_rthm_area[2] = x2; s_rthm_area[3] = y2;
        RTH_REC(RTH_AREA, 0, x1, y1, x2, y2);
        return;
    }
    glb_set_draw_area(x1, y1, x2, y2);
}
static void rtb_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    if (s_rth_on && !rt_on_render_thread() && !rt_held()) {
        *x1 = s_rthm_area[0]; *y1 = s_rthm_area[1]; *x2 = s_rthm_area[2]; *y2 = s_rthm_area[3];
        return;
    }
    glb_get_draw_area(x1, y1, x2, y2);
}
static void rtb_set_draw_offset(int x, int y) {
    RTH_DIRECT_OR(RTH_REC(RTH_OFFSET, 0, x, y)); glb_set_draw_offset(x, y); }
static void rtb_wide_configure(int wide_w, int offset) {
    if (rth_record_mode()) {
        if (s_raster_ok) {
            if (wide_w <= 0 || wide_w != s_rthm_wide_w) {
                for (int i = 0; i < WIDE_MAX_SURF; i++) s_rthm_base[i] = -1;
                s_rthm_cur = 0;
            }
            s_rthm_wide_w = wide_w > 0 ? wide_w : 0;
            s_rthm_wide_off = wide_w > 0 ? offset : 0;
        }
        RTH_REC(RTH_WIDE_CONFIGURE, 0, wide_w, offset);
        return;
    }
    glb_wide_configure(wide_w, offset);
}
static void rtb_wide_set_view(int en, int shift, int pl, int pr) {
    RTH_DIRECT_OR(RTH_REC(RTH_WIDE_VIEW, 0, en, shift, pl, pr)); glb_wide_set_view(en, shift, pl, pr); }
static void rtb_wide_set_target(int base_x) {
    if (rth_record_mode()) {
        s_rthm_cur = s_raster_ok ? rth_mirror_wide_for(base_x) : 0;
        RTH_REC(RTH_WIDE_TARGET, 0, base_x);
        return;
    }
    glb_wide_set_target(base_x);
}
static void rtb_wide_disable_target(void) {
    if (rth_record_mode()) { s_rthm_cur = 0; RTH_REC(RTH_WIDE_DISABLE, 0, 0); return; }
    glb_wide_disable_target();
}
static void rtb_wide_clear(int base_x, int y, int h, uint16_t color) {
    if (rth_record_mode()) {
        if (s_raster_ok && s_ws_ablate != 1) (void)rth_mirror_wide_for(base_x);
        RTH_REC(RTH_WIDE_CLEAR, 0, base_x, y, h, color);
        return;
    }
    glb_wide_clear(base_x, y, h, color);
}
static void rtb_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    if (rth_record_mode()) {
        if (s_raster_ok && s_ws_ablate != 1 && s_rthm_wide_off > 0)
            (void)rth_mirror_wide_for(base_x);
        RTH_REC(RTH_WIDE_CLEAR_MARGINS, 0, base_x, y, h, color, sides);
        return;
    }
    glb_wide_clear_margins(base_x, y, h, color, sides);
}
static int rtb_render_wide_display(uint32_t *out, int pitch, int base_x, int disp_y, int disp_h) {
    GL_RT_SYNC("render_wide_display");
    return glb_render_wide_display(out, pitch, base_x, disp_y, disp_h);
}
static int rtb_wide_dump_full(uint32_t *out, int cap, int *ow, int *oh, int base_x) {
    GL_RT_SYNC("wide_dump_full");
    return glb_wide_dump_full(out, cap, ow, oh, base_x);
}

static const GpuRenderBackend GL_RT_BACKEND = {
    .name = "opengl",
    .init = rtb_init, .set_scale = rtb_set_scale, .scale = rtb_scale,
    .set_texture_filter = rtb_set_texture_filter, .texture_filter = rtb_texture_filter,
    .set_semi_transparency = rtb_set_semi_transparency, .set_mask_bits = rtb_set_mask_bits,
    .set_texture_window = rtb_set_texture_window, .set_color_modulation = rtb_set_color_modulation,
    .set_precise_triangle = rtb_set_precise_triangle,
    .set_perspective_triangle = rtb_set_perspective_triangle,
    .set_dither = rtb_set_dither,
    .fill_rect = rtb_fill_rect, .copy_rect = rtb_copy_rect,
    .draw_flat_triangle = rtb_draw_flat_triangle, .draw_gouraud_triangle = rtb_draw_gouraud_triangle,
    .draw_textured_triangle = rtb_draw_textured_triangle,
    .draw_shaded_textured_triangle = rtb_draw_shaded_textured_triangle,
    .draw_flat_rect = rtb_draw_flat_rect, .draw_textured_rect = rtb_draw_textured_rect,
    .draw_textured_rect_scaled = rtb_draw_textured_rect_scaled,
    .draw_line = rtb_draw_line, .draw_shaded_line = rtb_draw_shaded_line,
    .render_display = rtb_render_display, .render_display_hires = rtb_render_display_hires,
    .vram_write = rtb_vram_write, .vram_read = rtb_vram_read,
    .vram_transfer_in = rtb_vram_transfer_in, .vram_transfer_out = rtb_vram_transfer_out,
    .set_draw_area = rtb_set_draw_area, .get_draw_area = rtb_get_draw_area,
    .set_draw_offset = rtb_set_draw_offset,
    .wide_configure = rtb_wide_configure,
    .wide_set_view = rtb_wide_set_view,
    .wide_set_target = rtb_wide_set_target,
    .wide_disable_target = rtb_wide_disable_target,
    .wide_clear = rtb_wide_clear,
    .wide_clear_margins = rtb_wide_clear_margins,
    .render_wide_display = rtb_render_wide_display,
    .wide_dump_full = rtb_wide_dump_full,
    .set_depth_triangle = rtb_set_depth_triangle,
    .hd_texture_note = rtb_hd_texture_note,
    .vram_upload_open = rtb_vram_upload_open,
};

/* ---- replay (render thread) ---------------------------------------------- */
static void gl_rth_exec(void *user, const RtCmd *c, const void *payload) {
    (void)user;
    const int32_t *v = (const int32_t *)payload;
    s_rthx_prim = (c->flags & RTHF_PRIM) != 0;
    s_rthx_tagged = (c->flags & RTHF_TAGGED) != 0;
    s_rthx_backdrop = (c->flags >> RTHF_BD_SHIFT) & 3;
    if (c->op == RTH_FRAME) { rthf_end(); return; }
    if (!s_rthf_open && (atomic_load_explicit(&s_rthf_want, memory_order_relaxed) || s_fg_on))
        rthf_begin();
    if (s_fg_on) fg_capture(c, payload);
    switch (c->op) {
    case RTH_SEMI:      glb_set_semi_transparency(v[0], v[1]); break;
    case RTH_DITHER:    glb_set_dither(v[0]); break;
    case RTH_MASK:      glb_set_mask_bits(v[0], v[1]); break;
    case RTH_TWIN:      glb_set_texture_window((uint32_t)v[0]); break;
    case RTH_MOD:       glb_set_color_modulation(v[0], v[1], v[2], v[3]); break;
    case RTH_PRECISE:   glb_set_precise_triangle(v[0], v[1], v[2], v[3], v[4], v[5], v[6]); break;
    case RTH_PERSP: {
        float q[3];
        memcpy(q, &v[1], sizeof q);
        glb_set_perspective_triangle(v[0], q[0], q[1], q[2]);
        break;
    }
    case RTH_DEPTH: {
        float z[3];
        memcpy(z, &v[1], sizeof z);
        glb_set_depth_triangle(v[0], z[0], z[1], z[2]);
        break;
    }
    case RTH_FILL:      glb_fill_rect(v[0], v[1], v[2], v[3], (uint16_t)v[4]); break;
    case RTH_COPY:      glb_copy_rect(v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case RTH_FLAT_TRI:  glb_draw_flat_triangle(v[0], v[1], v[2], v[3], v[4], v[5], (uint16_t)v[6]); break;
    case RTH_GOURAUD_TRI:
        glb_draw_gouraud_triangle(v[0], v[1], (uint16_t)v[2], v[3], v[4], (uint16_t)v[5],
                                  v[6], v[7], (uint16_t)v[8]);
        break;
    case RTH_TEX_TRI:
        glb_draw_textured_triangle(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                                   v[8], v[9], v[10], v[11],
                                   (uint16_t)v[12], (uint16_t)v[13], (uint16_t)v[14]);
        break;
    case RTH_SHADED_TEX_TRI:
        glb_draw_shaded_textured_triangle(v[0], v[1], v[2], v[3], (uint32_t)v[4],
                                          v[5], v[6], v[7], v[8], (uint32_t)v[9],
                                          v[10], v[11], v[12], v[13], (uint32_t)v[14],
                                          (uint16_t)v[15], (uint16_t)v[16], (uint16_t)v[17], v[18]);
        break;
    case RTH_FLAT_RECT: glb_draw_flat_rect(v[0], v[1], v[2], v[3], (uint16_t)v[4]); break;
    case RTH_TEX_RECT:
        glb_draw_textured_rect(v[0], v[1], v[2], v[3], v[4], v[5],
                               (uint16_t)v[6], (uint16_t)v[7], (uint16_t)v[8]);
        break;
    case RTH_TEX_RECT_SCALED:
        glb_draw_textured_rect_scaled(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                                      (uint16_t)v[8], (uint16_t)v[9], (uint16_t)v[10]);
        break;
    case RTH_LINE:      glb_draw_line(v[0], v[1], v[2], v[3], (uint16_t)v[4]); break;
    case RTH_SHADED_LINE:
        glb_draw_shaded_line(v[0], v[1], (uint16_t)v[2], v[3], v[4], (uint16_t)v[5]);
        break;
    case RTH_VRAM_WRITE: glb_vram_write(v[0], v[1], (uint16_t)v[2]); break;
    case RTH_XFER_IN:   glb_vram_transfer_in(v[0], v[1], v[2], v[3], (const uint16_t *)(v + 4)); break;
    case RTH_HD_NOTE:   hd_note_exec(v[0], v[1], v[2], v[3], v[4], v[5], v[6]); break;
    case RTH_AREA:      glb_set_draw_area(v[0], v[1], v[2], v[3]); break;
    case RTH_OFFSET:    glb_set_draw_offset(v[0], v[1]); break;
    case RTH_WIDE_CONFIGURE: glb_wide_configure(v[0], v[1]); break;
    case RTH_WIDE_VIEW: glb_wide_set_view(v[0], v[1], v[2], v[3]); break;
    case RTH_WIDE_TARGET: glb_wide_set_target(v[0]); break;
    case RTH_WIDE_DISABLE: glb_wide_disable_target(); break;
    case RTH_WIDE_CLEAR: glb_wide_clear(v[0], v[1], v[2], (uint16_t)v[3]); break;
    case RTH_WIDE_CLEAR_MARGINS: glb_wide_clear_margins(v[0], v[1], v[2], (uint16_t)v[3], v[4]); break;
    case RTH_PROJ_TRI: {
        PSXProjectedVertex vx[3];
        memcpy(vx, payload, sizeof vx);
        const int32_t *a = (const int32_t *)((const uint8_t *)payload + sizeof vx);
        gl_renderer_draw_projected_triangle(vx, (uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2],
                                            a[3], a[4], a[5]);
        break;
    }
    case RTH_WIDE_RECOVERY: gl_renderer_note_wide_triangle_recovery(v[0]); break;
    case RTH_INTERP_SUSPENDED: gl_renderer_set_interpolation_suspended(v[0]); break;
    case RTH_PRESENT_VRAM:
    case RTH_PRESENT_WIDE: rth_replay_present(c, (const uint8_t *)payload); break;
    case RTH_STATE:
        s_rths_flat_bd = v[0]; s_rths_vp_w = v[1]; s_rths_bg_full = v[2];
        break;
#ifndef PSX_NO_DEBUG_TOOLS
    case RTH_RING_CAPTURE: rth_ring_capture_now(v); break;
#endif
    case RTH_DYN_STEP:
        s_dyn_rth_disp_valid = 1;
        memcpy(s_dyn_rth_disp, v + 1, sizeof s_dyn_rth_disp);
        (void)dyn_apply(v[0]);
        s_dyn_rth_disp_valid = 0;
        break;
    case RTH_PEEK: {
        uint64_t ptr = (uint64_t)(uint32_t)v[4] | ((uint64_t)(uint32_t)v[5] << 32);
        (void)gl_renderer_fbo_peek(v[0], v[1], v[2], v[3], (uint16_t *)(uintptr_t)ptr);
        break;
    }
    default: break;
    }
    s_rthx_prim = 0;
}

static void gl_rth_ctx(void *user, int current) {
    (void)user;
    if (current) {
        SDL_GL_MakeCurrent(s_win, s_ctx);
    } else {
        if (rt_on_render_thread() && s_fg_on) {
            /* The emulation thread takes over (and may draw frames the lists
             * never see): show the waiting real frame now, and start the
             * lists afresh at the next flip after it hands back. */
            fg_flush();
            s_fg_broken = 1;
        }
        glFlush();
        SDL_GL_MakeCurrent(s_win, NULL);
        if (rt_on_render_thread() && s_rthf_open) s_rthf_taint = 1;
    }
}

/* ---- lifecycle / public ---------------------------------------------------- */
extern void gr_refresh_backend(void);   /* gpu_render.c */

int gl_renderer_render_thread_start(int max_frames) {
    if (s_rth_on) return 1;
    if (!s_ctx || !s_raster_ok || !s_vram || !gl_rth_eligible()) return 0;
    uint16_t *priv = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * sizeof(uint16_t));
    if (!priv) return 0;
    /* Cocoa: a background thread's swap must not wait on the main thread,
     * which may itself be waiting on the render thread. */
    SDL_SetHint("SDL_MAC_OPENGL_ASYNC_DISPATCH", "1");
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    memcpy(priv, s_vram, (size_t)VRAM_W * VRAM_H * sizeof(uint16_t));
    s_rth_vram_pub = s_vram;
    s_rth_vram_priv = priv;
    s_rthe_flat_bd = s_rthe_vp_w = s_rthe_bg_full = -1;
    rth_mirror_resync();
    s_vram = priv;
    rth_rebind_vram(priv);
    s_rth_on = 1;
    RtConfig cfg;
    cfg.ring_bytes = (size_t)32u << 20;
    cfg.max_frames = max_frames < 1 ? 1 : max_frames > 8 ? 8 : max_frames;
    cfg.exec = gl_rth_exec;
    cfg.ctx = gl_rth_ctx;
    cfg.user = NULL;
    cfg.tick = fg_tick;
    fg_invalidate();
    (void)pt_begin();   /* [video] present_thread; falls back to direct swaps */
    if (!rt_start(&cfg)) {
        pt_end();
        s_rth_on = 0;
        s_vram = s_rth_vram_pub;
        rth_rebind_vram(s_rth_vram_pub);
        free(priv);
        s_rth_vram_priv = NULL;
        return 0;
    }
    gr_refresh_backend();
    return 1;
}

void gl_renderer_render_thread_stop(void) {
    if (!s_rth_on) return;
    const int held = rt_held();
    rt_stop();                       /* drains; the context is current here again */
    pt_end();
    /* Unheld, the private copy holds the final HD-authoritative draws. */
    if (!held) rth_hd_publish_private();
    s_rth_on = 0;
    s_vram = s_rth_vram_pub;
    rth_rebind_vram(s_rth_vram_pub);
    free(s_rth_vram_priv);
    s_rth_vram_priv = NULL;
    gr_refresh_backend();
}

int gl_renderer_render_thread_active(void) { return s_rth_on; }

/* Frame boundary (once per vblank, after the present): hand the context back
 * to the render thread when the frame may run asynchronously, take it when it
 * may not, then close the frame (the in-flight bound applies here). */
void gl_renderer_render_thread_frame_boundary(void) {
    if (!s_rth_on) return;
    fg_note_guest_frame();
    if (rt_held()) {
        if (gl_rth_eligible()) gl_rth_release();
    } else if (!gl_rth_eligible()) {
        gl_rth_acquire("ineligible");
    }
    /* The render thread's per-frame cost (dynamic resolution) ends here. */
    if (!rt_held() && (atomic_load_explicit(&s_rthf_want, memory_order_relaxed) || s_fg_on))
        RTH_REC(RTH_FRAME, 0, 0);
    rt_frame_end();
}

/* Explicit sync point for callers that read renderer-written host state
 * without calling a GL entry point (debug rings). */
void gl_renderer_render_thread_sync(const char *reason) { GL_RT_SYNC(reason); }

int gl_renderer_render_thread_json(char *out, size_t cap) {
    RtStats st;
    rt_get_stats(&st);
    RtAcquireEvent ev[16];
    int n = rt_acquire_events(ev, 16);
    int k = snprintf(out, cap,
        "\"active\":%d,\"held\":%d,\"max_frames\":%d,\"records\":%llu,\"bytes\":%llu,"
        "\"frames_produced\":%llu,\"frames_consumed\":%llu,\"presents\":%llu,"
        "\"presents_stale\":%llu,\"acquires\":%llu,\"releases\":%llu,\"oversize\":%llu,"
        "\"backpressure_waits\":%llu,\"backpressure_ms\":%.3f,\"ring_full_waits\":%llu,"
        "\"ring_full_ms\":%.3f,\"acquire_ms\":%.3f,\"render_busy_ms\":%.3f,"
        "\"render_idle_ms\":%.3f,\"ring_high_water\":%llu,\"recent_acquires\":[",
        s_rth_on, st.held, st.max_frames, (unsigned long long)st.records,
        (unsigned long long)st.bytes, (unsigned long long)st.frames_produced,
        (unsigned long long)st.frames_consumed, (unsigned long long)s_rth_presents,
        (unsigned long long)s_rth_presents_stale, (unsigned long long)st.acquires,
        (unsigned long long)st.releases, (unsigned long long)st.oversize,
        (unsigned long long)st.backpressure_waits, st.backpressure_ns / 1e6,
        (unsigned long long)st.ring_full_waits, st.ring_full_ns / 1e6,
        st.acquire_ns / 1e6, st.render_busy_ns / 1e6, st.render_idle_ns / 1e6,
        (unsigned long long)st.ring_high_water);
    for (int i = 0; i < n && k > 0 && (size_t)k < cap; i++)
        k += snprintf(out + k, cap - (size_t)k, "%s{\"frame\":%llu,\"wait_us\":%.1f,\"reason\":\"%s\"}",
                      i ? "," : "", (unsigned long long)ev[i].frame, ev[i].wait_ns / 1e3,
                      ev[i].reason);
    if (k > 0 && (size_t)k < cap) k += snprintf(out + k, cap - (size_t)k, "],");
    if (k > 0 && (size_t)k < cap) k += gl_renderer_present_thread_json(out + k, cap - (size_t)k);
    return k;
}

#ifndef PSX_NO_DEBUG_TOOLS
/* Presented-image ring capture for --headless-opengl (main.cpp), at this point
 * of the command stream: the native-wide surface slice when wide != 0, else
 * the display rect at internal resolution. With the render thread recording
 * it is queued (ring readers sync first); a rect the GL surface cannot serve
 * is skipped there instead of resolving through a CPU readback. */
static void rth_ring_capture_now(const int32_t *a) {
    static uint32_t *buf = NULL;
    static size_t cap = 0;
    const uint32_t frame = (uint32_t)a[0];
    const int wide = a[1], base_x = a[2], disp_y = a[3], disp_h = a[4];
    const int cx = a[5], cy = a[6], cw = a[7], ch = a[8];
    size_t need = (size_t)1024 * 4 * 512 * 4;
    if ((size_t)cw * s_out_scale * ch * s_out_scale > need)
        need = (size_t)cw * s_out_scale * ch * s_out_scale;
    if (cap < need) {
        uint32_t *nb = (uint32_t *)realloc(buf, need * 4u);
        if (!nb) return;
        buf = nb; cap = need;
    }
    if (wide) {
        int w = 0, h = 0;
        if (glb_wide_dump_full(buf, (int)cap, &w, &h, base_x) > 0 && h >= 512) {
            const int sc = h / 512, y0 = disp_y * sc, rows = disp_h * sc;
            if (y0 + rows <= h) {
                present_image_ring_push_argb(frame, buf + (size_t)y0 * w, w, rows, w);
                return;
            }
        }
    }
    const int w = cw * s_out_scale, h = ch * s_out_scale;
    int n;
    if (rth_replaying())
        n = (s_raster_ok && !s_depth24_skip_up)
            ? gl_read_display_argb(cx, cy, cw, ch, buf, w * 4, INT_MAX, NULL, NULL) : 0;
    else
        n = gl_renderer_capture_display_hires(buf, w * 4, cx, cy, cw, ch);
    if (n == w * h) present_image_ring_push_argb(frame, buf, w, h, w);
}
void gl_renderer_ring_capture(uint32_t frame, int wide, int base_x, int disp_y, int disp_h,
                              int cx, int cy, int cw, int ch) {
    const int32_t a[9] = { (int32_t)frame, wide, base_x, disp_y, disp_h, cx, cy, cw, ch };
    if (rth_record_mode()) { rth_rec_ints(RTH_RING_CAPTURE, 0, 9, a); return; }
    GL_RT_SYNC("ring_capture");
    rth_ring_capture_now(a);
}
#else
static void rth_ring_capture_now(const int32_t *a) { (void)a; }
#endif

const GpuRenderBackend *gl_backend_get(void) { return s_rth_on ? &GL_RT_BACKEND : &GL_BACKEND; }
