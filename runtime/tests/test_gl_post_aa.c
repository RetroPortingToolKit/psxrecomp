/* Post-process anti-aliasing ([video] antialiasing_mode) on a real, hidden
 * OpenGL context.
 *
 *  - off: the composed frame is untouched (byte-identical before and after a
 *    round trip through the AA modes; the pass never runs);
 *  - fxaa / fxaa_hq: a hard polygon edge gains intermediate shades (less
 *    stair-stepping) while flat regions away from the edge stay identical;
 *  - guest VRAM / readbacks are the same whatever the mode.
 * With --dump PATH the off frame is written raw (RGB, bottom-up) so a run
 * against another tree can be compared byte for byte. */
#define GL_PRESENT_TEST_HOOK(gen) aa_capture_hook(gen)
static void aa_capture_hook(int gen);
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
/* RT replay links these residency callbacks even with no HD session. */
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;(void)w;(void)h;(void)p;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int x,int y,int w,int h){(void)sx;(void)sy;(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
#ifndef PSX_NO_DEBUG_TOOLS
/* This fixture tests AA timing, not the debug image ring. */
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
void psx_host_sleep_micros(unsigned us) { SDL_Delay((us + 999u) / 1000u); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }

static uint8_t *s_cap; static int s_cap_w, s_cap_h, s_cap_n;
static void aa_capture_hook(int gen) {
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

/* A white polygon with a shallow top edge (long stair steps) on black. */
static uint8_t *frame(int mode, uint64_t *vram_hash) {
#ifndef AA_FIXTURE_NO_POST_AA
    gl_renderer_set_post_aa(mode);
#else
    (void)mode;
#endif
    glb_set_draw_area(0, 0, FRAME_W - 1, FRAME_H - 1); glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0); glb_set_semi_transparency(0, 0);
    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0x0000);
    glb_draw_flat_triangle(20, 150, 300, 110, 300, 200, 0x7fff);
    glb_draw_flat_triangle(20, 150, 300, 200, 20, 200, 0x7fff);
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, FRAME_W, FRAME_H, peek), "vram readback");
    *vram_hash = fnv(peek, FRAME_W * FRAME_H * 2, 0xcbf29ce484222325ull);
    const int n0 = s_cap_n;
    if (s_rth_on) gl_renderer_render_thread_frame_boundary();
    gl_renderer_present_vram(0, 0, FRAME_W, FRAME_H, 0, 0);
    if (s_rth_on) {
        gl_renderer_render_thread_frame_boundary();
        gl_renderer_render_thread_sync("aa-capture");
    }
    check(s_cap_n == n0 + 1 && s_cap, "present captured");
    uint8_t *c = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    memcpy(c, s_cap, (size_t)s_cap_w * s_cap_h * 3);
    return c;
}

static int mid_tones(const uint8_t *c) {
    int n = 0;
    for (int i = 0; i < s_cap_w * s_cap_h; i++) { int g = c[i * 3 + 1]; n += g > 24 && g < 231; }
    return n;
}

