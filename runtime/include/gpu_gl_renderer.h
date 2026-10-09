#ifndef PSX_GPU_GL_RENDERER_H
#define PSX_GPU_GL_RENDERER_H

/* GL backend context + present entry points, called from main.cpp's window
 * setup and present path when [video] renderer = "opengl".  The backend's
 * rasterization vtable is obtained separately via gl_backend_get()
 * (gpu_render.h).  SDL_Window is forward-declared (SDL typedefs it from this
 * same struct tag) so this header needs no SDL include. */

#include <stdint.h>
#include <stddef.h>
#include "gpu_projective_clip.h"

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

/* Create the GL context on a window made with SDL_WINDOW_OPENGL.
 * Returns 1 on success, 0 to fall back to the SDL_Renderer present path. */
int  gl_renderer_init_context(struct SDL_Window *win);
/* Select a retained immutable bank for the next textured submission; zero
 * selects live VRAM. Emulation/GL owning thread only. Returns 0 if unavailable. */
int gl_renderer_select_texture_bank(uint16_t id);
/* Retain texture indices while sampling current guest CLUTs (fades/animation). */
int gl_renderer_select_texture_bank_live_clut(uint16_t id);
int gl_renderer_texture_banks_supported(void);
/* HD texture presentation uses an independent native SW VRAM authority.
 * Both entry points drain pending draws before changing or deleting textures. */
void gl_renderer_set_hd_texture_mode(int enabled);
void gl_renderer_clear_hd_texture_cache(void);

/* Set the GL swap interval / vsync mode (1=vsync, 0=immediate, -1=adaptive).
 * Safe before or after context creation; applies live when a context exists. */
void gl_renderer_set_swap_interval(int interval);
int gl_renderer_get_swap_interval(void); /* Actual driver interval, -2 if unavailable. */

/* Presentation-only temporal blending. High-refresh sub-presents blend the two
 * most recent stable display images on the owning render thread/context; this
 * does not generate motion vectors or true intermediate object positions. */
void gl_renderer_set_interpolation(int enabled, double host_hz, double target_hz,
                                   double source_hz, int blend_mode);
void gl_renderer_set_interpolation_suspended(int suspended);
/* Change only the blend mode (0 linear, 1 change-adaptive, 2 hold), keeping
 * the history and schedule; takes effect at the next present. */
void gl_renderer_set_interpolation_blend(int blend_mode);
/* Blend source: 0 = every guest VBlank is a source frame (default),
 * 1 = only real display flips are (psx_mod_set_frame_interpolation_source). */
void gl_renderer_set_interpolation_source(int source);
void gl_renderer_interpolation_source_diag(int *source, uint32_t *flip_period,
                                           uint64_t *captures,
                                           uint64_t *duplicates);

/* Render passes (render_pass.c; docs/RENDER_PASSES.md). The presenter is
 * ready for passes: GL raster, interpolation live with the FLIP source. */
int      gl_renderer_pass_ready(void);
/* Why not: 0 ready, PSX_MOD_RENDER_PASS_NO_PRESENTER or _BACKEND
 * (mod_plugins.h). A backend mode that cannot host passes reports BACKEND. */
uint32_t gl_renderer_pass_unavailable(void);
/* Debug: make the backend decline passes (PSX_RENDER_PASS_REFUSE=1 at start,
 * render_pass_refuse over TCP), as a renderer mode without them would. */
void     gl_renderer_pass_force_refuse(int on);
/* Phases the presenter will show for the frame the next flip displays. */
uint32_t gl_renderer_pass_plan(uint32_t period_vblanks,
                               uint32_t shown_after_vblanks,
                               uint32_t *alpha_q16, uint32_t max,
                               uint32_t *wanted);
/* Open the VRAM transaction for rect; open_gen first captures the game's own
 * image of it as a new generation. reuse_backup: the caller guarantees that
 * no guest code ran since the previous pass ended (its restore left VRAM as
 * backed up), so the backup of that pass is reused when it covers the same
 * rect at the same scale. Returns 0 when refused (nothing changed). */
int      gl_renderer_pass_begin(int x, int y, int w, int h, int open_gen,
                                uint32_t period_vblanks, int reuse_backup);
/* Snapshot at the last begin attempt, never reconstructed by a TCP query.
 * Strings are static reason names; GL enums are raw numeric values. */
