/* [video] dithering on a real, hidden OpenGL context, driven through the
 * renderer facade (gr_*) the way gpu.c drives it.
 *
 *  - off (the default): the GP0(E1h) dither bit changes nothing, so the
 *    native VRAM and the frame at internal resolution are byte-identical
 *    with the bit set or clear;
 *  - on / scaled with the bit clear: identical to off;
 *  - on / scaled with the bit set: a gouraud gradient and a modulated
 *    texture gain the PS1 4x4 pattern (more 5-bit steps along a row, same
 *    mean brightness), while flat fills, flat polygons and raw textures stay
 *    exactly as they were;
 *  - "scaled" puts the pattern on the native grid: the native VRAM at 4x
 *    (stepped by dynamic resolution) equals the native VRAM at 1x;
 *  - the render thread (records replayed later), PGXP depth and native-wide
 *    margins give the same pixels as the direct path.
 * Original source-owned scene; no retail payload. */
#define main rt_fixture_main
#include "test_gl_render_thread.c"
#undef main

#define DW 320
#define DH 240
static uint16_t nat[1024 * 512];
static uint32_t hi[DW * 4 * DH * 4];
static int k_test_scale = 4;

static void scene(int bit, int depth) {
    gr_set_draw_area(0, 0, DW - 1, DH - 1);
    gr_set_draw_offset(0, 0);
    gr_wide_set_target(0);
    mask_bits(0, 0);
    gr_set_semi_transparency(0, 0);
    gr_set_dither(0);
    gr_fill_rect(0, 0, DW, DH, 0x0000);
    gr_wide_clear(0, 0, DH, 0x0000);
    gr_set_dither(bit);
    /* A horizontal gouraud gradient, black to white (rows 0..63), reaching
     * into the left native-wide margin. */
    if (depth) gr_set_depth_triangle(1, 900.0f, 900.0f, 900.0f);
    gr_draw_gouraud_triangle(-40, 0, 0x0000, 256, 0, 0x7fff, -40, 64, 0x0000);
    if (depth) gr_set_depth_triangle(1, 900.0f, 900.0f, 900.0f);
    gr_draw_gouraud_triangle(256, 0, 0x7fff, 256, 64, 0x7fff, -40, 64, 0x0000);
    /* Flat polygon and flat rect (never dithered). */
    gr_draw_flat_rect(0, 80, 64, 32, 0x4210);
    gr_draw_flat_triangle(80, 80, 140, 80, 80, 112, 0x2d6b);
    /* Modulated and raw textured draws. */
    gr_set_color_modulation(128, 128, 128, 0);
    gr_draw_shaded_textured_triangle(160, 80, 0, 0, 0x404040, 300, 80, 63, 0, 0xa0a0a0,
                                     160, 200, 0, 63, 0x606060, 512, 256, 0x0008, 0);
    gr_set_color_modulation(128, 128, 128, 1);
    gr_draw_textured_rect(0, 130, 64, 64, 0, 0, 512, 256, 0x0008);
    gr_set_color_modulation(128, 128, 128, 0);
    gr_set_dither(0);
    gl_renderer_render_thread_frame_boundary();
}

