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
    /* A camera lock inside continuous geometry must not become a new scene
     * edge. An explicit scene bound still anchors at the real outer edges. */
    half_at(0x80097202u, 1968); half_at(0x80097216u, 1968);
    gpu_ws_set_view_bounds_override(1, 0, 5120);
    ws_view_sample();
    assert(ws_view_current().left == 53 && ws_view_current().shift == 0);
    half_at(0x80097202u, 0); ws_view_sample();
    assert(ws_view_current().left == 0 && ws_view_current().right == 106);
    gpu_ws_set_view_bounds_override(0, 0, 5120);
    half_at(0x80097202u, 1968); ws_view_sample();
    assert(ws_view_current().left == 0 && ws_view_current().right == 106);
    half_at(0x80097216u, 0); half_at(0x80097202u, 0);
    /* With the option disabled every original BG helper remains centered. */
    gpu_ws_set_view_anchor(0, 0, 0, 0);
    assert(psx_ws_bg2d_cols(21) == 29);
    assert(psx_ws_bg2d_startx(0) == -64);
    assert(psx_ws_bg2d_stream_right(336) == 400);
    ws_mode = 0;
    assert(psx_ws_bg2d_cols(21) == 21);
    assert(psx_ws_bg2d_startx(0) == 0);
    /* The opt-in host arena keeps guest loops and the ring native, while
     * its packet coordinates remain signed 16-bit beyond PS1's +/-1024. */
    configure_native_wide_16_9();
    gpu_ws_bg2d_set_host_arena(0x80100000u, 64u); /* mapped fixture memory */
    half_at(0x10001c, 0x4247u); half_at(0x10001e, 0x5836u);
    gp0_cmd_source_addr = 0x00100004u;
    int32_t px, py;
    parse_vertex(pack_vertex(1400, 20), &px, &py);
    assert(px == 1400 && py == 20);
    /* Reflection reverses the exact 16 texels, without reading the adjacent
     * tile's first column. Its metadata is confined to the registered arena. */
    half_at(0x10001e, 0xd836u);
    gp0_cmd_buf[0]=0x7d808080u;
    gp0_cmd_buf[1]=pack_vertex(400,20);
    gp0_cmd_buf[2]=0x79804000u;
    gp0_exec_textured_16x16();
    assert(last_scaled_rect.calls==1 && last_scaled_rect.x==400);
    assert(last_scaled_rect.w==16 && last_scaled_rect.h==16);
    assert(last_scaled_rect.u0==15 && last_scaled_rect.u1==-1);
    ws_mode = 0;
    gp0_cmd_buf[1]=pack_vertex(20,20);
    int before_resize_calls=last_scaled_rect.calls;
    gp0_exec_textured_16x16();
    assert(last_scaled_rect.calls==before_resize_calls+1 && last_scaled_rect.x==20);
    configure_native_wide_16_9();
    half_at(0x10001e, 0x5836u);
    /* New metadata packs a bank ID beside a signed view shift; old saved
     * packet metadata remains supported, with no accidental sign extension. */
    half_at(0x10001c, 0x4248u);
    half_at(0x100010, (uint16_t)-53); half_at(0x100012, 0x6001u);
    assert(ws_view_packet().shift == -53);
    half_at(0x10001c, 0x4247u); half_at(0x100012, 0xffffu);
    assert(ws_view_packet().shift == -53);
    gp0_cmd_source_addr = 0x00010004u;
    parse_vertex(pack_vertex(1400, 20), &px, &py);
    assert(px == -648 && py == 20); /* Ordinary hardware packet unchanged. */
    gpu_ws_tag_world_primitive(0x80010000u, 1);
    parse_vertex(pack_vertex(1400, 20), &px, &py);
    assert(px == 1400 && py == 20);
    parse_vertex(pack_vertex(-1400, 20), &px, &py);
    assert(px == -1400 && py == 20);
    gpu_ws_tag_world_primitive(0x80010000u, 0);
    parse_vertex(pack_vertex(1400, 20), &px, &py);
    assert(px == -648 && py == 20);
    assert(psx_ws_bg2d_cols(21) == 21);
    assert(psx_ws_bg2d_startcol(3, 63) == 3);
    assert(psx_ws_bg2d_startx(-5) == -5);
    assert(psx_ws_bg2d_stream_left(100) == 100);
    assert(psx_ws_bg2d_stream_right(436) == 436);
    gpu_ws_bg2d_set_host_arena(0, 0);
    puts("ws_view_anchor_gpu: sampled camera, per-layer coverage, packet origin, disabled identity PASS");
    return 0;
}