typedef struct GLRenderPassBeginDiag {
    const char *reason;
    const char *resource;
    uint32_t status, fbo_status, gl_error_before, gl_error;
    int active, open_gen, generation, valid, promoted;
    int hr_scale, out_scale, source_path, wide;
    int requested_w, requested_h, capture_w, capture_h;
    int generation_x, generation_y, generation_w, generation_h;
} GLRenderPassBeginDiag;
void gl_renderer_pass_begin_diag(GLRenderPassBeginDiag *out);
/* Generations opened from now on are for a frame already on screen (1) or
 * for the next flip (0, default); render_pass_gen_flip_matches. */
void     gl_renderer_pass_set_flip_shown(int shown);
/* Capture the drawn rect at alpha_q16 (keep) and roll the rect back. */
void     gl_renderer_pass_end(uint32_t alpha_q16, int keep);
uint32_t gl_renderer_pass_leaks(void);
int      gl_renderer_pass_verify_vram(void);
void     gl_renderer_pass_note_cost(uint64_t ticks);
/* Present output deadlines that fell due while guest code ran. */
void     gl_renderer_pass_service_presents(void);
/* promotions, presents, blended presents, expired, unmatched flips, presents
 * made between VBlanks, smoothed pass cost (us), images in the shown frame,
 * presents past the frame's planned end (the next flip was late; the newest
 * image held), stale pass-cost estimates measured again. */
void     gl_renderer_pass_diag(uint64_t out[10]);
/* Always-on snapshot of the last eligible planner call and lifetime count of
 * wanted plans refused with zero measured credit. Times are host milliseconds;
 * reads do not reset counters or invoke the planner. Backend refusals leave
 * this snapshot unchanged. */
typedef struct GLRenderPassPlanDiag {
    uint64_t plans, frame, zero_credit_refusals;
    double idle_ms, present_ms, prior_pass_ms, present_reserve_ms, spare_ms;
    double frame_ms, cost_ms, budget_ms;
    uint32_t wanted, planned;
} GLRenderPassPlanDiag;
void gl_renderer_pass_plan_diag(GLRenderPassPlanDiag *out);
/* Out-of-rect VRAM writes journaled and rolled back (lifetime count). */
uint64_t gl_renderer_pass_journaled(void);
/* Passes that reused the previous pass's VRAM backup (lifetime count). */
uint64_t gl_renderer_pass_backups_reused(void);
/* Pass image textures allocated now (both generations) and their bytes. */
uint32_t gl_renderer_pass_image_textures(uint64_t *bytes);
/* Debug: dump the images of the next `generations` shown frames as PNGs. */
void     gl_renderer_pass_dump_arm(const char *dir, int generations);
/* Netplay local view (psx_mod_render_local_view): the VRAM transaction with
 * no generation. Available only with a presenter surface separate from the
 * authoritative CPU VRAM (dual raster). While it is open, draws reach only
 * that surface. end(keep=1) keeps the rect's presented colour (hr surface and
 * native-wide band) and restores everything else (stencil, raw mirror, CPU
 * rows, out-of-rect journal, coherency state); keep=0 restores all. */
uint32_t gl_renderer_local_view_unavailable(void);
int gl_renderer_local_view_begin(int x, int y, int w, int h);
int gl_renderer_local_view_end(int keep);
/* Stereo shares the VRAM transaction, not the temporal generation/schedule. */
uint32_t gl_renderer_stereo_unavailable(void);
int gl_renderer_stereo_begin(int x, int y, int w, int h, int reuse_backup);
int gl_renderer_stereo_end(uint32_t eye, int keep);
void gl_renderer_stereo_stage_reset(void);
int gl_renderer_stereo_publish(uint64_t pair_id, uint64_t guest_cycle,
                                const int32_t eye_view[2][3]);
void gl_renderer_stereo_reset(void);
int gl_renderer_stereo_set_presentation(uint32_t mode);
void gl_renderer_stereo_dump_arm(const char *dir, int pairs);
/* Bounded metadata for explicitly requested PNG evidence, queried over TCP. */
typedef struct GLRenderStereoCapture {
    uint64_t pair_id, guest_cycle;
    uint32_t width, height;
    int32_t view_offset[2][3];
} GLRenderStereoCapture;
uint32_t gl_renderer_stereo_capture_records(GLRenderStereoCapture *out, uint32_t capacity);
typedef struct GLRenderStereoDiag {
    uint64_t pair_id, guest_cycle, presents;
    uint32_t valid, staged_mask, mode, width, height;
} GLRenderStereoDiag;
void gl_renderer_stereo_diag(GLRenderStereoDiag *out);
uint64_t gl_renderer_perf_ticks(void);
uint64_t gl_renderer_perf_frequency(void);
int gl_renderer_interpolation_owns_cadence(void);
void gl_renderer_interpolation_diag(int *enabled, int *suspended,
                                    int *history_frames,
                                    double *host_hz, double *target_hz,
                                    uint64_t *swaps);
