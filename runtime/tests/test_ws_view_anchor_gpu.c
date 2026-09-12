/* Reuse the executable GPU fixture, including its existing HUD regression. */
#define main textured_dot_regression_main
#include "test_gpu_textured_dot_nw_shift_exec.c"
#undef main

static void half_at(uint32_t addr, uint16_t value) {
    memcpy((uint8_t *)test_ram + (addr & 0x1fffffu), &value, 2);
}
int main(void) {
    assert(textured_dot_regression_main() == 0);
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    const uint32_t lower_sites[] = {0x80029fd0u};
    assert(!psx_ws_is_cull_bias_lower_site(lower_sites[0]));
    gpu_ws_set_bias_lower_cull_sites(lower_sites, 1);
    assert(psx_ws_is_cull_bias_lower_site(0xa0029fd0u));
    assert(!psx_ws_is_cull_bias_lower_site(0x80029fd4u));
    gpu_ws_set_bias_lower_cull_sites(NULL, 0);
    assert(!psx_ws_is_cull_bias_lower_site(lower_sites[0]));
    gpu_ws_set_view_anchor(0x80097202u, 0x80097216u, 0x80097214u, 0x800971f8u);
    half_at(0x80097214u, 5120);
    half_at(0x80097216u, 0);
    for (unsigned i = 0; i < 3; i++) {
        uint32_t b = 0x971f8u + i * 0x54;
        ((uint8_t *)test_ram)[b] = 1;
        ((uint8_t *)test_ram)[b + 0x52] = 255;
        ((uint8_t *)test_ram)[b + 0x4e] = 25;
    }
    half_at(0x80097202u, 50);
    half_at(0x80097256u, 25);
    half_at(0x800972aau, 12);
    g_mmx6_freshfix = 0; /* This fixture isolates geometry from tile decoding. */
    gpu_ws_bg2d_begin_view_layer(0, 0x80010000u, 6);
    assert(psx_ws_bg2d_startcol(3, 63) == 63);
    assert(psx_ws_bg2d_cols(21) == 29);
    gpu_ws_bg2d_end_view_layer(0, 0x80011000u);
    gpu_ws_bg2d_begin_view_layer(1, 0x80011000u, 6);
    assert(psx_ws_bg2d_startcol(1, 63) == 63);
    assert(psx_ws_bg2d_cols(21) == 29);
    gpu_ws_bg2d_end_view_layer(1, 0x80012000u);
    gpu_ws_bg2d_begin_view_layer(2, 0x80012000u, 6);
    assert(psx_ws_bg2d_startcol(0, 63) == 63);
    assert(psx_ws_bg2d_cols(21) == 28);
    gpu_ws_bg2d_end_view_layer(2, 0x80013000u);
    gp0_cmd_source_addr = 0x12004;
    WsViewAnchor bg = ws_view_packet();
    assert(bg.shift == -41 && bg.left == 12 && bg.right == 94);
    gp0_cmd_source_addr = 0x10004;
    assert(ws_view_packet().shift == -3);
    /* Transient camera writes do not change geometry after renderer setup. */
    half_at(0x80097202u, 0);
    assert(ws_view_current().left == 50);
    s_frame_count++;
    gpu_ws_bg2d_begin_view_layer(0, 0x80014000u, 6);
    assert(psx_ws_bg2d_startcol(0, 63) == 0);
    assert(psx_ws_bg2d_cols(21) == 28);
    assert(ws_view_current().left == 0 && ws_view_current().right == 106);
    /* With the option disabled every original BG helper remains centered. */
    gpu_ws_set_view_anchor(0, 0, 0, 0);
    assert(psx_ws_bg2d_cols(21) == 29);
    assert(psx_ws_bg2d_startx(0) == -64);
    assert(psx_ws_bg2d_stream_right(336) == 400);
    ws_mode = 0;
    assert(psx_ws_bg2d_cols(21) == 21);
    assert(psx_ws_bg2d_startx(0) == 0);
    puts("ws_view_anchor_gpu: sampled camera, per-layer coverage, packet origin, disabled identity PASS");
    return 0;
}
