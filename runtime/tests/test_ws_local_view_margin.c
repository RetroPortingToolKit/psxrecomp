#ifdef NDEBUG
#undef NDEBUG
#endif

/* Netplay own-view widescreen: with gpu_ws_set_local_view_only(1) the cull
 * margin the guest sees is the stock 0 everywhere except inside a sandboxed
 * local-view render (gpu_ws_set_local_view_scope), where it is the margin the
 * configured native-wide aspect implies. Off (the default) changes nothing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/gpu.c"
#include "gpu_exec_stubs.inc"

int main(void) {
    ws_mode = 2;
    ws_cfg_num = 16; ws_cfg_den = 9;
    hres1 = 1; hres2 = 0; video_mode = 0; /* 320 wide */
    h_display_x1 = 0x260; h_display_x2 = 0xC60;
    v_display_y1 = 0x10; v_display_y2 = 0x100;
    const int wide = psx_ws_x_margin();
    assert(wide > 0);
    assert(!gpu_ws_local_view_only());

    gpu_ws_set_local_view_only(1);
    assert(psx_ws_x_margin() == 0);           /* shared simulation: stock */
    assert(psx_ws_player_x_bound(160) == 160);
    gpu_ws_set_local_view_scope(1);
    assert(psx_ws_x_margin() == wide);        /* own view: this peer's */
    gpu_ws_set_local_view_scope(0);
    assert(psx_ws_x_margin() == 0);

    gpu_ws_set_local_view_only(0);
    assert(psx_ws_x_margin() == wide);        /* offline: unchanged */
    puts("ws_local_view_margin_test: ok");
    return 0;
}
