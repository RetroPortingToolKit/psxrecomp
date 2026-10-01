#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include <stdio.h>
#include <stdlib.h>

int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
static uint16_t vram[1024 * 512];
static const uint16_t bank[4] = {0x001f, 0x03e0, 0x0000, 0x8010};
static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void draw(int u, int v, int raw) {
    /* Constant UV makes the expected texel independent of edge conventions. */
    sw_draw_shaded_textured_triangle(100, 100, u, v, 0x404040,
        108, 100, u, v, 0x404040, 100, 108, u, v, 0x404040,
        0, 0, 0x100, raw);
}
static uint16_t pixel(void) { return vram[101 * 1024 + 101]; }
int main(void) {
    sw_renderer_init(vram);
    sw_set_texture_bank(bank, 2, 2);
    gpu_vram_dirty_set_tracking(1);
    gpu_vram_dirty_clear();
    draw(0, 0, 1);
    check(pixel() == 0x001f, "host texel reaches canonical VRAM");
    check(gpu_vram_dirty_any(), "canonical texture writes mark dirty rows");
    draw(1, 0, 1);
    check(pixel() == 0x03e0, "second texel uses host width");
    draw(0, 1, 1);
    check(pixel() == 0x03e0, "transparent host texel preserves destination");
    draw(2, 0, 1);
    check(pixel() == 0x03e0, "out-of-bank coordinates are transparent");
    sw_set_semi_transparency(1, 1);
    draw(1, 1, 1);
    check(pixel() == 0x03f0, "STP texel blends with native destination");
    draw(0, 0, 1);
    check(pixel() == 0x001f, "opaque texel ignores primitive semi-transparency");
    sw_set_mask_bits(1, 0);
    draw(1, 0, 1);
    check(pixel() == 0x83e0, "set-mask applies to bank primitives");
    sw_set_mask_bits(0, 1);
    draw(0, 0, 1);
    check(pixel() == 0x83e0, "masked destination rejects bank primitive");
    sw_set_mask_bits(0, 0);
    draw(0, 0, 0);
    check(pixel() == 0x000f, "vertex color modulates host texel");
    sw_set_texture_window(1); /* clear bit 3 of U */
    draw(8, 0, 1);
    check(pixel() == 0x001f, "texture window applies before bank lookup");
    sw_set_texture_window(0);
    vram[0] = 0x7c00;
    sw_set_texture_bank(NULL, 0, 0);
    draw(0, 0, 1);
    check(pixel() == 0x7c00, "clearing bank restores ordinary VRAM textures");
    sw_set_texture_bank(bank, 2, 2);
    sw_renderer_init(vram);
    draw(0, 0, 1);
    check(pixel() == 0x7c00, "renderer reset clears transient bank selection");
    {
        uint16_t indexed[128] = {0};
        indexed[0] = 0x2222;
        indexed[18] = 0x7fe0;
        indexed[64] = 0x4210;
        sw_set_texture_bank(indexed, 128, 1);
        sw_set_color_modulation(128, 128, 128, 1);
        sw_draw_textured_rect(101, 101, 1, 1, 0, 0, 16, 0, 0);
        check(pixel() == 0x7fe0, "indexed bank fetch uses its own CLUT");
        vram[18] = 0x001f;
        sw_set_texture_bank_live_clut(indexed, 128, 1);
        sw_draw_textured_rect(101, 101, 1, 1, 0, 0, 16, 0, 0);
        check(pixel() == 0x001f, "retained 4-bit indices use the live VRAM palette");
        indexed[0] = 0x0202;
        vram[18] = 0x03e0;
        sw_draw_textured_rect_scaled(101, 101, 2, 1, 1, 0, -1, 1, 16, 0, 0x80);
        check(pixel() == 0x03e0, "mirrored 8-bit retained tiles follow palette changes");
        sw_set_texture_bank(indexed, 128, 1);
        sw_draw_textured_rect(101, 101, 1, 1, 0, 0, 16, 0, 0x80);
        check(pixel() == 0x7fe0, "ordinary bank selection clears live CLUT mode");
        sw_draw_textured_rect(101, 101, 1, 1, 0, 0, 0, 0, 0x101);
        check(pixel() == 0x4210, "bank fetch respects texture page origin");
        check(sw_vram_read(0, 0) == 0x7c00, "bank selection never replaces VRAM readback");
        sw_set_texture_bank(NULL, 0, 0);
    }
    puts("PASS software texture banks");
    return 0;
}
