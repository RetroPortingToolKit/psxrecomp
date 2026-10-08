/* Production wiring of the finite-backdrop veto (beads-eio.4.28): the GP0
 * noters in gpu.c feed ws_backdrop_extent.h and the native-wide present
 * decision. Replays the measured overhead-village GP0 shape: a 256+128 px
 * 0x65 ground image at x=-2, then four flat-textured prop quads reaching
 * x=374..428 (exactly the overhang classifier's 4-prim world threshold). */
#include "../src/gpu.c"
#undef NDEBUG
#include <assert.h>

uint64_t s_frame_count;
uint32_t g_psx_ram_mask = 0x1FFFFFu;
uint32_t psx_read_word(uint32_t address) { (void)address; return 0; }
int mdec_recently_active(uint32_t frames) { (void)frames; return 0; }
/* memory.c: the packet key gpu.c uses (no GPU-DMA aperture in this test). */
uint32_t psx_gpu_packet_key(uint32_t address) {
    return psx_gpu_packet_key_for(address, 0);
}

static void sprite(int x, int y, int w, int h) {   /* GP0 0x65 */
    gp0_cmd_buf[0] = 0x65808080u;
    gp0_cmd_buf[1] = ((uint32_t)(uint16_t)y << 16) | (uint16_t)x;
    gp0_cmd_buf[2] = 0x7F880000u;
    gp0_cmd_buf[3] = ((uint32_t)h << 16) | (uint32_t)w;
    ws_note_overhang(0x65); ws_note_backdrop(0x65);
}
static void quad(int x0, int x1, int y0, int y1) { /* GP0 0x2C, flat textured */
    gp0_cmd_buf[0] = 0x2C808080u;
    gp0_cmd_buf[1] = ((uint32_t)(uint16_t)y0 << 16) | (uint16_t)x0;
    gp0_cmd_buf[3] = ((uint32_t)(uint16_t)y0 << 16) | (uint16_t)x1;
    gp0_cmd_buf[5] = ((uint32_t)(uint16_t)y1 << 16) | (uint16_t)x0;
    gp0_cmd_buf[7] = ((uint32_t)(uint16_t)y1 << 16) | (uint16_t)x1;
    ws_note_overhang(0x2C); ws_note_backdrop(0x2C);
}
/* One gameplay frame. `image` draws the ground image first. */
static int frame(int image) {
    ++s_frame_count;
    ws_last_tag_stamp = (uint32_t)s_frame_count;   /* actor funnel active */
    if (image) { sprite(-2, -29, 256, 256); sprite(254, -29, 128, 256); }
    quad(310, 374, -6, 26); quad(340, 388, 100, 164);
    quad(388, 428, 124, 196); quad(329, 409, 188, 236);
    return gpu_ws_present_native_43();
}

int main(void) {
    gp0_cmd_source_addr = 0xFFFFFFFFu;             /* untagged packets */
    ws_mode = 2; ws_cfg_num = 16; ws_cfg_den = 9;  /* native-wide 16:9 */
    assert(ws_nw_configured_offset() > 0);

    /* Prop overhang alone (a real world) presents wide. */
    for (int i = 0; i < 4; ++i) frame(0);
    assert(frame(0) == 0);

    /* The same overhang over a finite ground image: 4:3 once sustained. */
    int f1 = frame(1);
    (void)f1;
    assert(frame(1) == 1);
    for (int i = 0; i < 30; ++i) assert(frame(1) == 1);
    GpuWsDebug ws; gpu_ws_get_debug(&ws);
    assert(ws.bd_veto && ws.bd_full && ws.bd_short && ws.bd_rects == 2);
    /* Reset display (256x240 here): the image still covers >= 90%. */
    assert(ws.bd_min_x == -2 && ws.bd_max_x == 382 && ws.bd_canon_pct >= 90);
    assert(ws.bd_left_pct == 0 && ws.bd_reveal == ws_nw_configured_offset());

    /* Leaving for a scene without the image: wide again after the grace. */
    int wide_after = -1;
    for (int i = 1; i <= 20 && wide_after < 0; ++i)
        if (frame(0) == 0) wide_after = i;
    assert(wide_after > 0 && wide_after <= (int)WS_2D_SCENE_HYSTERESIS + 3);

    /* Native 4:3 configuration never evaluates or vetoes. */
    ws_reset_scene_history();
    ws_cfg_num = 4; ws_cfg_den = 3;
    for (int i = 0; i < 10; ++i) frame(1);
    gpu_ws_get_debug(&ws);
    assert(ws.bd_evaluations == 0 && !ws.bd_veto);

    /* Savestate restore discards the host-derived verdict. */
    ws_cfg_num = 16; ws_cfg_den = 9;
    for (int i = 0; i < 4; ++i) frame(1);
    gpu_ws_get_debug(&ws); assert(ws.bd_veto);
    ws_reset_scene_history();
    gpu_ws_get_debug(&ws); assert(!ws.bd_veto && ws.bd_evaluations == 0);

    puts("PASS village image vetoes prop overhang, field overhang stays wide, "
         "exit grace, 4:3 inert, reset");
    return 0;
}