/* Cumulative CPU-upload diagnostics: calls, rects, pixels, conversion ticks,
 * texture-upload ticks, FBO-draw ticks. Active only with PSX_RUNTIME_PERF_DIAG. */
void gl_renderer_runtime_diag(uint64_t out[6]);

/* Present an ARGB8888 image (BGRA byte order) as a letterboxed quad + swap.
 * Used for 24-bit (FMV) frames and the PSX_GL_FORCE_CPU_PRESENT diagnostic.
 * force_4_3 = pillarbox at native 4:3 even on a wide display aspect (FMVs
 * are authored 4:3 and get no GTE squash to compensate the stretch).
 * content_w: if 0 < content_w < src_w, only columns [0, content_w) are shown
 * (left-aligned in the letterbox; the rest stays cleared black). Used to hide
 * a trailing depth24 margin without changing CRTC width / stretching. */
void gl_renderer_present(const uint32_t *pixels, int src_w, int src_h, int linear,
                         int force_4_3, int content_w);

/* Bezel art shown in the letterbox/pillarbox margins. Takes RGBA8 pixels; the
 * caller owns them and may free them on return. Passing NULL clears it.
 * Returns 0 only if a texture could not be created. */
int  gl_renderer_set_bezel(const void *rgba, int w, int h);
int  gl_renderer_has_bezel(void);
/* Post-process anti-aliasing of the composed game image ([video]
 * antialiasing_mode, PSX_AA_MODE). Off by default; OSD and bezel are not
 * filtered and guest VRAM is never touched. Takes effect at the next present;
 * safe to call from any thread. */
enum { GL_POST_AA_OFF = 0, GL_POST_AA_FXAA = 1, GL_POST_AA_FXAA_HQ = 2 };
int  gl_renderer_set_post_aa(int mode);
int  gl_renderer_post_aa(void);
uint64_t gl_renderer_post_aa_passes(void);
double gl_renderer_post_aa_gpu_us(void);   /* debug builds, PSX_POST_AA_TIME=N: mean pass time (us), resets */

/* Clear to black + swap (display-disabled frame). */
void gl_renderer_present_blank(void);

/* §33: re-present the last Live frame captured before Swap (or from a VRAM
 * snapshot when interpolation owned the last present). Used during rollback
 * resim so the window keeps a wall-clock present cadence without reading
 * mid-resim VRAM. Returns 1 if a Swap happened, 0 if no hold is available. */
int gl_renderer_present_hold_last(void);

/* Sync the authoritative FBO down to CPU VRAM if the GPU side is ahead (else
 * a no-op). Screenshots and the debug server call this before reading CPU
 * VRAM. Do NOT use before 24-bit (FMV) scanout — a full readback can clobber
 * packed RGB888 MDEC bytes in the CPU mirror (use flush_cpu_uploads instead). */
void gl_renderer_sync_cpu(void);

/* Land pending CPU→VRAM uploads into the FBO without reading the FBO back.
 * Safe before 24-bit (FMV) CPU scanout of the mirror. */
void gl_renderer_flush_cpu_uploads(void);

/* Mark the whole display dirty, drop present-path latches, reset frame-
 * interpolation history, and force the next several SwapWindow calls even if
 * VRAM tiles match the last present. Call after savestate restore so a
 * reloaded identical frame still reaches the window (double/triple buffer). */
void gl_renderer_invalidate_present(void);

/* After savestate restore: push the CPU VRAM mirror into the GL FBO. Needed
 * when the load happened while GP1 depth24 was on — the normal path skips
 * framebuffer-sized uploads, which also skipped the full-VRAM boot_state
 * blit and left post-FMV menus without texture pages. */
void gl_renderer_restage_vram_after_savestate(void);

/* Netplay dual-raster: every GP0 also writes software VRAM @ 1× (snaps /
 * digests / GPUREAD authority) while the OpenGL hr FBO keeps settings-scale
 * SSAA for present-only. Never enables glReadPixels; CPU stays current. */
void gl_renderer_set_cpu_auth_dual(int on);

/* FMV present reconstruction, settings.toml [video] fmv_filter. Takes the
 * config enum VIDEO_FMV_FILTER_* (0 nearest, 1 bilinear, 2 sharp, 3 bicubic).
 * Only consulted while video antialiasing is on; AA off is always nearest. */
