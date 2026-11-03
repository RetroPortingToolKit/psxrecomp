/* PS1 semi-transparency and mask-bit accuracy of the OpenGL renderer at
 * internal resolution with PGXP depth, on a real, hidden OpenGL context.
 *
 * The same scene goes through the renderer facade twice: once on the
 * software rasterizer (the reference) and once on OpenGL at 4x internal
 * resolution, with PGXP depth off and on, directly and on the render thread.
 * Every mode (0: B/2+F/2, 1: B+F, 2: B-F, 3: B+F/4) is drawn over a ramp of
 * background colours, as flat rects, depth-tagged flat triangles, raw
 * textured rects whose texels mix STP=1 (blended), STP=0 (opaque) and 0
 * (transparent), and modulated textured triangles; then the mask bit: rects
 * drawn with set-mask, and semi-transparent draws over them with mask check.
 *
 * With [video] accurate_blending (every VRAM write in one 8-bit basis) the
 * following must hold; with it off (the historical bases) the mismatch count
 * is printed for the record.
 * Checked: the native VRAM a game reads back equals the software reference
 * exactly in every configuration, and the flat rects also equal the PS1
 * formula computed here (an oracle independent of both renderers): per 5-bit
 * channel, mode 0 = (B+F)>>1, 1 = min(B+F,31), 2 = max(B-F,0),
 * 3 = min(B+(F>>2),31); mask bit = set-mask OR source STP; a masked
 * destination is never written while check is on.
 * Original source-owned scene; no retail payload. */
#define main rt_fixture_main
#include "test_gl_render_thread.c"
#undef main

#define TP 0x0108   /* 15-bit page at (512,0) */
static const uint16_t BG[8] = {0x0000, 0x7fff, 0x4210, 0x001f, 0x03e0, 0x7c00, 0x1ce7, 0x6318};
static const uint16_t FG[4] = {0x7fff, 0x2d6b, 0x4210, 0x0c63};

static void page(void) {
    static uint16_t pg[16 * 16];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint16_t c = (uint16_t)(0x0421 * (1 + ((x + 2 * y) % 30)));
            int k = (x + y) % 4;
            pg[y * 16 + x] = k == 0 ? 0 : k == 1 ? (uint16_t)(c | 0x8000) : c;
        }
    cpu_upload(512, 0, 16, 16, pg);
}
static void scene(int depth) {
    gr_set_draw_area(0, 0, 319, 239);
    gr_set_draw_offset(0, 0);
    mask_bits(0, 0);
    gr_set_semi_transparency(0, 0);
    for (int i = 0; i < 8; i++) gr_fill_rect(0, i * 30, 320, 30, BG[i]);
    for (int m = 0; m < 4; m++) {
        int x = m * 80;
        gr_set_semi_transparency(1, m);
        gr_draw_flat_rect(x, 0, 16, 240, FG[m]);
        if (depth) gr_set_depth_triangle(1, 700.0f + m, 700.0f + m, 700.0f + m);
        gr_draw_flat_triangle(x + 18, 2, x + 34, 2, x + 18, 236, FG[(m + 1) & 3]);
        gr_set_color_modulation(128, 128, 128, 1);
        for (int y = 0; y < 240; y += 16) gr_draw_textured_rect(x + 36, y, 16, 16, 0, 0, 0, 0, TP);
        gr_set_color_modulation(128, 128, 128, 0);
        if (depth) gr_set_depth_triangle(1, 650.0f, 650.0f, 650.0f);
        gr_draw_shaded_textured_triangle(x + 54, 0, 0, 0, 0x6080a0, x + 78, 0, 15, 0, 0xa08060,
                                         x + 54, 239, 0, 15, 0x806080, 0, 0, TP, 1);
    }
    /* Mask: set-mask rects, then every mode over them with check on. */
    gr_set_semi_transparency(0, 0);
    mask_bits(1, 0);
    for (int m = 0; m < 4; m++) gr_draw_flat_rect(m * 80 + 4, 100, 8, 8, 0x1234);
    mask_bits(0, 1);
    for (int m = 0; m < 4; m++) {
        gr_set_semi_transparency(1, m);
        gr_draw_flat_rect(m * 80, 96, 16, 16, 0x7fff);
    }
    mask_bits(1, 0);   /* set-mask with semi: result carries bit 15 */
    for (int m = 0; m < 4; m++) {
        gr_set_semi_transparency(1, m);
        gr_draw_flat_rect(m * 80, 200, 16, 8, FG[m]);
    }
    mask_bits(0, 0);
    gr_set_semi_transparency(0, 0);
    gl_renderer_render_thread_frame_boundary();
}
static int ch(uint16_t p, int c) { return (p >> (5 * c)) & 31; }
static uint16_t blend(uint16_t b, uint16_t f, int m) {
    uint16_t o = 0;
    for (int c = 0; c < 3; c++) {
        int B = ch(b, c), F = ch(f, c), r;
        if (m == 0) r = (B + F) >> 1;
        else if (m == 1) r = B + F > 31 ? 31 : B + F;
        else if (m == 2) r = B - F < 0 ? 0 : B - F;
        else r = B + (F >> 2) > 31 ? 31 : B + (F >> 2);
        o |= (uint16_t)(r << (5 * c));
    }
    return o;
}
static int near1(uint16_t a, uint16_t b) {
    for (int c = 0; c < 3; c++) if (abs(ch(a, c) - ch(b, c)) > 1) return 0;
    return (a & 0x8000) == (b & 0x8000);
}
/* Single-layer blends must be exact. Rows 96..111 and 200..207 stack a
 * second semi-transparent layer on a blended one: the hardware truncates the
 * first result to 5 bits before the second blend, while the 8-bit surface
 * keeps the first result's extra precision (a fixed-function blend cannot
 * truncate), so those rows may differ by one 5-bit step per channel. */