typedef struct { uint64_t native, hires, wide; } Shot;
static Shot shot(void) {
    Shot s;
    gr_vram_transfer_out(0, 0, 1024, 512, nat);
    s.native = fnv(nat, sizeof nat, 0xcbf29ce484222325ull);
    int ow = 0, oh = 0, sc = gr_scale();
    int n = gl_renderer_read_display_hires(0, 0, DW, DH, hi, DW * sc * DH * sc, &ow, &oh);
    check(n == DW * sc * DH * sc, "hires readback");
    s.hires = fnv(hi, (size_t)n * 4, 0xcbf29ce484222325ull);
    static uint32_t wb[426 * 4 * 512 * 4];
    int gw = 0, gh = 0, got = gr_wide_dump_full(wb, 426 * sc * 512 * sc, &gw, &gh, 0);
    s.wide = got > 0 ? fnv(wb, (size_t)got * 4, 0xcbf29ce484222325ull) : 0;
    return s;
}
static int same(Shot a, Shot b) { return a.native == b.native && a.hires == b.hires && a.wide == b.wide; }
static int region_equal(const uint16_t *a, const uint16_t *b, int x, int y, int w, int h) {
    for (int r = y; r < y + h; r++)
        if (memcmp(a + r * 1024 + x, b + r * 1024 + x, (size_t)w * 2)) return 0;
    return 1;
}
/* Along the gradient's row 30: steps between neighbours, and the mean red. */
static void row_stats(const uint16_t *v, int *steps, double *mean) {
    int s = 0; long sum = 0;
    for (int x = 0; x < 250; x++) {
        int r = v[30 * 1024 + x] & 31, n = v[30 * 1024 + x + 1] & 31;
        s += r != n; sum += r;
    }
    *steps = s; *mean = (double)sum / 250.0;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Dithering hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(k_test_scale);
    gl_renderer_set_dynamic_resolution(1);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    textures();
    gr_wide_configure(426, 53);
    static uint16_t ref[1024 * 512], dith[1024 * 512];

    /* Off: the bit is ignored. */
    check(gl_renderer_dithering() == 0, "off by default");
    scene(0, 0); Shot off0 = shot(); memcpy(ref, nat, sizeof ref);
    scene(1, 0); Shot off1 = shot();
    check(same(off0, off1), "off: dither bit changes nothing");
    for (int mode = 1; mode <= 2; mode++) {
        gl_renderer_set_dithering(mode);
        scene(0, 0); Shot clear = shot();
        check(same(off0, clear), "on/scaled with the bit clear equals off");
        scene(1, 0); Shot on = shot();
        check(on.native != off0.native && on.hires != off0.hires, "bit set dithers");
        check(on.wide != off0.wide, "native-wide margin dithers too");
        memcpy(dith, nat, sizeof dith);
        check(region_equal(ref, dith, 0, 80, 150, 32), "flat polygons untouched");
        check(region_equal(ref, dith, 0, 130, 64, 64), "raw texture untouched");
        check(!region_equal(ref, dith, 160, 80, 140, 120), "modulated texture dithered");
        int s0, s1; double m0, m1;
        row_stats(ref, &s0, &m0); row_stats(dith, &s1, &m1);
        printf("mode=%d steps true=%d dithered=%d mean true=%.2f dithered=%.2f\n", mode, s0, s1, m0, m1);
        /* "on" dithers per internal pixel, so the native pack (each block's
         * top-left sample) sees one pattern column; "scaled" all four. */
        check(mode == 2 ? s1 > s0 * 3 : s1 > s0, "dither pattern adds 5-bit steps");
        check(fabs(m1 - m0) < 1.0, "dither keeps the mean brightness");
        for (int x = 0; x < 250; x++) {
            uint16_t p = dith[30 * 1024 + x];
            check((p & 31) == ((p >> 5) & 31) && (p & 31) == ((p >> 10) & 31), "grey stays grey");
        }
        /* PGXP depth: the same 2D-depth scene, depth-tested. */
        gl_renderer_set_pgxp_depth(1);
        scene(1, 1); Shot dep = shot();
        check(dep.native == on.native, "PGXP depth path dithers the same");
        gl_renderer_set_pgxp_depth(0);
        /* Render thread: dither bit and mode recorded with the draws. */
        check(gl_renderer_render_thread_start(2) == 1, "render thread started");
        scene(1, 0);
        gl_renderer_render_thread_sync("dither");
        Shot rt = shot();
        gl_renderer_render_thread_stop();
        check(same(rt, on), "render thread gives the same pixels");
        /* With accurate_blending the dither output uses the k*8 basis, like
         * every other write: every gradient channel at internal resolution
         * is a multiple of 8 (no k*255/31 values mixed in). */
        gl_renderer_set_accurate_blending(1);
        scene(1, 0); Shot acc = shot();
        int off_basis = 0;
        for (int y = 0; y < 60 * gr_scale(); y++)
            for (int x = 0; x < 250 * gr_scale(); x++) {
                uint32_t p = hi[y * DW * gr_scale() + x];
                off_basis += ((p & 0xff) % 8) != 0;
            }
        int sa; double ma;
        row_stats(nat, &sa, &ma);
        printf("mode=%d accurate: mean=%.2f channels off the k*8 grid=%d\n", mode, ma, off_basis);
        check(fabs(ma - m0) < 1.0, "dither + accurate blending keeps the mean");
        check(off_basis == 0, "dither output follows the accurate (k*8) basis");
        gl_renderer_set_accurate_blending(0);
        if (mode == 2) {
            /* Scaled: the pattern sits on the native grid at any scale. */
            check(gl_renderer_step_internal_scale_now(1) == 1, "step to 1x");
            scene(1, 0); Shot one = shot();
            check(one.native == on.native, "scaled: native VRAM at 4x equals 1x");
            check(gl_renderer_step_internal_scale_now(k_test_scale) == 1, "back to 4x");
        }
    }
    gl_renderer_set_dithering(0);
    scene(1, 0); Shot back = shot();
    check(same(back, off0), "back off restores the exact true-colour image");
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("off native=%016llx hires=%016llx\n", (unsigned long long)off0.native,
           (unsigned long long)off0.hires);
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