void gl_renderer_set_fmv_filter(int cfg_value);
/* [video] fmv_chroma_smoothing: on 24-bit (MDEC) presents, keep each sample's
 * luma and smooth its chroma over the 3x3 source pixels around it, hiding the
 * 2x2 colour blocks of 4:2:0 video. Off by default. main.cpp notes whether
 * the next gl_renderer_present is a 24-bit scanout. */
void gl_renderer_set_fmv_chroma_smoothing(int on);
int  gl_renderer_fmv_chroma_smoothing(void);
uint64_t gl_renderer_fmv_chroma_frames(void);
void gl_renderer_note_present_depth24(int d24);
int  gl_renderer_cpu_auth_dual(void);

/* Post-savestate freeze probe: skip/swap/dirty-mark counters (GL present path).
 * take() returns deltas since the previous take/reset. Safe no-ops when GL is
 * inactive. rect_dirty tests the current present-tile dirty bits. */
void gl_renderer_present_probe_reset(void);
void gl_renderer_present_probe_take(uint64_t *skip_delta, uint64_t *swap_delta,
                                    uint64_t *dirty_mark_delta,
                                    int *force_remaining);
int  gl_renderer_present_rect_dirty(int disp_x, int disp_y, int w, int h);

/* THE present path for 15-bit frames: blit the display region straight from
 * the authoritative VRAM FBO into a letterboxed rect (no readback).
 * Deterministic — used for every 15-bit frame. linear = filter on scale.
 * force_4_3 pins to native 4:3 (15-bit MDEC FMV frames on a wide aspect). */
void gl_renderer_present_vram(int disp_x, int disp_y, int w, int h, int linear,
                              int force_4_3);

/* GPU-direct native-wide present: blit the displayed buffer's wide FBO (key =
 * disp_x) straight to the window, no CPU readback. Returns 0 if no wide surface
 * exists for disp_x (caller falls back to the CPU readout path). */
int gl_renderer_present_wide_fbo(int disp_x, int disp_y, int disp_h, int linear);

/* Display aspect for the present letterbox (default 4:3). A wide aspect
 * stretches the 4:3 frame; pair with gte_set_display_aspect (cpu_state.h)
 * for the widescreen field-of-view hack. */
void gl_renderer_set_display_aspect(int num, int den);

/* Scanline post-process (host display setting). on toggles the effect; strength
 * (0..1) is the depth of the dark gap between PS1 scanlines. Applied at the
 * native display-line pitch in the present/interpolation shaders, and faded in
 * with output scale so it never shimmers on a sub-2x window. gl_renderer_get_
 * scanlines returns the on flag and (via out-param) the current strength. */
void gl_renderer_set_scanlines(int on, float strength);
int  gl_renderer_get_scanlines(float *strength);

/* Presentation-only gamma adjustment. gamma = 1.0 is the identity; values
 * above 1.0 lift shadow detail and values below 1.0 darken it. The adjustment
 * is applied once to game content in the final GL presentation pass, including
 * temporal interpolation, but not to the bezel, host OSD, black margins, or an
 * already-composed hold-last image. Non-finite and out-of-range values are
 * clamped to a safe range. Safe to call before GL context creation. */
void  gl_renderer_set_post_gamma(float gamma);
float gl_renderer_get_post_gamma(void);

/* Select full native-wide mirror rendering instead of the centre-splice fast
 * path. Textured edge expansion needs the complete mirror surface. */
void gl_renderer_set_wide_fast(int on);
/* PGXP renderer features (docs/ENHANCEMENTS.md G1.14), all off by default:
 * depth buffer for opaque 3D polygons, perspective-correct Gouraud colour,
 * seam expansion (0 off, 1 = 1 output px above 1x, 2 = 0.5 native px),
 * and the depth clear threshold in SZ units (DuckStation's default 4096). */
/* Nonzero when a PGXP renderer feature is on: only then does gpu.c send
 * gr_set_depth_triangle (no extra command per triangle otherwise). Any thread. */
int  gl_renderer_pgxp_render_wanted(void);
void gl_renderer_set_pgxp_depth(int on);
/* [video] texture_lod / anisotropic_filtering: mip-level emulation (box
 * average of the screen footprint, clamped to the primitive's UV bounds) for
 * minified world textures, with up to `aniso` taps along the footprint's
 * major axis. mode 0 (default) leaves sampling exactly as before. */
void gl_renderer_set_texture_lod(int mode, int aniso);
int  gl_renderer_texture_lod(void);
int  gl_renderer_anisotropy(void);
int  gl_renderer_get_pgxp_depth(void);
void gl_renderer_set_pgxp_color_perspective(int on);
int  gl_renderer_get_pgxp_color_perspective(void);
void gl_renderer_set_pgxp_seam(int mode);
int  gl_renderer_get_pgxp_seam(void);
void gl_renderer_set_pgxp_depth_threshold(float sz);
/* Cumulative depth clears by cause: config, drawing-area change, fill,
 * average-SZ jump-back threshold, displayed frame. */