/* Pixels whose 5x5 neighbourhood in ref is flat must be identical in c. */
static int flat_changed(const uint8_t *ref, const uint8_t *c) {
    int bad = 0;
    for (int y = 2; y < s_cap_h - 2; y++)
        for (int x = 2; x < s_cap_w - 2; x++) {
            const uint8_t *r = ref + ((size_t)y * s_cap_w + x) * 3;
            int flat = 1;
            for (int dy = -2; dy <= 2 && flat; dy++)
                for (int dx = -2; dx <= 2; dx++)
                    if (memcmp(ref + ((size_t)(y + dy) * s_cap_w + x + dx) * 3, r, 3)) { flat = 0; break; }
            if (flat && memcmp(c + ((size_t)y * s_cap_w + x) * 3, r, 3)) bad++;
        }
    return bad;
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
    SDL_Window *win = SDL_CreateWindow("Post AA hidden test", 0, 0, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    glb_init(vram); glb_set_scale(1); gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    uint64_t h_off = 0, h_fx = 0, h_hq = 0, h_off2 = 0;
    uint8_t *off = frame(GL_POST_AA_OFF, &h_off);
    if (dump) {
        FILE *f = fopen(dump, "wb");
        check(f && fwrite(off, 1, (size_t)s_cap_w * s_cap_h * 3, f) == (size_t)s_cap_w * s_cap_h * 3, "dump");
        if (f) fclose(f);
    }
#ifndef AA_FIXTURE_NO_POST_AA
    check(gl_renderer_post_aa_passes() == 0, "off never runs the AA pass");
    uint8_t *fx = frame(GL_POST_AA_FXAA, &h_fx);
    uint8_t *hq = frame(GL_POST_AA_FXAA_HQ, &h_hq);
    uint8_t *off2 = frame(GL_POST_AA_OFF, &h_off2);
    const size_t bytes = (size_t)s_cap_w * s_cap_h * 3;
    const int m_off = mid_tones(off), m_fx = mid_tones(fx), m_hq = mid_tones(hq);
    printf("drawable=%dx%d mid_tones off=%d fxaa=%d fxaa_hq=%d flat_changed fxaa=%d hq=%d passes=%llu\n",
           s_cap_w, s_cap_h, m_off, m_fx, m_hq, flat_changed(off, fx), flat_changed(off, hq),
           (unsigned long long)gl_renderer_post_aa_passes());
    check(gl_renderer_post_aa_passes() == 2, "one pass per AA present");
    check(m_fx > m_off + 40, "fxaa softens the polygon edge");
    check(m_hq > m_off + 40, "fxaa_hq softens the polygon edge");
    check(memcmp(off, fx, bytes) != 0, "fxaa changes the frame");
    check(flat_changed(off, fx) == 0 && flat_changed(off, hq) == 0, "flat regions untouched");
    check(memcmp(off, off2, bytes) == 0, "off after AA is byte-identical");
    check(h_off == h_fx && h_off == h_hq && h_off == h_off2, "guest VRAM identical in every mode");
    free(fx); free(hq); free(off2);
    /* The same live transitions with actual RT-owned presents. The setter
     * must hand ownership to the caller before changing the mode. */
    check(gl_renderer_render_thread_start(2) == 1, "AA render thread started");
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(), "AA render thread initially owns the context");
    gl_renderer_set_post_aa(GL_POST_AA_OFF);
    check(rt_held(), "live AA setter acquires context ownership");
#ifndef PSX_NO_DEBUG_TOOLS
    s_paa_time = 1;
#endif
    uint64_t h_rt = 0;
    uint8_t *rt_off = frame(GL_POST_AA_OFF, &h_rt);
    check(h_rt == h_off && memcmp(off, rt_off, bytes) == 0, "RT AA off preserves pixels and native VRAM");
    free(rt_off);
    uint8_t *rt_fx = frame(GL_POST_AA_FXAA, &h_rt);
    check(h_rt == h_off && mid_tones(rt_fx) > m_off + 40, "RT FXAA softens edges without changing VRAM");
    free(rt_fx);
    uint8_t *rt_hq = frame(GL_POST_AA_FXAA_HQ, &h_rt);
    check(h_rt == h_off && mid_tones(rt_hq) > m_off + 40, "RT HQ softens edges without changing VRAM");
    free(rt_hq);
    uint8_t *rt_off2 = frame(GL_POST_AA_OFF, &h_rt);
    check(h_rt == h_off && memcmp(off, rt_off2, bytes) == 0, "RT AA round trip restores exact off image");
    free(rt_off2);
    gl_renderer_render_thread_frame_boundary();
    check(gl_renderer_post_aa() == GL_POST_AA_OFF && rt_held(), "AA mode getter uses ownership handoff");
    gl_renderer_render_thread_frame_boundary();
    check(gl_renderer_post_aa_passes() == 4 && rt_held(), "AA pass getter drains in-flight work safely");
#ifndef PSX_NO_DEBUG_TOOLS
    gl_renderer_render_thread_frame_boundary();
    check(gl_renderer_post_aa_gpu_us() > 0.0 && rt_held(), "AA timing getter safely reads and resets RT measurements");
    check(gl_renderer_post_aa_gpu_us() == 0.0, "AA timing mean resets after read");
#endif
    gl_renderer_render_thread_stop();
#endif
    free(off);
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
