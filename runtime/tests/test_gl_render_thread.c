/* Render thread equivalence on a real, hidden OpenGL context.
 *
 * Run twice per scale by run_gl_render_thread.py, once with the render thread
 * off (argv[2] == 0) and once on (1); the runner requires every digest to
 * match. The scripted stream goes through the renderer facade (gr_*), exactly
 * as gpu.c drives it, over many frames:
 *   - CPU->VRAM uploads written the way gpu.c writes them (the guest-visible
 *     array first, then the facade with a copy), including the same rect
 *     re-uploaded with new data before the render thread can have drawn the
 *     first (the render thread must stage each payload, not the live array);
 *     full-VRAM uploads push the ring past its end (wrap);
 *   - GP0(E6) mask set / check around draws, and a mask-checked CPU upload
 *     whose per-pixel mask test reads VRAM back (a sync point mid-frame);
 *   - fills, VRAM copies, render-to-texture, every semi-transparency mode,
 *     lines, textured and shaded primitives, draw area / offset changes;
 *   - native-wide: margin draws, wide clears, and per-primitive widescreen
 *     tags that change with every primitive (captured at record time) plus a
 *     stream latch that flips mid-frame (sent as state);
 *   - presents and frame boundaries every frame, with readbacks (GPUREAD
 *     style gr_vram_read, VRAM->CPU transfer_out) in some frames only.
 * With argv[4] == "dynres" ([video] dynamic_resolution) the surfaces are
 * allocated at the scale and the level steps between frames
 * (gl_renderer_step_internal_scale_now after a frame boundary, as the
 * controller does): recorded in stream order with the thread on, run at once
 * with it off, or at once when the frame is held at a sync point; the
 * render thread's frame cost is measured too. Back at the ceiling before the
 * final readbacks.
 * Printed: rb (digest of every value the "guest" read back), digest (native
 * VRAM at the end), hires (frame at internal resolution), wide (native-wide
 * surface), and with the thread on its record/acquire counters.
 * Original source-owned scene; no retail payload. */
#include "gpu_gl_renderer.c"
#include "gpu_hd_texture_stubs.inc"
#include "mod_texture_banks.c"
#include "gl_batch_policy_lle.c"
#include "gl_batch_policy_hle.c"
#include "gl_batch_policy_select.c"
/* gpu_render.c (the facade) observes draws for HD texture packs: no pack here. */
int gpu_hd_textures_active(void){return 0;}
int gpu_hd_textures_dump_enabled(void){return 0;}
void gpu_hd_textures_observe_draw(uint16_t tp,uint16_t cx,uint16_t cy,const int l[4],
    uint32_t w,int semi){(void)tp;(void)cx;(void)cy;(void)l;(void)w;(void)semi;}
