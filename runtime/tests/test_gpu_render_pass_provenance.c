/* Exercise the real GPU checkpoint and GP0 provenance consumers without a GL
 * context. RAM rollback deliberately follows GPU restore, as render_pass.c
 * does: a later canonical DMA must find the proof for its restored words. */
#define GPU_EXEC_REAL_UI_GROUP
#define main gpu_packet_fixture_main
#include "test_gpu_textured_dot_nw_shift_exec.c"
#undef main

static const uint32_t command_addr = 0x10004u;
static const uint32_t canonical[] = {
    0x28020304u, 0, 320, 240u << 16, (240u << 16) | 320u
};

static void packet_to_ram(const uint32_t *words, uint32_t count) {
    memcpy(&test_ram[command_addr / 4u], words, count * sizeof(words[0]));
}

static void packet_to_gp0(const uint32_t *words, uint32_t count) {
    gp0_cmd_source_addr = command_addr;
    gp0_words_needed = (int)count;
    memcpy(gp0_cmd_buf, words, count * sizeof(words[0]));
}

static void start_scene(void) {
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    ws_hud_anchor_clear(ws_reveal_clear_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_hud_anchor_clear(ws_screen_mask_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    ws_radial_mask_clear(ws_radial_screen_mask_tags);
    ws_repeat_rect_tag_clear(ws_repeat_rect_tags);
    memset(ws_primitive_roles, 0, sizeof(ws_primitive_roles));
    ws_ui_prepass_count = ws_ui_prepass_node_count = 0;
    ws_ui_prepass_rank = 0xFFFFu;
    ws_reset_scene_history();
    ws_nw_phase_backdrop = 0;
    s_bg_phase_frame = UINT32_MAX;
    packet_to_ram(canonical, 5);
    packet_to_gp0(canonical, 5);
}

static void test_retagged_canonical_packet(void) {
    start_scene();
    gpu_ws_tag_background_prim(command_addr - 4u);
    gpu_ws_tag_hud_prim(command_addr - 4u, -1);
    gpu_ws_tag_world_primitive(command_addr - 4u, 1);
    gpu_ws_tag_hud_primitive(command_addr - 4u, -1);
    const int32_t offset = ws_nw_offset();
    assert(offset > 0);
    GpuWsTagStats before, after;
    gpu_ws_get_tag_stats(NULL, &before);

    /* Repeated in-between draws reuse the same packet address and guest frame. */
    for (uint32_t pass = 1; pass <= 3; ++pass) {
        uint32_t synthetic[5];
        memcpy(synthetic, canonical, sizeof(synthetic));
        synthetic[1] += pass;
        assert(gpu_pass_checkpoint_save());
        packet_to_ram(synthetic, 5);
        packet_to_gp0(synthetic, 5);
        gpu_ws_tag_background_prim(command_addr - 4u);
        gpu_ws_tag_hud_prim(command_addr - 4u, 1);
        gpu_ws_tag_world_primitive(command_addr - 4u, 0);
        gpu_ws_tag_hud_primitive(command_addr - 4u, 1);
        int32_t delta;
        assert(psx_ws_prim_in_backdrop() == 1);
        assert(ws_nw_explicit_hud_delta(&delta) && delta == offset);

        gpu_pass_checkpoint_restore();
        packet_to_ram(canonical, 5);
        packet_to_gp0(canonical, 5);
        assert(psx_ws_prim_in_backdrop() == 1);
        assert(ws_nw_explicit_hud_delta(&delta) && delta == -offset);
        assert(ws_tagged_world_primitive());
        assert(ws_tagged_hud_edge() == -1);
        assert(gpu_ws_background_requires_full_composite());
        packet_to_gp0(synthetic, 5);
        assert(!ws_nw_explicit_background());
        assert(!ws_nw_explicit_hud_delta(&delta));
        packet_to_gp0(canonical, 5);
    }
    gpu_ws_get_tag_stats(NULL, &after);
    assert(after.tag_calls == before.tag_calls + 3);
    assert(after.hit >= before.hit + 6); /* Diagnostic activity is not rolled back. */
}

static void test_pass_only_tags_and_phase(void) {
    /* No image is consumed: discarded/watchdog-aborted passes use this same
     * GPU restore. render_pass_abort_test covers the watchdog control flow
     * separately with a mocked GPU/presenter. */
    start_scene();
    ws_nw_phase_backdrop = 1;
    ws_bg_phase_note(0x28u);
    assert(psx_ws_prim_in_backdrop() == 1);
    assert(!gpu_ws_background_requires_full_composite());
    assert(gpu_pass_checkpoint_save());
    gpu_ws_tag_background_prim(command_addr - 4u);
    gpu_ws_tag_hud_prim(command_addr - 4u, 1);
    gpu_ws_tag_world_primitive(command_addr - 4u, 1);
    ws_bg_phase_note(0x30u);
    assert(gpu_ws_background_requires_full_composite());
    gpu_pass_checkpoint_restore();
    packet_to_ram(canonical, 5);
    packet_to_gp0(canonical, 5);
    assert(!ws_nw_explicit_background());
    assert(!ws_nw_explicit_hud_delta(NULL));
    assert(!gpu_ws_background_stretch_active());
    assert(!gpu_ws_background_requires_full_composite());
    assert(!ws_tagged_world_primitive());
    assert(psx_ws_prim_in_backdrop() == 1); /* Original early-backdrop phase. */
}

static void test_hud_dot_draw(void) {
    start_scene();
    set_dot_packet(command_addr, 20, 20);
    gp0_words_needed = 3;
    uint32_t dot[3];
    memcpy(dot, gp0_cmd_buf, sizeof(dot));
    gpu_ws_tag_hud_prim(command_addr - 4u, -1);
    assert(gpu_pass_checkpoint_save());
    set_dot_packet(command_addr, 80, 20);
    gpu_ws_tag_hud_prim(command_addr - 4u, 1);
    exec_dot_and_expect(80 + ws_nw_offset(), 20);
    gpu_pass_checkpoint_restore();
    packet_to_ram(dot, 3);
    packet_to_gp0(dot, 3);
    exec_dot_and_expect(20 - ws_nw_offset(), 20);
}

static void test_screen_mask_packet(void) {
    start_scene();
    gpu_ws_tag_screen_mask_quad(command_addr - 4u);
    gpu_ws_tag_radial_screen_mask_quad(command_addr - 4u, 2.f);
    assert(gpu_pass_checkpoint_save());
    uint32_t synthetic[5];
    memcpy(synthetic, canonical, sizeof(synthetic));
    synthetic[1] = 16;
    packet_to_ram(synthetic, 5);
    gpu_ws_tag_screen_mask_quad(command_addr - 4u);
    gpu_ws_tag_radial_screen_mask_quad(command_addr - 4u, 3.f);
    gpu_pass_checkpoint_restore();
    packet_to_ram(canonical, 5);
    packet_to_gp0(canonical, 5);
    assert(ws_hud_anchor_lookup(ws_screen_mask_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                               command_addr, gp0_cmd_buf, 5, (uint32_t)s_frame_count, NULL));
    int32_t x[4] = {0, 320, 0, 320}, y[4] = {0, 0, 240, 240};
    assert(ws_nw_radial_mask_transform(x, y));
    assert(x[0] == -160 && x[1] == 480); /* Canonical scale 2, not pass scale 3. */
}

static void test_repeat_rect_packet(void) {
    start_scene();
    const uint32_t rect[] = {0x64808080u, 0, 0, (64u << 16) | 32u};
    uint32_t synthetic[4];
    memcpy(synthetic, rect, sizeof(synthetic));
    synthetic[1] = 8;
    packet_to_ram(rect, 4);
    packet_to_gp0(rect, 4);
    gpu_ws_tag_black_reveal_rect(command_addr - 4u);
    gpu_ws_tag_repeat_rect(command_addr - 4u, 128);
    assert(gpu_pass_checkpoint_save());
    packet_to_ram(synthetic, 4);
    gpu_ws_tag_black_reveal_rect(command_addr - 4u);
    gpu_ws_tag_repeat_rect(command_addr - 4u, 64);
    gpu_pass_checkpoint_restore();
    packet_to_ram(rect, 4);
    packet_to_gp0(rect, 4);
    assert(ws_hud_anchor_lookup(ws_reveal_clear_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                               command_addr, gp0_cmd_buf, 4, (uint32_t)s_frame_count, NULL));
    assert(ws_repeat_rect_tag_lookup(ws_repeat_rect_tags, command_addr,
                                    gp0_cmd_buf, 4, (uint32_t)s_frame_count) == 128);
}

static void test_scene_evidence(void) {
    start_scene();
    ws_full_2d = 0;
    gpu_ws_set_gte_game_mode(1);
    assert(gpu_ws_present_native_43());
    assert(gpu_pass_checkpoint_save());
    psx_ws_note_gte_project(3);
    assert(!gpu_ws_present_native_43());
    gpu_pass_checkpoint_restore();
    packet_to_ram(canonical, 5);
    assert(gpu_ws_present_native_43());
    gpu_ws_set_gte_game_mode(0);
}

static void test_sprite_anchor(void) {
    start_scene();
    /* Sprite-anchor coordinates are consumed by the projection/squash path;
     * native-wide uses the same table only for packet identity. */
    ws_mode = 1;
    ws_xnum = 3;
    ws_xden = 4;
    const uint32_t anchor_addr = 0x20000u;
    ws_anchor_addr = anchor_addr;
    CPUState cpu = {0};
    cpu.read_word = psx_read_word;
    cpu.gpr[4] = command_addr - 4u;
    test_ram[anchor_addr / 4u] = 40;
    psx_ws_sprite_tag(&cpu);
    assert(gpu_pass_checkpoint_save());
    test_ram[anchor_addr / 4u] = 80;
    psx_ws_sprite_tag(&cpu);
    gpu_pass_checkpoint_restore();
    test_ram[anchor_addr / 4u] = 40;
    int32_t anchor;
    assert(ws_tagged_anchor(&anchor) && anchor == 40);
}

static void test_auto_ui_prepass(void) {
    start_scene();
    ws_mode = 1;
    ws_xnum = 3;
    ws_xden = 4;
    gpu_ws_set_auto_ui_squash(1);
    const uint32_t hud[] = {
        0x28802040u, pack_vertex(20, 20), pack_vertex(40, 20),
        pack_vertex(20, 32), pack_vertex(40, 32)
    };
    uint32_t synthetic[5];
    memcpy(synthetic, hud, sizeof(synthetic));
    synthetic[1] = pack_vertex(24, 20);
    synthetic[3] = pack_vertex(24, 32);
    test_ram[(command_addr - 4u) / 4u] = (5u << 24) | 0xFFFFFFu;
    const uint32_t ot_head = 0xFF00u;
    test_ram[ot_head / 4u] = command_addr - 4u; /* Empty OT entry establishes rank 0. */
    packet_to_ram(hud, 5);
    gpu_ws_prepass_linked_list(ot_head);
    packet_to_gp0(hud, 5);
    int32_t anchor;
    assert(ws_auto_ui_anchor(&anchor));
    assert(gpu_pass_checkpoint_save());
    packet_to_ram(synthetic, 5);
    gpu_ws_prepass_linked_list(ot_head);
    gpu_pass_checkpoint_restore();
    packet_to_ram(hud, 5);
    gpu_ws_validate_linked_list_node(command_addr - 4u, 5);
    packet_to_gp0(hud, 5);
    assert(ws_auto_ui_anchor(&anchor));
    packet_to_gp0(synthetic, 5);
    assert(!ws_auto_ui_anchor(&anchor));
    gpu_ws_set_auto_ui_squash(0);
}

int main(void) {
    test_retagged_canonical_packet();
    test_pass_only_tags_and_phase();
    test_hud_dot_draw();
    test_screen_mask_packet();
    test_repeat_rect_packet();
    test_sprite_anchor();
    test_auto_ui_prepass();
    test_scene_evidence();
    puts("gpu_render_pass_provenance: PASS");
    return 0;
}
