#ifdef NDEBUG
#undef NDEBUG
#endif

/* The squash path reveals a fraction of the game's own half-width, so the
 * per-side cull margin follows the display mode: 320 px keeps the historical
 * 160*(1/s-1), a 512 px mode (Army Men in-game) hides 256*(1/s-1) px per side.
 * Ratio-style sites (angles, cones, scaled bounds) read the 320-basis margin and
 * never change with the mode. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/gpu.c"
#include "gpu_exec_stubs.inc"

static void set_mode(int hres, uint32_t x2) {
    hres1 = (uint32_t)hres; hres2 = 0; video_mode = 0;
    h_display_x1 = 0x260; h_display_x2 = x2;
    v_display_y1 = 0x10; v_display_y2 = 0x100;
}

int main(void) {
    ws_mode = 1;
    ws_gte_game_mode_cfg = 1;               /* game frame: squash is active */
    ws_last_gte_stamp = (uint32_t)s_frame_count;
    ws_cfg_num = 16; ws_cfg_den = 9;
    ws_xnum = 3; ws_xden = 4;                /* 16:9 squash */

    set_mode(1, 0xC60);                      /* 320 wide */
    assert(gpu_h_display_width() == 320);
    assert(psx_ws_x_margin() == 53);
    assert(psx_ws_x_margin_320() == 53);

    set_mode(2, 0xC60);                      /* 512 wide */
    assert(gpu_h_display_width() == 512);
    assert(psx_ws_x_margin() == 85);         /* 256 * (4/3 - 1) */
    assert(psx_ws_x_margin_320() == 53);     /* ratio sites unchanged */
    assert(psx_ws_player_x_bound(160) == 160 * (320 + 2 * 53) / 320);

    ws_xnum = 8; ws_xden = 15;               /* 2.5:1 */
    assert(psx_ws_x_margin() == 224);        /* 256 * (15/8 - 1) */
    assert(psx_ws_x_margin_320() == 140);

    gpu_ws_set_cull_guard_pixels(32);
    assert(psx_ws_x_margin() == 256);
    gpu_ws_set_cull_guard_pixels(0);

    ws_xnum = ws_xden = 1;                   /* true 4:3 stays identity */
    assert(psx_ws_x_margin() == 0);
    assert(psx_ws_x_margin_320() == 0);
    puts("ws_cull_margin_width_test: ok");
    return 0;
}
