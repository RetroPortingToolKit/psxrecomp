/* Bloom ([video] bloom) on a real, hidden OpenGL context.
 *
 *  - off: the composed frame is the stock one (byte-identical before and
 *    after a round trip through bloom; the pass never runs);
 *  - on: a bright 2D primitive (sprite, HUD) and a 2D one drawn over a lit 3D
 *    one gain no glow at all (byte-identical to off), while an additive
 *    primitive and a PGXP-proven 3D one glow around their edges;
 *  - guest VRAM / readbacks are the same either way, with and without the
 *    render thread.
 * With --dump PATH the off frame of every scene is written raw (RGB,
 * bottom-up) so a run against another tree can be compared byte for byte
 * (BLOOM_FIXTURE_NO_BLOOM builds it without the bloom API). */
#define GL_PRESENT_TEST_HOOK(gen) bloom_capture_hook(gen)
static void bloom_capture_hook(int gen);
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;(void)w;(void)h;(void)p;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int x,int y,int w,int h){(void)sx;(void)sy;(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
#ifndef PSX_NO_DEBUG_TOOLS
int present_image_ring_accepting(void){return 0;}
int present_image_ring_thumb_w(int w,int h){(void)w;(void)h;return 0;}
void present_image_ring_push(uint32_t f,const uint8_t* p,int w,int pitch,int flip){(void)f;(void)p;(void)w;(void)pitch;(void)flip;}
void present_image_ring_push_argb(uint32_t f,const uint32_t* p,int w,int h,int pitch){(void)f;(void)p;(void)w;(void)h;(void)pitch;}
#endif
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }

static uint8_t *s_cap; static int s_cap_w, s_cap_h, s_cap_n;
static void bloom_capture_hook(int gen) {
    (void)gen;
    SDL_GL_GetDrawableSize(s_win, &s_cap_w, &s_cap_h);
    free(s_cap);
    s_cap = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s_cap_w, s_cap_h, GL_RGB, GL_UNSIGNED_BYTE, s_cap);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    s_cap_n++;
}

enum { SC_2D, SC_ADD, SC_3D, SC_HUD_OVER_3D, SC_N };
static const char *k_scene[SC_N] = { "2d", "additive", "3d", "hud-over-3d" };

/* A PGXP-proven triangle: precise positions plus GTE SZ, as gpu.c sends. */
static void tri3d(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    glb_set_precise_triangle(1, x0 * 65536, y0 * 65536, x1 * 65536, y1 * 65536,
                             x2 * 65536, y2 * 65536);
    glb_set_depth_triangle(1, 2000.0f, 2000.0f, 2000.0f);
    glb_draw_flat_triangle(x0, y0, x1, y1, x2, y2, c);
}

static uint8_t *frame(int scene, float bloom, uint64_t *vram_hash) {
#ifndef BLOOM_FIXTURE_NO_BLOOM
    gl_renderer_set_bloom(bloom);
#else
    (void)bloom;
#endif
    glb_set_draw_area(0, 0, FRAME_W - 1, FRAME_H - 1); glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0); glb_set_semi_transparency(0, 0);
    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0x0000);
    switch (scene) {
    case SC_2D:     /* a white HUD panel */
        glb_draw_flat_rect(120, 90, 80, 60, 0x7fff);
        break;
    case SC_ADD:    /* an additive light halo over black */
        glb_set_semi_transparency(1, 1);
        glb_draw_flat_triangle(120, 150, 200, 150, 160, 80, 0x7fff);
        glb_set_semi_transparency(0, 0);
        break;
    case SC_3D:     /* a lit 3D polygon */
        tri3d(120, 150, 200, 150, 160, 80, 0x7fff);
        break;
    case SC_HUD_OVER_3D:   /* the same 3D polygon under an opaque panel */
        tri3d(120, 150, 200, 150, 160, 80, 0x7fff);
        glb_draw_flat_rect(100, 70, 120, 90, 0x7fff);
        break;
    }
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, FRAME_W, FRAME_H, peek), "vram readback");
    *vram_hash = fnv(peek, FRAME_W * FRAME_H * 2, 0xcbf29ce484222325ull);
    const int n0 = s_cap_n;
    if (s_rth_on) gl_renderer_render_thread_frame_boundary();
    gl_renderer_present_vram(0, 0, FRAME_W, FRAME_H, 0, 0);
    if (s_rth_on) {
        gl_renderer_render_thread_frame_boundary();
        gl_renderer_render_thread_sync("bloom-capture");
    }
    check(s_cap_n == n0 + 1 && s_cap, "present captured");
    uint8_t *c = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    memcpy(c, s_cap, (size_t)s_cap_w * s_cap_h * 3);
    return c;
}