void gl_renderer_pgxp_depth_clear_reasons(uint64_t out[5]);
void gl_renderer_pgxp_render_stats(uint64_t *depth_tris, uint64_t *depth_clears,
                                   uint64_t *seam_tris);

/* Internal-resolution scale state of the live GL context (0 before init). */
typedef struct GlScaleInfo {
    int requested;      /* scale requested before context init (gr_set_scale) */
    int effective;      /* scale rendered at now (the hr surface's allocation
                         * unless dynamic resolution stepped below it) */
    int max_scale;      /* largest scale this context could allocate */
    int max_dim;        /* min(GL_MAX_TEXTURE_SIZE, _RENDERBUFFER_SIZE, _VIEWPORT_DIMS) */
    int clamp_reason;   /* PSX_GL_SCALE_* mask (gl_scale_limits.h) */
    int alloc_retries;  /* FBO allocations that failed and stepped the scale down */
    int budget_mib;     /* memory budget for the hr surface (0 = none) */
    int fbo_w, fbo_h;   /* hr surface size in pixels */
    int drawable_w, drawable_h; /* window drawable in pixels */
    /* Windowed high-resolution mode (true 8K on 16384-limit GPUs): the hr
     * surface stays at hr_scale (1) and the displayed columns are kept at
     * `effective` scale in window_tiles surfaces (more than one when
     * side-by-side buffers are too wide for one). window_x/_w/_fbo_* describe
     * the tile holding the last presented display. */
    int hr_scale;
    int windowed;
    int window_x, window_w;
    int window_fbo_w, window_fbo_h;
    int window_grows;   /* tile allocations so far */
    int window_tiles;
    int window_mib;     /* all tiles */
    /* Dynamic resolution: the scale the hr and native-wide surfaces are
     * allocated at (the ceiling a step never exceeds); 0 when steps are not
     * available (off, the window mode, dual raster). */
    int alloc_scale;
} GlScaleInfo;
int gl_renderer_scale_info(GlScaleInfo *out);

/* ---- Dynamic internal resolution ([video] dynamic_resolution) ---------------
 * Opt-in. On, the hr and native-wide surfaces stay allocated at the scale
 * context init gave them (the ceiling) and the internal scale may step to any
 * integer level from 1 to it while the game runs; off (the default), nothing
 * changes. A step keeps the guest-visible VRAM exact (the pack of every native
 * block is preserved) and is presentation-only. Not available in the windowed
 * high-resolution mode or with dual raster (netplay): the scale stays fixed. */
void gl_renderer_set_dynamic_resolution(int on);   /* any time; before init too */
/* The ceiling a level may step up to, or 0 when steps are not available. */
int  gl_renderer_dynamic_resolution_ceiling(void);
/* Ask for a level: applied at the next post-present point outside a render
 * pass (clamped to 1..ceiling). Returns 0 when steps are not available. */
int  gl_renderer_request_internal_scale(int scale);
/* Step now (debug probes and tests): 1 when the level is `scale` on return,
 * 0 when refused or deferred (inside a render pass; it then applies after
 * the next present). Never call it inside a render pass. */
int  gl_renderer_step_internal_scale_now(int scale);
typedef struct GlDynresStats {
    int      level, ceiling, pending;   /* pending: requested level, 0 = none */
    uint64_t steps, deferred, refused;
    int      last_from, last_to;
    /* The last step's wall time (ms, glFinish'ed when timing is on) and its
     * parts: draining queued work, the 1x top-left image and the saved rects,
     * the reseed, the rects back, the native-wide surfaces. */
    double   last_ms, last_prep_ms, last_seed_ms, last_rects_ms, last_wide_ms;
    int      last_rects;                /* rects kept at full detail */
    uint64_t hr_reallocs, wide_reallocs; /* surfaces reallocated at a step */
} GlDynresStats;
void gl_renderer_dynres_stats(GlDynresStats *out);
/* Frame generation ([video] frame_generation, docs/FRAME_GENERATION.md):
 * in-between frames drawn by the render thread from the last two game
 * frames' draw lists when it has time to spare. Off by default; with it off
 * nothing changes. set: a sync point. configure: the display's refresh and
 * the guest's frame rate (any thread). hold: pause generation for `secs`
 * (not a breaker trip), e.g. while dynamic resolution is over budget.
 * json: the debug server's {"cmd":"frame_gen"} fields (no sync point). */