void gpu_hd_textures_set_vram(const uint16_t* v){(void)v;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t* words){
    (void)x;(void)y;(void)w;(void)h;(void)words;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int dx,int dy,int w,int h){
    (void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
/* mod_texture_banks.c refuses inside a netplay host-output hook (mod_runtime.cpp); none here. */
int psx_mod_host_output_scope(void){return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
int g_psx_vram_dirty_tracking=0;
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){(void)y;}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
const GpuRenderBackend *vk_backend_get(void){return NULL;}
int gpu_display_is_depth24(void){return 0;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->width=320;out->height=240;}
/* Guest state the backend reads per primitive. It changes with every
 * primitive here, so a replay that read it live (instead of the value
 * captured when the primitive was recorded) would draw a different image. */
static int g_prim_id;
static int g_bg_full;
int psx_ws_prim_in_backdrop(void){return (g_prim_id % 3) == 0;}
int psx_ws_prim_is_tagged(void){return (g_prim_id % 5) == 0;}
int gpu_ws_nw_flat_backdrop_enabled(void){return 0;}
int gpu_ws_background_requires_full_composite(void){return g_bg_full;}
int g_ws_tex_edge_pct=0;
void psx_ws_dbg_gate_frame_snapshot(void){}
void gpu_depth24_upload_span_reset(void){}
int gpu_ws_netplay_local_viewport_width(void){return 0;}
int render_pass_netplay_enabled(void){return 0;}
uint64_t psx_cycle_count=0;
uint32_t g_psx_vblank_cycles=564480u;
/* Frontend hooks of the present path (host overlays, pacing, rings). */
int host_osd_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
int host_osd_volume_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
int host_osd_needs_present(void){return 0;}
void host_osd_present_done(void){}
int psx_rewind_overlay_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
float psx_rewind_slide(void){return 0.0f;}
int psx_savestate_menu_overlay_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
int psx_host_menu_overlay_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
void psx_video_telemetry_compose(int r,int s,int sw,int sh,int dw,int dh,int vw,int vh){
    (void)r;(void)s;(void)sw;(void)sh;(void)dw;(void)dh;(void)vw;(void)vh;
}
void psx_video_telemetry_present(int result){(void)result;}
int present_shot_take(char *out,int n){(void)out;(void)n;return 0;}
void present_shot_done(int ok){(void)ok;}
void psx_host_sleep_ms(uint32_t ms){(void)ms;}
void psx_host_sleep_micros(unsigned us){(void)us;}
/* Called on every present. On the render thread it also stands in for a slow
 * swap, so replay runs well behind recording and anything it read live from
 * the "guest" (instead of what was recorded) would be a later value. */
int psx_present_vsync_owns_cadence(void){
    if (rt_on_render_thread()) SDL_Delay(4);
    return 0;
}
void latency_ring_mark(LatencyStage s){(void)s;}
void gpu_timeline_note(uint8_t k,uint32_t a,uint32_t b){(void)k;(void)a;(void)b;}

static uint16_t vram[1024*512];
static int checks, failures;
static void check(int ok, const char *label) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}
/* Force the real allocation-failure branch without exhausting VRAM. The
 * fixture includes the backend, so no production fault-injection API is needed. */
static void (APIENTRY *saved_renderbuffer_storage)(GLenum, GLenum, GLsizei, GLsizei);
static void APIENTRY refuse_renderbuffer_storage(GLenum target, GLenum format,
                                                  GLsizei w, GLsizei h) {
    (void)format;
    saved_renderbuffer_storage(target, 0 /* invalid format */, w, h);
}
static void test_live_resolution(int original_scale) {
    check(gl_renderer_set_internal_scale_live(2) == 2, "live scale A applied");
    check(gl_renderer_set_internal_scale_live(1) == 1, "live A to B applied");
    gl_renderer_render_thread_sync("fixture_allocation_refusal");
    saved_renderbuffer_storage = p_glRenderbufferStorage;
    p_glRenderbufferStorage = refuse_renderbuffer_storage;
    check(gl_renderer_set_internal_scale_live(2) == 0, "live B to A allocation refused");
    p_glRenderbufferStorage = saved_renderbuffer_storage;
    check(s_hr_scale == 1 && s_req_scale == 1 && s_alloc_scale == 1,
          "refusal preserves applied scale and ceiling");
    check(!s_dyn_pending, "refused live request cannot apply later");
    check(gl_renderer_set_internal_scale_live(original_scale) == original_scale,
          "live resolution recovers after refusal");
}
static uint64_t fixture_clock_us(void) {
    const uint64_t now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
    return now / hz * 1000000u + now % hz * 1000000u / hz;
}
static int drawable_worker(void *data) {
    int *size = data;
    gl_drawable_size(&size[0], &size[1]);
    return 0;
}
static uint64_t fnv(const void *pp, size_t n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)pp;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
static uint64_t rb_digest = 0xcbf29ce484222325ull;
static int wide_present;
static int quality_scene;
static uint32_t rng = 0x2468ace1u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* gpu.c's CPU->VRAM path: write the guest-visible array (honouring the mask
 * test, which reads VRAM through the facade), then hand the facade a copy. */