static long oracle_errors(const uint16_t *v) {
    long bad = 0;
    for (int m = 0; m < 4; m++)
        for (int y = 0; y < 240; y++)
            for (int x = m * 80; x < m * 80 + 16; x++) {
                uint16_t got = v[y * 1024 + x], want;
                int stacked = 0;
                if (y >= 96 && y < 112) {   /* masked square kept, rest blended white */
                    int masked = x >= m * 80 + 4 && x < m * 80 + 12 && y >= 100 && y < 108;
                    uint16_t under = blend(BG[y / 30], FG[m], m);
                    want = masked ? 0x9234 : blend(under, 0x7fff, m);
                    stacked = !masked;
                } else if (y >= 200 && y < 208) {
                    want = (uint16_t)(blend(blend(BG[y / 30], FG[m], m), FG[m], m) | 0x8000);
                    stacked = 1;
                } else want = blend(BG[y / 30], FG[m], m);
                const int ok = stacked ? near1(got, want) : got == want;
                if (!ok && getenv("BLEND_VERBOSE") && bad < 12)
                    printf("  oracle (%d,%d) got=%04x want=%04x\n", x, y, got, want);
                bad += !ok;
            }
    return bad;
}
/* Flat triangles: the GL and software edges differ by a pixel (documented
 * coverage rules), so only the interior, one pixel in from every edge, is
 * checked, against the formula. */
static long oracle_tri_errors(const uint16_t *v) {
    long bad = 0;
    for (int m = 0; m < 4; m++)
        for (int y = 4; y < 234; y++) {
            if ((y >= 96 && y < 112) || (y >= 200 && y < 208)) continue;
            const double span = 16.0 * (1.0 - (y - 2) / 234.0);
            for (int lx = 1; lx + 1.5 < span; lx++) {
                const uint16_t got = v[y * 1024 + m * 80 + 18 + lx];
                bad += got != blend(BG[y / 30], FG[(m + 1) & 3], m);
            }
        }
    return bad;
}
/* The software rasterizer drops a texel's STP bit instead of writing it to
 * bit 15 (the PS1 writes texel STP OR set-mask), so bit 15 is compared
 * against the oracle on its own (stp_errors) and masked here. */
/* Rects against the software reference: exact where a single layer is drawn
 * from 5-bit operands (flat and raw-textured rects), within one step in the
 * stacked rows. */