void gl_renderer_set_frame_generation(int on);
/* The sources of the next triangle's vertices (gte_fg_source_lookup: id,
 * camera-space x/y/z, projection distance H; id 0 = none) and the integer
 * positions it will be drawn at; recorded for frame generation only, never
 * drawn. */
void gl_renderer_fg_source(const uint32_t id[3], const int32_t pc[9], const int32_t h[3],
                           const int32_t x[3], const int32_t y[3]);
int  gl_renderer_frame_generation(void);
void gl_renderer_frame_gen_configure(double refresh_hz, double guest_hz);
double gl_renderer_frame_gen_real_share(void);
/* How in-between frames are made ([video] frame_generation_method,
 * docs/FRAME_GENERATION.md): redraw (default) or reprojection (opt-in). */
enum { GL_FG_METHOD_REDRAW = 0, GL_FG_METHOD_REPROJECTION = 1 };
void gl_renderer_set_frame_generation_method(int method);
int  gl_renderer_frame_generation_method(void);
/* Why the real frames want generation paused (dynamic resolution). */
typedef enum { GL_FG_HOLD_STEP_DOWN = 0, GL_FG_HOLD_OVER_BUDGET = 1 } GlFgHold;
void gl_renderer_frame_gen_hold(GlFgHold kind, double secs);
int  gl_renderer_frame_gen_json(char *out, int cap);
/* Smooth motion's atomically published running totals, for rate readouts. */
void gl_renderer_frame_gen_counts(uint64_t *generated, uint64_t *real_presents);
/* Generated frames so far, how many of them were timed, and the timed ones'
 * summed cost (ns, GPU time or CPU wall, the larger): dynamic resolution
 * counts what Smooth motion actually spent in its load. The measured count
 * and summed cost are a coherent pair; generated count advances independently
 * because GPU measurements may arrive later. This never stalls the GL queue. */
void gl_renderer_frame_gen_costs(uint64_t *generated, uint64_t *measured, uint64_t *cost_ns);
/* Host time the renderer spent, as running totals in performance-counter
 * ticks (only kept while dynamic resolution is on): waits for the frame
 * blend's next present, render passes, blend presents' own work, and time
 * blocked in the swap. The dynamic-resolution controller differences them per
 * guest interval. */
typedef struct GlHostLedger {
    uint64_t idle_ticks, pass_ticks, interp_work_ticks, swap_ticks;
    uint64_t interp_presents, swaps;
} GlHostLedger;
void gl_renderer_host_ledger(GlHostLedger *out);
/* Render thread ([video] render_thread): the cost of each guest frame it
 * replayed, max(GPU time, its CPU time without idle waits and the swap), as
 * running totals the emulation thread differences. Measured only while
 * gl_renderer_render_thread_measure(1) (dynamic resolution on). */
typedef struct GlRthCosts {
    uint64_t frames, cost_ns, cpu_ns, gpu_ns, gpu_frames, dropped;
} GlRthCosts;
void gl_renderer_render_thread_measure(int on);
void gl_renderer_render_thread_costs(GlRthCosts *out);

/* Narrow a native-wide display aspect num:den to the widest whose surface
 * this context can allocate at its internal scale (psx_gl_fit_wide_aspect;
 * disp_w is the display width the margins derive from). Returns 1 when it
 * changed num:den; 0, leaving it alone, before init or with no known limit. */
int gl_renderer_fit_wide_aspect(int disp_w, int *num, int *den);

/* Read the display rect at internal resolution from the hr FBO: (w*S) x (h*S)
 * ARGB8888, top row first. Returns the pixel count, 0 if unavailable or larger
 * than cap_px. Debug/verification only (screenshot_hires under GL). */
int gl_renderer_read_display_hires(int x, int y, int w, int h, uint32_t *out,
                                   int cap_px, int *ow, int *oh);
/* Capture the presented OpenGL surface at internal resolution with an output
 * pitch, without writing CPU VRAM (the --headless-opengl presentation ring).
 * depth24 scanout, or a rect the GL surface cannot serve, falls back to the
 * CPU-mirror resolve. The backend's render_display_hires is unchanged. */
int gl_renderer_capture_display_hires(uint32_t *out, int pitch_bytes,
                                      int x, int y, int w, int h);

void gl_renderer_shutdown(void);

/* Diagnostics (debug server): read GPU-side VRAM without touching the CPU
 * array; report coherency flags + dirty rects. fbo_peek returns 0 when the
 * GL pipeline is inactive. */
