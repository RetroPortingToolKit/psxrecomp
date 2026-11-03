/* [video] texture_lod / anisotropic_filtering on a real, hidden OpenGL
 * context, driven through the renderer facade (gr_*).
 *
 *  - off (the default) draws exactly what it always drew; with it on,
 *    untracked (HUD / sprite) primitives and magnified textures are still
 *    byte-identical to off;
 *  - on, a heavily minified one-texel red/blue checker on proven world
 *    geometry averages towards purple instead of aliasing;
 *  - the footprint never leaves the primitive's UV bounds: a green border
 *    packed right next to the sampled texture never bleeds in;
 *  - anisotropic: a texture minified along u only keeps its v detail with
 *    16 taps, where a single isotropic tap box blurs it;
 *  - the render thread and PGXP depth give the same pixels as the direct
 *    path, at 4x internal resolution.
 * Original source-owned scene; no retail payload. */
#define main rt_fixture_main
#include "test_gl_render_thread.c"
#undef main

static uint16_t nat[1024 * 512];

/* 15-bit page at (512,0): 128x128 one-texel checker red/blue, in a 2-texel
 * green border (outside every UV range drawn). Rows 0..127 of a second page
 * at (640,0): horizontal stripes, 2 texels each, red/blue. */
static void pages(void) {
    static uint16_t pg[132 * 132], st[128 * 128];
    for (int y = 0; y < 132; y++)
        for (int x = 0; x < 132; x++) {
            int in = x >= 2 && x < 130 && y >= 2 && y < 130;
            pg[y * 132 + x] = !in ? 0x03e0 : (((x + y) & 1) ? 0x001f : 0x7c00);
        }
    cpu_upload(510, 0, 132, 132, pg);   /* texel (0,0) of page 8 is pg(2,2) */
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) st[y * 128 + x] = ((y >> 1) & 1) ? 0x001f : 0x7c00;
    cpu_upload(640, 0, 128, 128, st);
}
#define TP_CHECK 0x0108   /* 15-bit page at (512,0) */
#define TP_STRIPE 0x010A  /* 15-bit page at (640,0) */

static void tri(int tracked, int x, int y, int w, int h, int u0, int v0, int uw, int vh, uint16_t tp) {
    gr_set_precise_triangle(tracked, x << 16, y << 16, (x + w) << 16, y << 16, x << 16, (y + h) << 16);
    gr_draw_textured_triangle(x, y, u0, v0, x + w, y, u0 + uw, v0, x, y + h, u0, v0 + vh, 0, 0, tp);
    gr_set_precise_triangle(tracked, (x + w) << 16, y << 16, (x + w) << 16, (y + h) << 16, x << 16, (y + h) << 16);
    gr_draw_textured_triangle(x + w, y, u0 + uw, v0, x + w, y + h, u0 + uw, v0 + vh, x, y + h, u0, v0 + vh, 0, 0, tp);
}
static void scene(int depth) {
    gr_set_draw_area(0, 0, 319, 239);
    gr_set_draw_offset(0, 0);
    mask_bits(0, 0);
    gr_set_semi_transparency(0, 0);
    gr_set_color_modulation(128, 128, 128, 1);
    gr_fill_rect(0, 0, 320, 240, 0x0000);
    if (depth) gl_renderer_set_pgxp_depth(1);
    tri(1, 10, 10, 16, 16, 2, 2, 120, 120, TP_CHECK);      /* 7.5:1 minified world */
    tri(0, 40, 10, 16, 16, 2, 2, 120, 120, TP_CHECK);      /* same, untracked (HUD) */
    tri(1, 70, 10, 64, 64, 2, 2, 32, 32, TP_CHECK);        /* magnified world */
    tri(1, 10, 100, 16, 100, 0, 0, 127, 100, TP_STRIPE);   /* u minified only */
    gl_renderer_render_thread_frame_boundary();
    if (depth) gl_renderer_set_pgxp_depth(0);
    gr_vram_transfer_out(0, 0, 1024, 512, nat);
}
static int channel(uint16_t p, int c) { return (p >> (c * 5)) & 31; }
/* Mean |red - blue| over the minified checker: 31 = aliasing, 0 = averaged. */
static double chroma(int x0) {
    long d = 0; int n = 0;
    for (int y = 13; y < 23; y++)
        for (int x = x0 + 3; x < x0 + 9; x++) { uint16_t p = nat[y * 1024 + x]; d += labs(channel(p, 0) - channel(p, 2)); n++; }
    return (double)d / n;
}
static int any_green(void) {
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) if (channel(nat[y * 1024 + x], 1) > 2) return 1;
    return 0;
}
/* Stripe contrast down column 12 of the u-minified quad. */
static double stripe_contrast(void) {
    long d = 0; int n = 0;
    for (int y = 104; y < 190; y++) { uint16_t p = nat[y * 1024 + 12]; d += labs(channel(p, 0) - channel(p, 2)); n++; }
    return (double)d / n;
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
    SDL_Window *win = SDL_CreateWindow("Texture LOD hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(1);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    pages();
    static uint16_t off[1024 * 512], on[1024 * 512];
    check(gl_renderer_texture_lod() == 0, "off by default");
    scene(0); memcpy(off, nat, sizeof off);
    const double c_off = chroma(10), s_off = stripe_contrast();
    check(!any_green(), "baseline samples no border");
    gl_renderer_set_texture_lod(1, 1);
    scene(0); memcpy(on, nat, sizeof on);
    const double c_on = chroma(10), s_iso = stripe_contrast();
    check(region_equal(off, on, 40, 10, 16, 16), "untracked (HUD) primitive untouched");
    check(region_equal(off, on, 70, 10, 64, 64), "magnified texture untouched");
    check(c_on < c_off / 3.0, "minified checker averages instead of aliasing");
    check(!any_green(), "footprint stays inside the primitive's UV bounds");
    gl_renderer_set_texture_lod(1, 16);
    scene(0);
    const double s_aniso = stripe_contrast();
    printf("chroma off=%.2f on=%.2f stripes off=%.2f iso=%.2f aniso16=%.2f\n",
           c_off, c_on, s_off, s_iso, s_aniso);
    check(s_iso < s_off / 2.0, "isotropic box blurs the minor axis");
    check(s_aniso > s_off * 0.8, "16 anisotropic taps keep the minor-axis detail");
    check(!any_green(), "anisotropic taps stay inside the UV bounds");
    memcpy(on, nat, sizeof on);
    scene(1);
    check(memcmp(nat, on, sizeof on) == 0, "PGXP depth path samples the same");
    check(gl_renderer_render_thread_start(2) == 1, "render thread started");
    scene(0);
    gl_renderer_render_thread_sync("lod");
    check(memcmp(nat, on, sizeof on) == 0, "render thread gives the same pixels");
    gl_renderer_render_thread_stop();
    gl_renderer_set_texture_lod(0, 1);
    scene(0);
    check(memcmp(nat, off, sizeof off) == 0, "back off restores the exact image");
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