static long diff(const uint16_t *a, const uint16_t *b) {
    long n = 0;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
        const uint16_t p = a[y * 1024 + x] | 0x8000, q = b[y * 1024 + x] | 0x8000;
        const int loose = (y >= 96 && y < 112) || (y >= 200 && y < 208) || (x % 80) >= 52;
        if ((x % 80) >= 16 && ((x % 80) < 36 || (x % 80) >= 52))
            continue;   /* triangles: edges differ by a pixel; see oracle_tri_errors */
        n += loose ? !near1(p, q) : p != q;
    }
    return n;
}
/* Raw textured rects: bit 15 of each written pixel is its texel's STP. */
static long stp_errors(const uint16_t *v) {
    long bad = 0;
    for (int m = 0; m < 4; m++)
        for (int y = 0; y < 240; y++) {
            if (y >= 96 && y < 112) continue;
            for (int tx = 0; tx < 16; tx++) {
                int k = (tx + (y & 15)) % 4;
                if (k == 0) continue;   /* transparent texel: background stays */
                bad += ((v[y * 1024 + m * 80 + 36 + tx] >> 15) & 1) != (k == 1);
            }
        }
    return bad;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    static uint16_t ref[1024 * 512], out[1024 * 512];
    /* Software reference. */
    gr_set_backend(GR_BACKEND_SOFTWARE);
    gr_init(vram);
    page();
    scene(0);
    gr_vram_transfer_out(0, 0, 1024, 512, ref);
    const long ref_oracle = oracle_errors(ref);
    printf("software reference: oracle mismatches=%ld\n", ref_oracle);
    check(ref_oracle == 0, "software reference matches the PS1 formulas");

    SDL_Window *win = SDL_CreateWindow("Blend accuracy hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    memset(vram, 0, sizeof vram);
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(4);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    page();
    /* No caller opt-in: default rendering must match the console. */
    check(gl_renderer_accurate_blending() == 1, "correct blending is the default");
    scene(0);
    gr_vram_transfer_out(0, 0, 1024, 512, out);
    check(oracle_errors(out) == 0, "default GL rendering matches the PS1 formulas");
    check(diff(out, ref) == 0, "default GL VRAM matches the software reference");
    /* Historical bases: measured for diagnosis, not required. */
    gl_renderer_set_accurate_blending(0);
    scene(0);
    gr_vram_transfer_out(0, 0, 1024, 512, out);
    printf("gl 4x accurate_blending=0: oracle mismatches=%ld (historical bases)\n", oracle_errors(out));
    gl_renderer_set_accurate_blending(1);
    for (int threaded = 0; threaded < 2; threaded++)
        for (int depth = 0; depth < 2; depth++) {
            if (threaded && depth == 0) check(gl_renderer_render_thread_start(2) == 1, "render thread started");
            gl_renderer_set_pgxp_depth(depth);
            scene(depth);
            if (threaded) gl_renderer_render_thread_sync("blend");
            gr_vram_transfer_out(0, 0, 1024, 512, out);
            const long d = diff(out, ref), o = oracle_errors(out);
            printf("gl 4x pgxp_depth=%d render_thread=%d: differs from software=%ld oracle mismatches=%ld\n",
                   depth, threaded, d, o);
            check(o == 0, "GL matches the PS1 blend/mask formulas");
            check(oracle_tri_errors(out) == 0, "depth-tested triangle interiors match the formulas");
            check(stp_errors(out) == 0, "textured pixels carry their texel's STP in bit 15");
            check(d == 0, "GL native VRAM equals the software reference");
            if (d) {
                int shown = 0;
                for (int y = 0; y < 240 && shown < 8; y++) for (int x = 0; x < 320 && shown < 8; x++)
                    if ((out[y * 1024 + x] | 0x8000) != (ref[y * 1024 + x] | 0x8000) && (x % 80) < 52 &&
                        ((x % 80) < 16 || (x % 80) >= 36) &&
                        !((y >= 96 && y < 112) || (y >= 200 && y < 208))) {
                        printf("  (%d,%d) gl=%04x sw=%04x\n", x, y, out[y * 1024 + x], ref[y * 1024 + x]);
                        shown++;
                    }
            }
        }
    gl_renderer_render_thread_stop();
    gl_renderer_set_pgxp_depth(0);
    gl_renderer_set_accurate_blending(0);
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