int  gl_renderer_fbo_peek(int x, int y, int w, int h, uint16_t *out);
void gl_renderer_diag(int *gpu_dirty, int pending[5], int pack[5]);

/* Always-on coherency event ring (debug server "gl_coh_ring"): every upload
 * flush, fill, copy, draw bbox, pack, full readback, present, and probe
 * perturbation, with rect + frame. An op that flushes internally records its
 * own event AFTER the FLUSH it caused (the event after a FLUSH = trigger). */
enum {
    GL_COH_FLUSH    = 1,   /* CPU->FBO upload flush (pending box)     */
    GL_COH_FILL     = 2,   /* GP0(02) fill rect                       */
    GL_COH_COPY_SRC = 3,   /* GP0(80) copy, source rect               */
    GL_COH_COPY     = 4,   /* GP0(80) copy, dest rect                 */
    GL_COH_DRAW     = 5,   /* drawn prim bbox (clipped to draw area)  */
    GL_COH_PACK     = 6,   /* hr FBO -> raw mirror pack (dirty box)   */
    GL_COH_ENSURE   = 7,   /* full FBO -> CPU VRAM readback           */
    GL_COH_PRESENT  = 8,   /* 15-bit present blit (display rect)      */
    GL_COH_UPLOAD   = 9,   /* bulk CPU->VRAM transfer_in dest rect    */
    GL_COH_PEEK     = 10,  /* gl_fbo_peek probe (perturbs: flushes)   */
    GL_COH_DIFF     = 11,  /* gl_vram_diff probe (perturbs: flushes)  */
};

typedef struct {
    uint32_t frame;
    uint8_t  kind;
    int16_t  x0, y0, x1, y1;   /* native VRAM coords, inclusive */
} GlCohEvent;

uint64_t gl_renderer_coh_total(void);
/* Fetch event by absolute sequence number; 0 if evicted or out of range. */
int gl_renderer_coh_get(uint64_t seq, GlCohEvent *out);

/* Always-on present ring (debug server "gl_present_ring"): EVERY SwapWindow —
 * including blank (display-disabled) and CPU-quad presents, which the coherency
 * ring does not record — with the path taken, source display rect, letterbox
 * dest rect, a glGetError sample, wall-clock ms, and a backbuffer pixel sampled
 * at the letterbox centre right before the swap (the ground truth for "did this
 * swap present black"). */
enum {
    GL_PRES_VRAM  = 0,   /* 15-bit FBO blit present (gl_renderer_present_vram) */
    GL_PRES_WIDE  = 1,   /* native-wide FBO blit present                       */
    GL_PRES_CPU   = 2,   /* CPU-readout quad present (24-bit FMV / forced)     */
    GL_PRES_BLANK = 3,   /* display-disabled black present                     */
    GL_PRES_INTERP = 4,  /* host-refresh interpolation sub-present              */
    GL_PRES_STEREO = 5,  /* simultaneous left/right side-by-side pair           */
};

typedef struct {
    uint32_t frame;        /* s_frame_count at swap                        */
    uint32_t t_ms;         /* SDL_GetTicks() at swap                       */
    uint8_t  path;         /* GL_PRES_*                                    */
    uint8_t  px_r, px_g, px_b; /* backbuffer sample at letterbox centre    */
    uint16_t glerr;        /* glGetError() drained just before the swap    */
    int16_t  dx, dy, w, h; /* source display rect (native px; 0 for blank) */
    int16_t  lx, ly, lw, lh; /* letterbox dest rect (window px)            */
    uint8_t  src_r, src_g, src_b, src_valid; /* blit SOURCE (hr FBO) sample
                                              * at the display-rect centre  */
} GlPresEvent;

uint64_t gl_renderer_pres_total(void);
int gl_renderer_pres_get(uint64_t seq, GlPresEvent *out);

/* frame_perf: aggregate the per-frame GPU/CPU phase-timing ring (debug server
 * "frame_perf"). wide_filter: -1 = all frames, 0 = 4:3 present, 1 = native-wide.
 * Fills out[13]: [0]=count, [1]=total_ms avg, [2]=total_ms max, [3]=emu_cpu_ms avg
 * (frame minus the present call), [4]=present_wall_ms avg, [5]=scene_gpu_ms avg,
 * [6]=scene_gpu_ms max, [7]=present_gpu_ms avg, [8]=present_gpu_ms max,
 * [9]=scene primitives/frame avg (pre double-draw), [10]=mirror_gpu_ms avg (of
 * scene_gpu, the native-wide mirror passes; GL_TIMESTAMP pairs), [11]=mirror_gpu_ms
 * max, [12]=mirror passes/frame avg, [13]=CPU wall in flush_tex_batch avg,
 * [14]=CPU wall in glb_wide_* avg, [15]=batches/frame avg, [16]=wide target
 * sets/frame avg, [17]=wide FBO creations/frame avg. GPU phases are true
 * GL_TIME_ELAPSED times (CPU-overhead independent). Returns the count. */