static uint16_t staging[1024*512];
static int s_set_mask, s_check_mask;
static void cpu_upload_ex(int x, int y, int w, int h, const uint16_t *src, int catch_up) {
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int vx = (x + c) & 1023, vy = (y + r) & 511;
            uint16_t px = src[r * w + c];
            if (s_check_mask && (gr_vram_read(vx, vy) & 0x8000)) continue;
            if (s_set_mask) px |= 0x8000;
            vram[vy * 1024 + vx] = px;
        }
    /* Worst case for a render thread that staged uploads from this array:
     * it replays an earlier upload of the same rect right here, between
     * gpu.c's pixel writes and its commit (which re-reads the array). */
    if (catch_up && gl_renderer_render_thread_active()) rt_drain();
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            staging[r * w + c] = vram[((y + r) & 511) * 1024 + ((x + c) & 1023)];
    gr_vram_transfer_in(x, y, w, h, staging);
}
static void cpu_upload(int x, int y, int w, int h, const uint16_t *src) {
    cpu_upload_ex(x, y, w, h, src, 0);
}
static void mask_bits(int set, int chk) {
    s_set_mask = set; s_check_mask = chk;
    gr_set_mask_bits(set, chk);
}

#define FRAME_W 320
#define FRAME_H 240

static void textures(void) {
    static uint16_t page[64*64], clut[16];
    for (int i = 0; i < 64*64; i++) page[i] = (uint16_t)((i * 0x1357u) ^ (i >> 3));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0421u * (uint16_t)i) | ((i & 1) << 15) : 0);
    cpu_upload(512, 0, 64, 64, page);
    cpu_upload(512, 256, 16, 1, clut);
}

