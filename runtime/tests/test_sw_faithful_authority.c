/* test_sw_faithful_authority.c -- the netplay CPU-authoritative rasterizer
 * draws the hardware image whatever presentation enhancements are armed.
 *
 * Under netplay the software rasterizer (dual-raster SW@1x, or the software
 * GPU) owns the guest-visible VRAM: GP0 C0 readback and savestates come from
 * it. PGXP's perspective/precise overrides depend on host-only shadows (a
 * rollback load drops them on one peer only) and the texture filter is each
 * player's Display setting, so neither may reach those pixels: V8:2 reads its
 * own frame back pixel by pixel. sw_set_faithful_authority(1) must make an
 * enhanced draw byte-identical to the plain one, and turning it off must
 * restore the player's filter. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/gpu_sw_renderer.c"

int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
static int test_local_split_width;
int gpu_ws_local_split_source_width(void) { return test_local_split_width; }
int g_psx_vram_dirty_tracking;
void gpu_vram_dirty_mark_row_impl(uint32_t y) { (void)y; }
void gpu_vram_dirty_mark_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void gpu_vram_dirty_mark_all(void) {}

static uint16_t vram[1024 * 512];
static uint16_t plain[256 * 200], out[256 * 200];

/* 15-bit direct texture page at x=512: every texel opaque and distinct. */
#define TEXPAGE ((uint16_t)((2u << 7) | 8u))

static void reset_vram(void) {
    memset(vram, 0, sizeof vram);
    for (int v = 0; v < 256; v++)
        for (int u = 0; u < 256; u++)
            vram[v * 1024 + 512 + u] =
                (uint16_t)(((u * 7) & 31) | (((v * 5) & 31) << 5) |
                           ((((u ^ v) * 3) & 31) << 10) | 0x8000u);
}

static void draw(int perspective, int precise, int filter, uint16_t *dst) {
    reset_vram();
    sw_set_texture_filter(filter);
    sw_set_color_modulation(16, 16, 16, 1);
    if (precise)
        sw_set_precise_triangle(1, 10 << 16 | 0x8000, 260 << 16 | 0x4000,
                                230 << 16 | 0x8000, 280 << 16,
                                40 << 16, 455 << 16 | 0xC000);
    if (perspective) sw_set_perspective_triangle(1, 1.0f, 0.2f, 0.45f);
    sw_draw_textured_triangle(10, 260, 0, 0,
                              230, 280, 250, 10,
                              40, 455, 20, 240,
                              0, 0, TEXPAGE);
    for (int y = 0; y < 200; y++)
        memcpy(dst + y * 256, vram + (260 + y) * 1024, 256 * sizeof(uint16_t));
}

int main(void) {
    sw_renderer_init(vram);
    test_local_split_width=160;
    sw_wide_configure(424,52);sw_wide_set_target(0);
    sw_wide_clear(0,0,240,0);
    sw_set_draw_area(0,0,158,239);sw_draw_flat_rect(-32,40,224,40,0x001f);
    sw_set_draw_area(161,0,319,239);sw_draw_flat_rect(128,40,224,40,0x03e0);
    assert(vram[60*1024]==0x001f && vram[60*1024+158]==0x001f);
    assert(vram[60*1024+159]==0 && vram[60*1024+160]==0);
    assert(vram[60*1024+161]==0x03e0 && vram[60*1024+319]==0x03e0);
    assert(g_wide_cur[60*424]==0x001f && g_wide_cur[60*424+210]==0x001f);
    assert(g_wide_cur[60*424+211]==0 && g_wide_cur[60*424+212]==0);
    assert(g_wide_cur[60*424+213]==0x03e0 && g_wide_cur[60*424+423]==0x03e0);
    sw_wide_disable_target();sw_wide_configure(0,0);test_local_split_width=0;
    sw_set_draw_area(0,0,1023,511);
    assert(!sw_faithful_authority());

    draw(0, 0, 0, plain);
    int lit = 0;
    for (int i = 0; i < 256 * 200; i++) lit += plain[i] != 0;
    assert(lit > 10000);

    /* Offline: each enhancement changes the native pixels. */
    draw(1, 0, 0, out);
    assert(memcmp(out, plain, sizeof plain) != 0);
    draw(0, 0, 1, out);
    assert(memcmp(out, plain, sizeof plain) != 0);

    /* Faithful authority: the same requests draw the hardware image. */
    sw_set_texture_filter(1);
    sw_set_faithful_authority(1);
    assert(sw_faithful_authority() && sw_texture_filter() == 0);
    draw(1, 1, 1, out);
    assert(memcmp(out, plain, sizeof plain) == 0);
    /* A filter change while armed is remembered, not applied. */
    sw_set_texture_filter(1);
    assert(sw_texture_filter() == 0);

    /* Off again: the player's filter is back and enhancements apply. */
    sw_set_faithful_authority(0);
    assert(sw_texture_filter() == 1);
    draw(1, 0, 1, out);
    assert(memcmp(out, plain, sizeof plain) != 0);

    printf("sw_faithful_authority_test: all checks passed\n");
    return 0;
}