/* Brightness added outside the off frame's lit pixels (the glow). */
static long glow(const uint8_t *off, const uint8_t *on) {
    long sum = 0;
    for (int i = 0; i < s_cap_w * s_cap_h; i++) {
        if (off[i * 3] | off[i * 3 + 1] | off[i * 3 + 2]) continue;
        sum += on[i * 3] + on[i * 3 + 1] + on[i * 3 + 2];
    }
    return sum;
}

int main(int argc, char **argv) {
    const char *dump = argc > 2 && !strcmp(argv[1], "--dump") ? argv[2] : NULL;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Bloom hidden test", 0, 0, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    glb_init(vram); glb_set_scale(2); gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    uint8_t *off[SC_N]; uint64_t h_off[SC_N];
    for (int s = 0; s < SC_N; s++) off[s] = frame(s, 0.0f, &h_off[s]);
    const size_t bytes = (size_t)s_cap_w * s_cap_h * 3;
    if (dump) {
        FILE *f = fopen(dump, "wb");
        for (int s = 0; s < SC_N; s++)
            check(f && fwrite(off[s], 1, bytes, f) == bytes, "dump");
        if (f) fclose(f);
    }
#ifndef BLOOM_FIXTURE_NO_BLOOM
    GlBloomInfo bi;
    gl_renderer_bloom_debug(-1.0f, -1.0f, -1, -1, &bi);
    check(bi.passes == 0, "off never runs the bloom pass");
    for (int pass = 0; pass < 2; pass++) {   /* 0: this thread, 1: render thread */
        if (pass == 1) {
            check(gl_renderer_render_thread_start(2) == 1, "render thread started");
            gl_renderer_render_thread_frame_boundary();
        }
        long g[SC_N];
        for (int s = 0; s < SC_N; s++) {
            uint64_t h = 0;
            uint8_t *on = frame(s, 1.5f, &h);
            g[s] = glow(off[s], on);
            check(h == h_off[s], "guest VRAM identical with bloom on");
            if (s == SC_2D || s == SC_HUD_OVER_3D)
                check(memcmp(off[s], on, bytes) == 0, "2D / HUD pixels never glow");
            free(on);
            uint8_t *back = frame(s, 0.0f, &h);
            check(memcmp(off[s], back, bytes) == 0 && h == h_off[s], "off after bloom is byte-identical");
            free(back);
        }
        printf("%s drawable=%dx%d glow 2d=%ld additive=%ld 3d=%ld hud-over-3d=%ld\n",
               pass ? "rt" : "st", s_cap_w, s_cap_h, g[SC_2D], g[SC_ADD], g[SC_3D], g[SC_HUD_OVER_3D]);
        check(g[SC_2D] == 0 && g[SC_HUD_OVER_3D] == 0, "no glow from 2D");
        check(g[SC_ADD] > 20000, "additive primitive glows");
        check(g[SC_3D] > 20000, "3D primitive glows");
    }
    gl_renderer_render_thread_stop();
    (void)k_scene;
#endif
    for (int s = 0; s < SC_N; s++) free(off[s]);
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