static void frame(int f, int buf_x) {
    gr_set_draw_area(buf_x, 0, buf_x + FRAME_W - 1, FRAME_H - 1);
    gr_set_draw_offset(buf_x, 0);
    gr_wide_set_target(buf_x);
    gr_fill_rect(buf_x, 0, FRAME_W, FRAME_H, (uint16_t)(0x0842 * (f & 7)));
    gr_wide_clear(buf_x, 0, FRAME_H, (uint16_t)(0x0842 * (f & 7)));
    gr_set_color_modulation(128, 128, 128, 0);
    mask_bits(0, 0);
    /* Many primitives with tags that change per primitive. */
    for (int i = 0; i < 600; i++) {
        g_prim_id = f * 1000 + i;
        if (i == 300) g_bg_full = (f & 1);          /* stream latch flips mid-frame */
        int x = buf_x + (int)(rnd() % 420) - 50, y = (int)(rnd() % 240);
        int w = 4 + (int)(rnd() % 40), h = 4 + (int)(rnd() % 30);
        uint16_t c = (uint16_t)(rnd() & 0x7fff);
        gr_set_semi_transparency((int)(rnd() % 4 == 0), (int)(rnd() % 4));
        switch (i % 7) {
        case 0: gr_draw_flat_rect(x, y, w, h, c); break;
        case 1: gr_draw_gouraud_triangle(x, y, c, x + w, y + 3, (uint16_t)(c ^ 0x1f), x + 2, y + h, (uint16_t)(c ^ 0x3e0)); break;
        case 2: gr_draw_textured_rect(x, y, w, h, (int)(rnd() % 32), (int)(rnd() % 32), 512, 256, 0x0008); break;
        case 3: gr_draw_shaded_textured_triangle(x, y, 0, 0, 0x808080, x + w, y, 63, 4, 0x6090b0,
                                                 x, y + h, 8, 63, 0xb09060, 512, 256, 0x0008, (int)(i & 1)); break;
        case 4: gr_draw_line(x, y, x + w * 2, y + h, c); break;
        case 5: gr_draw_shaded_line(x, y, c, x + 3, y + h, (uint16_t)~c); break;
        default: gr_draw_flat_triangle(x, y, x + w, y + h, x - 5, y + h / 2, c); break;
        }
        g_prim_id = -1;   /* the next GP0 command is in flight: no tags */
    }
    gr_set_semi_transparency(0, 0);
    /* Mask set, then mask check over it. */
    mask_bits(1, 0);
    gr_draw_flat_rect(buf_x + 40, 40, 30, 30, 0x4210);
    mask_bits(0, 1);
    gr_draw_flat_rect(buf_x + 30, 30, 60, 60, 0x7fff);
    /* A mask-checked CPU upload over the masked rect: the per-pixel test
     * reads VRAM back (sync point), in some frames only. */
    if (f % 4 == 1) {
        static uint16_t patch[24 * 24];
        for (int i = 0; i < 24 * 24; i++) patch[i] = (uint16_t)(0x1234 + i * 7 + f);
        cpu_upload(buf_x + 35, 35, 24, 24, patch);
    }
    mask_bits(0, 0);
    /* The same rect uploaded twice with different data back to back. */
    {
        static uint16_t a[32 * 16], b[32 * 16];
        for (int i = 0; i < 32 * 16; i++) { a[i] = (uint16_t)(0x0300 + i + f); b[i] = (uint16_t)(0x6000 ^ (i * 3 + f)); }
        cpu_upload(600, 100, 32, 16, a);
        gr_draw_textured_rect(buf_x + 200, 180, 32, 16, 88, 100, 512, 256, 0x0009); /* samples a */
        cpu_upload_ex(600, 100, 32, 16, b, 1);
        gr_draw_textured_rect(buf_x + 240, 180, 32, 16, 88, 100, 512, 256, 0x0009); /* samples b */
        /* The guest keeps writing its array (as gpu.c does for the next
         * GP0(A0) before that upload completes); nothing may read this. */
        for (int i = 0; i < 32 * 16; i++) vram[(100 + i / 32) * 1024 + 600 + i % 32] = 0x7c1f;
    }
    /* Render to texture, copies. */
    gr_draw_flat_rect(576, 64, 32, 32, (uint16_t)(0x03e0 + f));
    gr_draw_textured_rect(buf_x + 100, 200, 32, 32, 0, 64, 512, 256, 0x0009);
    gr_copy_rect(buf_x + 10, 10, buf_x + 14, 13, 80, 50);
    /* Identity copies preserve colour/mask unless set-mask adds bit15. */
    mask_bits(f % 4 == 2, 0);
    gr_copy_rect(buf_x + 14, 13, buf_x + 14, 13, 80, 50);
    mask_bits(0, 0);
    if (quality_scene && f % 3 == 1) {
        /* Consume the copied bit15 mask before any later framebuffer clear. */
        mask_bits(0, 1);
        gr_draw_flat_rect(buf_x + 14, 13, 80, 50, (uint16_t)(0x7c00 + f));
        mask_bits(0, 0);
    }
    /* A half-VRAM upload every frame pushes the ring past its end. */
    {
        static uint16_t big[1024 * 256];
        for (int i = 0; i < 1024 * 256; i++) big[i] = (uint16_t)(rnd() & 0x7fff);
        cpu_upload(0, 256, 1024, 256, big);
    }
    /* Readbacks in some frames (GPUREAD-style and VRAM->CPU). */
    if (f % 5 == 3) {
        for (int i = 0; i < 64; i++) {
            uint16_t v = gr_vram_read(buf_x + (int)(rnd() % FRAME_W), (int)(rnd() % FRAME_H));
            rb_digest = fnv(&v, 2, rb_digest);
        }
        static uint16_t out[64 * 32];
        gr_vram_transfer_out(buf_x + 50, 50, 64, 32, out);
        rb_digest = fnv(out, sizeof out, rb_digest);
    }
    if (wide_present) {
        /* Reach both reveal margins after the last upload/readback, leaving
         * real queued work for the final present flush. Alternate the full
         * background gate so the pruning fallback is exercised too. */
        g_bg_full = f % 3 == 0;
        gr_wide_set_view(0, 0, 0, 0);
        g_prim_id = 1;
        gr_set_semi_transparency(f % 2, f % 4);
        if (quality_scene) {
            gr_set_precise_triangle(1, (buf_x-40)*65536, 20*65536,
                (buf_x+360)*65536, 20*65536, (buf_x+160)*65536, 220*65536);
            gr_set_depth_triangle(1, 2000.0f, 2200.0f, 2400.0f);
        }
        gr_draw_textured_triangle(buf_x-40, 20, 0, 0, buf_x+360, 20, 63, 0,
                                  buf_x+160, 220, 32, 63, 512, 256, 0x0008);
        if (quality_scene) {
            /* Minification, transparent/STP texels, and a later occluded draw
             * make depth coverage observable in the final colour image. */
            gr_set_semi_transparency(0, 0);
            gr_set_precise_triangle(1, (buf_x+80)*65536, 60*65536,
                (buf_x+100)*65536, 60*65536, (buf_x+90)*65536, 80*65536);
            gr_set_depth_triangle(1, 3000.0f, 3000.0f, 3000.0f);
            gr_draw_textured_triangle(buf_x+80, 60, 0, 0, buf_x+100, 60, 255, 0,
                                      buf_x+90, 80, 127, 255, 512, 256, 0x0008);
            gr_set_precise_triangle(0, 0, 0, 0, 0, 0, 0);
            gr_set_depth_triangle(0, 0, 0, 0);
        }
        check(gl_renderer_present_wide_fbo(buf_x, 0, FRAME_H, 0), "wide present");
    } else
        gl_renderer_present_vram(buf_x, 0, FRAME_W, FRAME_H, 0, 0);
    gl_renderer_render_thread_frame_boundary();
}

