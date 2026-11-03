/* PS1 dithering ([video] dithering, on by default) on a real, hidden OpenGL
 * context, driven through the renderer facade (gr_*) the way gpu.c drives it.
 *
 * Eligibility, with GP0(E1h) bit 9 set (hardware rule):
 *   - Gouraud polygon .................. dithered (also when its three
 *     vertex colours are equal and 5-bit exact: eligibility is the command,
 *     not the colour)
 *   - shaded line ...................... dithered
 *   - texture-modulated polygon ........ dithered (also at the neutral
 *     0x80 modulation, whose output is 5-bit exact)
 *   - flat polygon, flat line, flat rect, raw-texture polygon,
 *     textured rectangle (modulated or not) ...... never dithered
 * With the bit clear nothing is dithered, and "off" is the true-colour
 * output whatever the bit. Each eligible case is compared against "off"
 * region by region in the native VRAM.
 * The default "on" puts the pattern on the native VRAM grid: the native VRAM
 * at 4x, stepped by dynamic resolution, equals 1x (no guest-visible change
 * across scales). "fine" (elective) dithers per internal pixel. Also: the render thread, PGXP depth
 * and native-wide margins give the same pixels; with accurate_blending the
 * dithered channels stay on the k*8 grid.
 * Original source-owned scene; no retail payload. */
#define main rt_fixture_main
#include "test_gl_render_thread.c"
#undef main

#define DW 320
#define DH 240
static uint16_t nat[1024 * 512];
static uint32_t hi[DW * 4 * DH * 4];
static int k_test_scale = 4;

/* Regions (native px): x, y, w, h */
enum { R_GOURAUD, R_GOURAUD_FLATCOL, R_SHADED_LINE, R_TEX_MOD, R_TEX_NEUTRAL,
       R_FLAT_TRI, R_FLAT_RECT, R_FLAT_LINE, R_TEX_RAW, R_RECT_MOD, R_RECT_RAW, R_N };
static const int REG[R_N][4] = {
    {0, 0, 160, 40},      /* Gouraud gradient */
    {170, 0, 60, 40},     /* Gouraud, equal 5-bit-exact vertex colours */
    {240, 0, 70, 40},     /* shaded lines */
    {0, 50, 70, 60},      /* modulated textured polygon */
    {80, 50, 70, 60},     /* textured polygon at neutral 0x80 modulation */
    {160, 50, 60, 40},    /* flat triangle */
    {230, 50, 60, 40},    /* flat rect */
    {0, 120, 70, 30},     /* flat lines */
    {80, 120, 70, 60},    /* raw textured polygon */
    {160, 120, 64, 64},   /* modulated textured rect */
    {240, 120, 64, 64},   /* raw textured rect */
};
static const char *RNAME[R_N] = {"gouraud", "gouraud equal colours", "shaded line",
    "modulated textured poly", "neutral-modulated textured poly", "flat triangle",
    "flat rect", "flat line", "raw textured poly", "modulated textured rect", "raw textured rect"};
static const int ELIGIBLE[R_N] = {1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};