int gl_renderer_perf_aggregate(int wide_filter, double out[18]);

/* Native-wide mirror ablation (perf attribution, debug cmd gl_ws_ablate):
 * 0 = normal, 1 = skip the whole mirror pass (incl. wide_clear), 2 = full mirror
 * state churn without the draw calls, 3 = mirror draws stay on the hr FBO (no
 * per-pass FBO rebind; diagnostic only — corrupts both surfaces' content). */
void gl_renderer_set_ws_ablate(int mode);
int  gl_renderer_get_ws_ablate(void);

/* Cumulative textured fraction of scene primitives since boot (flat vs textured
 * batching decision). Sets *out_tex_frac; returns total prim count. */
uint64_t gl_renderer_perf_prim_split(double *out_tex_frac);
/* Counts proven projected world triangles admitted past the hardware size
 * reject. Diagnostic only; they use the normal canonical and mirror passes. */
void gl_renderer_note_wide_triangle_recovery(int enabled);
uint64_t gl_renderer_wide_triangle_recovery_count(void);
/* Exact-provenance world polygons only. Returns zero when the GPU context or
 * authority policy cannot support clipping; the caller then uses its stock path. */
int gl_renderer_projective_supported(void);
void gl_renderer_draw_projected_triangle(const PSXProjectedVertex vertices[3],
    uint16_t texpage, uint16_t clut_x, uint16_t clut_y, int raw, int semi,
    int perspective);
/* Cumulative textured-batch diagnostics: total, then flushes caused by
 * isolation, blend-mode, mask, filter, backdrop-gate, texture-window, capacity. */
void gl_renderer_batch_diag(uint64_t out[9]);
/* Cumulative flushes of non-empty batches by caller source line in
 * gpu_gl_renderer.c (kind 0 textured, 1 flat/gouraud). Returns entries
 * written; diff two reads to attribute a window. */
int gl_renderer_batch_sites(int kind, int *lines, uint64_t *counts, int cap);

/* Texture-window batching ([video] texture_window_batching; default off).
 * On: textured prims with different GP0(E2h) texture windows share a batch
 * (each vertex carries its window), except while mask checking is on. The
 * image is the same either way; only the number of draws changes. Safe to
 * call before GL context creation. */
void gl_renderer_set_texture_window_batching(int on);
int  gl_renderer_get_texture_window_batching(void);

/* Render thread ([video] render_thread, docs/RENDER_THREAD.md). Opt-in, off
 * by default; with it off nothing below runs and the backend is unchanged.
 * start: after gl_renderer_init_context, on the thread holding the context;
 * moves the context to a new render thread and records backend calls from
 * here on (1 = running). max_frames bounds closed frames in flight. stop:
 * drain and take the context back (gl_renderer_shutdown also stops it).
 * frame_boundary: once per vblank after the present; hands the context to or
 * from the render thread and applies the in-flight bound. sync: an explicit
 * sync point for readers of renderer-written host state outside the GL entry
 * points. json: stats object body for the render_thread debug command. */
int  gl_renderer_render_thread_start(int max_frames);
void gl_renderer_render_thread_stop(void);
int  gl_renderer_render_thread_active(void);
void gl_renderer_render_thread_frame_boundary(void);
void gl_renderer_render_thread_sync(const char *reason);
int  gl_renderer_render_thread_json(char *out, size_t cap);
/* [video] present_thread (docs/RENDER_THREAD.md "Present thread"): set before
 * gl_renderer_render_thread_start; with the render thread, composed frames go
 * to `slots` (2..4) offscreen slots and a present thread with a shared context
 * does the copy + swap. Falls back to direct swaps when it cannot start.
 * json: the "present_thread" member for the render_thread debug command. */
void gl_renderer_set_present_thread(int on, int slots);
int  gl_renderer_present_thread_active(void);
int  gl_renderer_present_thread_json(char *out, size_t cap);
int  gl_renderer_fbo_peek_deferred(int x, int y, int w, int h, uint16_t *out);
void gl_renderer_ring_capture(uint32_t frame, int wide, int base_x, int disp_y, int disp_h,
                              int cx, int cy, int cw, int ch);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_GL_RENDERER_H */