int main(int argc, char **argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    int threaded = argc > 2 && argv[2][0] == '1';
    int frames = argc > 3 ? atoi(argv[3]) : 80;
    int dynres = argc > 4 && !strcmp(argv[4], "dynres");
    wide_present = argc > 4 && !strcmp(argv[4], "interp-wide");
    int interp = wide_present || (argc > 4 && !strcmp(argv[4], "interp"));
    quality_scene = argc > 5 && !strcmp(argv[5], "quality");
    /* No video device, window or GL context (headless, Windows over SSH):
     * exit 77, which the harness reports as a CTest skip. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SKIP no video (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Render thread hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window (%s)\n", SDL_GetError()); return 77; }
    {   /* Probe with the renderer's attributes: only a probe tells "no
         * context on this host" from "the renderer's init broke". */
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GLContext probe = SDL_GL_CreateContext(win);
        if (!probe) {
            fprintf(stderr, "SKIP no GL 3.3 core context (%s)\n", SDL_GetError());
            return 77;
        }
        SDL_GL_MakeCurrent(win, NULL);
        SDL_GL_DeleteContext(probe);
    }
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(scale);
    if (dynres) gl_renderer_set_dynamic_resolution(1);
    gl_renderer_set_swap_interval(0);
    if (getenv("PSX_GL_TEST_PRESENT_THREAD")) gl_renderer_set_present_thread(1, 3);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "FAIL context\n"); return 2; }
    check(gr_backend() == GR_BACKEND_OPENGL, "OpenGL backend");
    textures();
    if (wide_present && s_hiw) {
        check(hiw_ensure(0, FRAME_W) != NULL, "first hires buffer");
        check(hiw_ensure(512, 512+FRAME_W) != NULL, "second hires buffer");
    }
    gl_renderer_swap_timing_start(fixture_clock_us);
    gr_wide_configure(426, 53);
    if (quality_scene) {
        gl_renderer_set_bloom(1.0f);
        gl_renderer_set_pgxp_depth(1);
        gl_renderer_set_texture_lod(1, 16);
    }
    if (interp) {
        gl_renderer_set_interpolation_source(1);
        gl_renderer_set_interpolation(1, 60.0, 60.0, 60.0, 2);
    }
    if (threaded) {
        check(gl_renderer_render_thread_start(2) == 1, "render thread started");
        check(gl_renderer_render_thread_active(), "render thread active");
    }
    test_live_resolution(scale);
    GlDynresStats live_baseline;
    gl_renderer_dynres_stats(&live_baseline);
    if (dynres) {
        check(gl_renderer_dynamic_resolution_ceiling() == scale, "dynres ceiling");
        if (threaded) gl_renderer_render_thread_measure(1);
    }
    static const int levels[] = { 2, 1, 3, 1, 4, 2 };
    int nsteps = 0, level = scale;
    const uint64_t drawable_queries_before = s_drawable_queries;
    for (int f = 0; f < frames; f++) {
        frame(f, (f & 1) ? 512 : 0);
        if (dynres && f % 7 == 6) {   /* includes frames held at a sync point */
            int l = levels[(f / 7) % 6];
            if (l > scale) l = scale;
            if (l != level) nsteps++;
            level = l;
            check(gl_renderer_step_internal_scale_now(l) == 1, "dynres step");
            GlDynresStats ds;
            gl_renderer_dynres_stats(&ds);
            check(ds.level == l, "dynres level as asked");
            check(gr_scale() == l, "gr_scale follows the level");
        }
    }
    if (dynres) {
        if (level != scale) nsteps++;
        check(gl_renderer_step_internal_scale_now(scale) == 1, "dynres back to the ceiling");
        gl_renderer_render_thread_sync("test");
        GlDynresStats ds;
        gl_renderer_dynres_stats(&ds);
        const uint64_t scripted_steps = ds.steps - live_baseline.steps;
        printf("dyn_steps=%llu asked=%d\n", (unsigned long long)scripted_steps, nsteps);
        check(scripted_steps == (uint64_t)nsteps && ds.level == scale, "every step applied");
        if (threaded) {
            GlRthCosts co;
            gl_renderer_render_thread_costs(&co);
            printf("rth_cost_frames=%llu gpu_frames=%llu dropped=%llu mean_ms=%.3f\n",
                   (unsigned long long)co.frames, (unsigned long long)co.gpu_frames,
                   (unsigned long long)co.dropped,
                   co.frames ? (double)co.cost_ns / (double)co.frames * 1e-6 : 0.0);
            check(co.frames > 0, "render-thread frame costs measured");
        }
    }

    /* Guest-visible VRAM, the frame at internal resolution, the wide surface. */
    static uint16_t native[1024 * 512];
    gr_vram_transfer_out(0, 0, 1024, 512, native);
    uint64_t digest = fnv(native, sizeof native, 0xcbf29ce484222325ull);
    check(memcmp(native, vram, sizeof native) == 0, "readback landed in the guest-visible array");
    int fw = FRAME_W * scale, fh = FRAME_H * scale, ow = 0, oh = 0;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    int n = img ? gl_renderer_read_display_hires(512, 0, FRAME_W, FRAME_H, img, fw * fh, &ow, &oh) : 0;
    check(n == fw * fh, "hires readback");
    uint64_t hires = n ? fnv(img, (size_t)fw * fh * 4, 0xcbf29ce484222325ull) : 0;
    free(img);
    uint64_t wide = 0;
    {
        int ww = 426 * scale, wh = 512 * scale, gw = 0, gh = 0;
        uint32_t *wb = (uint32_t *)malloc((size_t)ww * wh * 4);
        int got = wb ? gr_wide_dump_full(wb, ww * wh, &gw, &gh, 512) : 0;
        check(got == ww * wh, "wide dump");
        if (got) wide = fnv(wb, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        free(wb);
    }
    if (interp) {
        check(s_interp_captures > 0 && s_interp_swaps > 0, "temporal frames presented");
    }
    /* Colour hashes alone do not verify the bloom-source stencil. Read its
     * bit on every canonical/window/wide target while the context is held. */
    uint64_t stencil = 0xcbf29ce484222325ull;
    if (quality_scene) {
        gl_renderer_render_thread_sync("test_stencil");
        GLuint fbos[1 + HIW_MAX_TILES + 8];
        int widths[1 + HIW_MAX_TILES + 8], heights[1 + HIW_MAX_TILES + 8], nf = 0;
        fbos[nf] = s_hr_fbo; widths[nf] = VRAM_W * s_hr_scale;
        heights[nf++] = VRAM_H * s_hr_scale;
        for (int t = 0; t < s_hiw_n; ++t) {
            fbos[nf] = s_hiw_t[t].fbo;
            widths[nf] = (s_hiw_t[t].x1 - s_hiw_t[t].x0) * scale;
            heights[nf++] = VRAM_H * scale;
        }
        /* The displayed wide surface is sufficient for the centre splice
         * and clipped mirror; canonical targets cover both buffer origins. */
        if (g_wide_cur) {
            fbos[nf] = g_wide_cur; widths[nf] = g_wide_w * scale;
            heights[nf++] = VRAM_H * scale;
        }
        for (int t = 0; t < nf; ++t) {
            size_t bytes = (size_t)widths[t] * heights[t];
            unsigned char *bits = malloc(bytes);
            check(bits != NULL, "stencil readback allocation");
            if (!bits) continue;
            /* Bit0 can remain deferred under disabled mask checking. Compare
             * alpha-derived semantic masks, preserving bloom bit1, rather
             * than comparing the unused deferred storage representation. */
            rebuild_target_stencil(fbos[t], widths[t], heights[t]);
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbos[t]);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, widths[t], heights[t], GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, bits);
            for (size_t p = 0; p < bytes; ++p) bits[p] &= 3;
            stencil = fnv(bits, bytes, stencil);
            free(bits);
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    }
    if (wide_present && wide_margin_scissor()) {
        check(s_hq_center_pruned > 0, "queued centre shading was elided");
        printf("pruned_cmds=%llu\n", (unsigned long long)s_hq_center_pruned);
    }
    if (wide_present && hiw_upload_fast()) {
        check(s_hq_saved_uploads > s_hq_uploads, "queue uploaded once for multiple replay draws");
        printf("queue_uploads=%llu replay_draws=%llu\n", (unsigned long long)s_hq_uploads,
               (unsigned long long)s_hq_saved_uploads);
    }
    if (quality_scene && coverage_fast_enabled()) {

        check(s_bcls_fused_draws > 0, "bloom stencil fused with colour draws");
        printf("fused_draws=%llu\n", (unsigned long long)s_bcls_fused_draws);
        check(s_coverage_depth_draws > 0, "coverage-only depth passes ran");
        printf("coverage_draws=%llu\n", (unsigned long long)s_coverage_depth_draws);
    }
    if (!getenv("PSX_GL_TEXEL_CACHE") || getenv("PSX_GL_TEXEL_CACHE")[0] != '0') {
        check(s_tc_ready == 1 && s_tc_hits > 0 && s_tc_misses > 0, "decoded indexed texels reused");
        printf("texel_cache_hits=%llu misses=%llu evictions=%llu\n",
               (unsigned long long)s_tc_hits, (unsigned long long)s_tc_misses,
               (unsigned long long)s_tc_evictions);
    }
    if (threaded) {
        RtStats st;
        rt_get_stats(&st);
        printf("rt_records=%llu rt_acquires=%llu rt_frames=%llu rt_high_water=%llu presents=%llu stale=%llu\n",
               (unsigned long long)st.records, (unsigned long long)st.acquires,
               (unsigned long long)st.frames_produced, (unsigned long long)st.ring_high_water,
               (unsigned long long)s_rth_presents, (unsigned long long)s_rth_presents_stale);
        check(st.records > (uint64_t)frames * 600, "draws were recorded");
        /* Sync points: the mask-checked uploads, the readback frames and the
         * final readbacks; never more than one per frame plus the tail. */
        check(st.acquires >= 2 && st.acquires <= (uint64_t)frames + 4, "acquires bounded");
        check(st.bytes > ((uint64_t)32 << 20), "ring wrapped (more than its size recorded)");
    }
    {
        uint64_t times[256], tail[2];
        int count = gl_renderer_swap_timestamps(times, 256);
        check(count > 2, "swap completion timestamps captured");
        for (int i=1; i<count; ++i)
            check(times[i] >= times[i-1], "swap clock is monotonic");
        check(gl_renderer_swap_timestamps(tail, 2) == 2 &&
              tail[0] == times[count-2] && tail[1] == times[count-1],
              "bounded swap query returns newest entries");
    }
    if (drawable_events_enabled())
        check(s_drawable_queries == drawable_queries_before, "no per-frame drawable queries");
    /* Pixel-size events publish resizes without querying the window on a
     * worker or taking its property lock on every frame boundary. */
    for (int k = 0; k < 2; ++k) {
        SDL_SetWindowSize(win, 400 + k * 80, 300 + k * 60);
        SDL_PumpEvents();
        gl_renderer_render_thread_frame_boundary();
        int expected[2] = {0}, seen[2] = {0};
        SDL_GL_GetDrawableSize(win, &expected[0], &expected[1]);
        SDL_Thread *worker = SDL_CreateThread(drawable_worker, "drawable-test", seen);
        check(worker != NULL, "drawable reader thread created");
        if (worker) SDL_WaitThread(worker, NULL);
        check(seen[0] == expected[0] && seen[1] == expected[1], "worker sees resized drawable");
    }
    gl_renderer_render_thread_sync("test_done");
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("rb=%016llx\n", (unsigned long long)rb_digest);
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("hires=%016llx\n", (unsigned long long)hires);
    printf("wide=%016llx\n", (unsigned long long)wide);
    printf("stencil=%016llx\n", (unsigned long long)stencil);
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown();
    check(!gl_renderer_render_thread_active(), "stopped by shutdown");
    SDL_DestroyWindow(win);
    SDL_Quit();
    return failures ? 1 : 0;
}
