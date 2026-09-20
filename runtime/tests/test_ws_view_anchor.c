#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ws_view_anchor.h"
#include "../src/gpu_sw_renderer.c"

int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
int g_psx_vram_dirty_tracking;
void gpu_vram_dirty_mark_row_impl(uint32_t y) { (void)y; }
void gpu_vram_dirty_mark_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void gpu_vram_dirty_mark_all(void) {}

static uint16_t native[1024 * 512], reference[1024 * 512];

static void check(int cx, int lo, int hi, int left, int right, int shift) {
    WsViewAnchor v = ws_view_anchor(53, cx, lo, hi);
    assert(v.left == left && v.right == right && v.shift == shift);
    assert(v.left + v.right + v.pad_left + v.pad_right == 106);
    assert(53 + v.shift - v.left == v.pad_left);
    assert(53 + v.shift + 320 + v.right == 426 - v.pad_right);
}

static void draw(int shift, int pad_left, int pad_right) {
    sw_wide_clear(0, 0, 240, 0);
    memset(native, 0, sizeof(native));
    sw_wide_set_view(1, shift, pad_left, pad_right);
    /* Every column has a distinct world color. Include both potential reveal
     * regions so the renderer, rather than the fixture, decides visibility. */
    for (int x = -106; x < 426; x++)
        sw_draw_flat_rect(x, 40, 1, 1, (uint16_t)(0x4000 | ((x + 107) & 1023)));
    /* Existing left/center/right HUD placements, independent of world origin. */
    sw_wide_set_view(1, 0, 0, 0);
    sw_draw_flat_rect(19 - 53, 10, 3, 1, 31);
    sw_draw_flat_rect(160, 10, 3, 1, 31);
    sw_draw_flat_rect(300 + 53, 10, 3, 1, 31);
}

int main(void) {
    /* Same framing at 32:9 / 64:9 and beyond; finite rooms keep symmetric
     * padding while a full stage transfers the reveal to the available side. */
    const int extras[] = {267, 693, 1600};
    for (unsigned i = 0; i < sizeof extras / sizeof extras[0]; ++i) {
        int e = extras[i];
        WsViewAnchor left = ws_view_anchor(e,0,0,5120);
        WsViewAnchor right = ws_view_anchor(e,5120,0,5120);
        WsViewAnchor room = ws_view_anchor(e,200,200,200);
        assert(left.left==0 && left.right==2*e && left.shift==-e);
        assert(right.left==2*e && right.right==0 && right.shift==e);
        assert(room.left==0 && room.right==0 && room.shift==0);
        assert(room.pad_left==e && room.pad_right==e);
    }
    check(0, 0, 1000, 0, 106, -53);
    check(20, 0, 1000, 20, 86, -33);
    check(500, 0, 1000, 53, 53, 0);
    check(980, 0, 1000, 86, 20, 33);
    check(1000, 0, 1000, 106, 0, 53);
    check(200, 200, 200, 0, 0, 0);
    check(200, 200, 249, 0, 49, -25);
    check(249, 200, 249, 49, 0, 24);
    /* Continuous, monotonic origin for every pixel of a full-room traversal. */
    int previous = -53;
    for (int x = 0; x <= 1000; x++) {
        WsViewAnchor v = ws_view_anchor(53, x, 0, 1000);
        assert(v.shift >= previous && v.shift <= previous + 1);
        previous = v.shift;
    }
    assert(ws_view_anchor(53, -1, 0, 1000).shift == 0);
    assert(ws_view_anchor(53, 500, 1000, 0).shift == 0);
    assert(ws_view_anchor(0, 0, 0, 1000).left == 0);

    sw_renderer_init(native);
    sw_set_draw_area(0, 0, 319, 239);
    sw_wide_configure(426, 53);
    sw_wide_set_target(0);
    draw(0, 0, 0);
    memcpy(reference, native, sizeof(native));
    for (int shift = -53; shift <= 53; shift++) {
        draw(shift, 0, 0);
        assert(memcmp(reference, native, sizeof(native)) == 0);
        assert(g_wide_cur[40 * 426] == (uint16_t)(0x4000 | (54 - shift)));
        assert(g_wide_cur[40 * 426 + 425] == (uint16_t)(0x4000 | (479 - shift)));
        assert(g_wide_cur[10 * 426 + 19] == 31);
        assert(g_wide_cur[10 * 426 + 213] == 31);
        assert(g_wide_cur[10 * 426 + 406] == 31);
    }
    draw(-25, 28, 29);
    for (int x = 0; x < 426; x++)
        assert((g_wide_cur[40 * 426 + x] != 0) == (x >= 28 && x < 397));
    assert(g_wide_cur[10 * 426 + 19] == 31); /* HUD can occupy padding. */
    sw_wide_set_view(0, 77, 28, 29);
    assert(wide_dx() == 53 && rt_wide().cx1 == 0 && rt_wide().cx2 == 425);
    puts("ws_view_anchor: geometry, edge pixels, fixed HUD, narrow-room clip, canonical identity PASS");
    return 0;
}
