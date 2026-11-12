/* test_sw_bilinear_page_edge.c -- bilinear neighbours at a 256-texel page
 * edge (#719).
 *
 * A sprite whose last texel is u/v=255 must blend its edge with that texel,
 * not with the far side of its own texture: wrapping the neighbour at 256 to
 * 0 before the prim clamp lands on the prim's first column/row. A sprite that
 * genuinely runs past 255 must keep reading the wrapped texels. Nearest
 * sampling is the control. The GL fixture (test_gl_texture_filter.c) checks
 * the same scene on the shader path. */
#include <stdio.h>
#include <string.h>
#include "../src/gpu_sw_renderer.c"

int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
int g_psx_vram_dirty_tracking;
void gpu_vram_dirty_mark_row_impl(uint32_t y) { (void)y; }
void gpu_vram_dirty_mark_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void gpu_vram_dirty_mark_all(void) {}

static uint16_t vram[1024 * 512];
static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

#define RED   0x001f
#define BLUE  0x7c00
#define GREEN 0x03e0

/* 15-bit direct texture page at x=512: blue on column/row 224, green on
 * columns 0..15, red elsewhere. */
#define TEXPAGE ((uint16_t)((2u << 7) | 8u))

static uint16_t at(int x, int y) { return vram[y * 1024 + x] & 0x7fff; }

static void draw(int filter, int *seam, int *wrapped) {
    memset(vram, 0, sizeof vram);
    for (int v = 0; v < 256; v++)
        for (int u = 0; u < 256; u++)
            vram[v * 1024 + 512 + u] = (u == 224 || v == 224) ? BLUE
                                     : u < 16 ? GREEN : RED;
    sw_set_texture_filter(filter);
    /* 2x magnified, so the edge pixels' footprints reach past 255. */
    sw_draw_textured_rect_scaled(20, 300, 64, 64, 224, 224, 256, 256,
                                 0, 0, TEXPAGE);
    sw_draw_textured_rect_scaled(90, 400, 32, 8, 248, 0, 264, 4,
                                 0, 0, TEXPAGE);
    *seam = 0; *wrapped = 0;
    for (int i = 4; i < 64; i++) {
        *seam += at(83, 300 + i) != RED;
        *seam += at(20 + i, 363) != RED;
    }
    for (int y = 400; y < 408; y++)
        for (int x = 108; x < 120; x++)
            *wrapped += at(x, y) != GREEN;
}

int main(void) {
    sw_renderer_init(vram);
    sw_set_draw_area(0, 0, 1023, 511);
    sw_set_draw_offset(0, 0);
    sw_set_color_modulation(128, 128, 128, 1);
    for (int filter = 0; filter < 2; filter++) {
        int seam, wrapped;
        draw(filter, &seam, &wrapped);
        printf("filter=%d seam=%d wrapped=%d\n", filter, seam, wrapped);
        CHECK(seam == 0, "page-edge neighbours do not wrap to the prim's far side");
        CHECK(wrapped == 0, "a sprite past u=255 still samples the wrapped texels");
    }
    if (failures) return 1;
    puts("sw_bilinear_page_edge_test: all checks passed");
    return 0;
}
