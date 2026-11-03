/* [video] texture_filtering = "xbr" on a real, hidden OpenGL context, driven
 * through the renderer facade (gr_*).
 *
 *  - a 45-degree red/blue edge magnified 8x loses its 8-pixel staircase: the
 *    boundary column in each row follows the diagonal closely;
 *  - straight edges and flat areas stay exactly the nearest-texel image, and
 *    a 1:1 (unmagnified) draw is byte-identical to nearest;
 *  - cutouts (texel 0) stay cutouts and never lend their colour;
 *  - the render thread gives the same pixels as the direct path;
 *  - the other filter modes are unchanged by the new one (nearest after xbr
 *    restores the exact image).
 * Original source-owned scene; no retail payload. */
#define main rt_fixture_main
#include "test_gl_render_thread.c"
#undef main

static uint16_t nat[1024 * 512];
#define TP 0x0108   /* 15-bit page at (512,0) */

static void pages(void) {
    static uint16_t pg[32 * 16];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 32; x++) {
            uint16_t c;
            if (x < 16) c = x > y ? 0x001f : 0x7c00;            /* diagonal edge */
            else if (x < 24) c = x < 20 ? 0x001f : 0x7c00;      /* vertical edge */
            else c = (x == 28 && y == 8) ? 0x0000 : 0x03e0;     /* green with a hole */
            pg[y * 32 + x] = c;
        }
    cpu_upload(512, 0, 32, 16, pg);
}
/* Each triangle carries GTE provenance (precise vertices), as world geometry
 * does, so [video] texture_lod sets its LOD bit on these draws. */
static void quad(int x, int y, int w, int h, int u0, int v0, int uw, int vh) {
    gr_set_precise_triangle(1, x << 16, y << 16, (x + w) << 16, y << 16, x << 16, (y + h) << 16);
    gr_draw_textured_triangle(x, y, u0, v0, x + w, y, u0 + uw, v0, x, y + h, u0, v0 + vh, 0, 0, TP);
    gr_set_precise_triangle(1, (x + w) << 16, y << 16, (x + w) << 16, (y + h) << 16, x << 16, (y + h) << 16);
    gr_draw_textured_triangle(x + w, y, u0 + uw, v0, x + w, y + h, u0 + uw, v0 + vh, x, y + h, u0, v0 + vh, 0, 0, TP);
}
static void scene(void) {
    gr_set_draw_area(0, 0, 319, 239);
    gr_set_draw_offset(0, 0);
    mask_bits(0, 0);
    gr_set_semi_transparency(0, 0);
    gr_set_color_modulation(128, 128, 128, 1);
    gr_fill_rect(0, 0, 320, 240, 0x1084);
    quad(0, 0, 128, 128, 0, 0, 16, 16);        /* diagonal, 8x */
    quad(140, 0, 64, 128, 16, 0, 8, 16);       /* vertical edge, 8x */
    quad(210, 0, 64, 128, 24, 0, 8, 16);       /* flat green with a hole, 8x */
    quad(0, 150, 32, 16, 0, 0, 32, 16);        /* 1:1 */
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(0, 0, 1024, 512, nat);
}
/* Mean distance between each row's first red-dominant column and the ideal
 * diagonal (x = y + 0.5 texel, in pixels). */
static double stair_error(void) {
    double e = 0; int n = 0;
    for (int y = 4; y < 124; y++) {
        int edge = 128;
        for (int x = 0; x < 128; x++) {
            uint16_t p = nat[y * 1024 + x];
            if ((p & 31) > ((p >> 10) & 31)) { edge = x; break; }
        }
        e += fabs((double)edge - ((double)y + 4.0)); n++;
    }
    return e / n;
}
static int region_equal(const uint16_t *a, const uint16_t *b, int x, int y, int w, int h) {
    for (int r = y; r < y + h; r++)
        if (memcmp(a + r * 1024 + x, b + r * 1024 + x, (size_t)w * 2)) return 0;
    return 1;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("xBR hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(1);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    pages();
    static uint16_t ref[1024 * 512], x[1024 * 512];
    gr_set_texture_filter(0);
    scene(); memcpy(ref, nat, sizeof ref);
    const double e_near = stair_error();
    gr_set_texture_filter(3);
    check(gr_texture_filter() == 3, "xbr selectable");
    scene(); memcpy(x, nat, sizeof x);
    const double e_xbr = stair_error();
    printf("stair error nearest=%.2f xbr=%.2f\n", e_near, e_xbr);
    check(e_xbr < e_near * 0.7, "diagonal staircase smoothed");
    check(region_equal(ref, x, 140, 0, 64, 128), "straight edge unchanged");
    check(region_equal(ref, x, 210, 0, 64, 128), "flat area and cutout unchanged");
    check(region_equal(ref, x, 0, 150, 32, 16), "1:1 draw identical to nearest");
    /* texture_lod adds bit 4 to the filter key; xbr must still run (the
     * magnified quads keep their texels under LOD). */
    gl_renderer_set_texture_lod(1, 16);
    scene();
    check(memcmp(nat, x, sizeof x) == 0, "xbr unchanged with texture_lod on");
    gl_renderer_set_texture_lod(0, 1);
    check(gl_renderer_render_thread_start(2) == 1, "render thread started");
    scene();
    gl_renderer_render_thread_sync("xbr");
    check(memcmp(nat, x, sizeof x) == 0, "render thread gives the same pixels");
    gl_renderer_render_thread_stop();
    gr_set_texture_filter(0);
    scene();
    check(memcmp(nat, ref, sizeof ref) == 0, "nearest after xbr restores the exact image");
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