static void tquad(int x, int y, int w, int h, uint32_t c, int raw) {
    gr_draw_shaded_textured_triangle(x, y, 0, 0, c, x + w, y, 63, 0, c, x, y + h, 0, 63, c, 512, 256, 0x0008, raw);
    gr_draw_shaded_textured_triangle(x + w, y, 63, 0, c, x + w, y + h, 63, 63, c, x, y + h, 0, 63, c, 512, 256, 0x0008, raw);
}
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
    /* Gouraud gradient, black to white, reaching into the left wide margin. */
    if (depth) gr_set_depth_triangle(1, 900.0f, 900.0f, 900.0f);
    gr_draw_gouraud_triangle(-40, 0, 0x0000, 160, 0, 0x7fff, -40, 40, 0x0000);
    if (depth) gr_set_depth_triangle(1, 900.0f, 900.0f, 900.0f);
    gr_draw_gouraud_triangle(160, 0, 0x7fff, 160, 40, 0x7fff, -40, 40, 0x0000);
    /* Gouraud with three equal 5-bit-exact colours. */
    gr_draw_gouraud_triangle(170, 0, 0x4210, 230, 0, 0x4210, 170, 40, 0x4210);
    gr_draw_gouraud_triangle(230, 0, 0x4210, 230, 40, 0x4210, 170, 40, 0x4210);
    for (int i = 0; i < 40; i += 2) gr_draw_shaded_line(240, i, 0x0421, 309, i, 0x5ef7);
    gr_set_color_modulation(128, 128, 128, 0);
    tquad(0, 50, 70, 60, 0x6080a0, 0);
    tquad(80, 50, 70, 60, 0x808080, 0);
    gr_draw_flat_triangle(160, 50, 220, 50, 160, 90, 0x2d6b);
    gr_draw_flat_rect(230, 50, 60, 40, 0x4210);
    for (int i = 0; i < 30; i += 2) gr_draw_line(0, 120 + i, 69, 120 + i, 0x5294);
    tquad(80, 120, 70, 60, 0x6080a0, 1);
    gr_set_color_modulation(0x60, 0x80, 0xa0, 0);
    gr_draw_textured_rect(160, 120, 64, 64, 0, 0, 512, 256, 0x0008);
    gr_set_color_modulation(128, 128, 128, 1);
    gr_draw_textured_rect(240, 120, 64, 64, 0, 0, 512, 256, 0x0008);
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
static int region_equal(const uint16_t *a, const uint16_t *b, int r) {
    const int x = REG[r][0], y = REG[r][1], w = REG[r][2], h = REG[r][3];
    for (int yy = y; yy < y + h; yy++)
        if (memcmp(a + yy * 1024 + x, b + yy * 1024 + x, (size_t)w * 2)) return 0;
    return 1;
}
static double region_mean(const uint16_t *v, int r) {
    long s = 0; int n = 0;
    for (int y = REG[r][1]; y < REG[r][1] + REG[r][3]; y++)
        for (int x = REG[r][0]; x < REG[r][0] + REG[r][2]; x++) {
            uint16_t p = v[y * 1024 + x]; s += (p & 31) + ((p >> 5) & 31) + ((p >> 10) & 31); n += 3;
        }
    return (double)s / n;
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

    check(gl_renderer_dithering() == 1, "on by default");
    /* Off: true colour whatever the bit. */
    gl_renderer_set_dithering(0);
    scene(0, 0); Shot off0 = shot(); memcpy(ref, nat, sizeof ref);
    scene(1, 0); Shot off1 = shot();
    check(same(off0, off1), "off: dither bit changes nothing");
    for (int mode = 1; mode <= 2; mode++) {
        gl_renderer_set_dithering(mode);
        scene(0, 0); Shot clear = shot();
        check(same(off0, clear), "bit clear: nothing is dithered");
        scene(1, 0); Shot on = shot();
        memcpy(dith, nat, sizeof dith);
        check(on.wide != off0.wide, "native-wide margin dithers too");
        for (int r = 0; r < R_N; r++) {
            const int eq = region_equal(ref, dith, r);
            const double m0 = region_mean(ref, r), m1 = region_mean(dith, r);
            printf("mode=%d %-32s %s mean %.2f -> %.2f\n", mode, RNAME[r], eq ? "unchanged" : "dithered", m0, m1);
            char msg[128];
            snprintf(msg, sizeof msg, "%s: %s", RNAME[r], ELIGIBLE[r] ? "dithered" : "never dithered");
            /* "fine" at 4x: the native pack sees the top-left sample of each
             * 4x4 block, i.e. one pattern cell (-4), still a change. */
            check(ELIGIBLE[r] ? !eq : eq, msg);
            /* Mean over the native pack: the default reads the whole pattern;
             * "fine" at 4x reads one pattern cell per block. */
            if (ELIGIBLE[r] && mode == 1) check(fabs(m1 - m0) < 1.0, "dither keeps the mean brightness");
        }
        /* PGXP depth: the same scene, depth-tested. */
        gl_renderer_set_pgxp_depth(1);
        scene(1, 1); Shot dep = shot();
        check(dep.native == on.native, "PGXP depth path dithers the same");
        gl_renderer_set_pgxp_depth(0);
        /* Render thread: bit, mode and per-primitive eligibility recorded. */
        check(gl_renderer_render_thread_start(2) == 1, "render thread started");
        scene(1, 0);
        gl_renderer_render_thread_sync("dither");
        Shot rt = shot();
        gl_renderer_render_thread_stop();
        check(same(rt, on), "render thread gives the same pixels");
        /* With accurate_blending every dithered gradient channel is on the
         * k*8 grid (the surface basis). */
        const int acc0 = gl_renderer_accurate_blending();
        gl_renderer_set_accurate_blending(1);
        scene(1, 0); (void)shot();
        int off_basis = 0;
        for (int y = 0; y < 40 * gr_scale(); y++)
            for (int x = 0; x < 150 * gr_scale(); x++)
                off_basis += ((hi[y * DW * gr_scale() + x] & 0xff) % 8) != 0;
        printf("mode=%d accurate: gradient channels off the k*8 grid=%d\n", mode, off_basis);
        check(off_basis == 0, "dither output follows the accurate (k*8) basis");
        gl_renderer_set_accurate_blending(acc0);
        if (mode == 1) {   /* default: guest VRAM independent of the scale */
            check(gl_renderer_step_internal_scale_now(1) == 1, "step to 1x");
            scene(1, 0); Shot one = shot();
            (void)one;
            /* Lines are drawn as one-native-pixel quads above 1x (see
             * test_gl_scale_invariance.c), so they are not compared. */
            int inv = 1;
            for (int r = 0; r < R_N; r++)
                if (r != R_SHADED_LINE && r != R_FLAT_LINE) inv &= region_equal(dith, nat, r);
            check(inv, "default dithering: native VRAM at 4x equals 1x");
            check(gl_renderer_step_internal_scale_now(k_test_scale) == 1, "back to 4x");
        }
    }
    gl_renderer_set_dithering(0);
    scene(1, 0); Shot back = shot();
    check(same(back, off0), "off again restores the exact true-colour image");
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
