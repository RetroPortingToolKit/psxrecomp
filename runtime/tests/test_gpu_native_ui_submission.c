/* Reuse real UI grouping, actual GPU command bodies and the established
 * renderer fixture. Compare the new immutable-input classifier and submission
 * with the reference prepass/parser across ordinary and aperture HUD lists. */
#define main reference_ui_checks
#include "test_ws_auto_ui_untextured_exec.c"
#undef main

static void prepare_native_ui(uint32_t address) {
    gpu_ws_native_list_begin();
    for (unsigned safety = 0; safety < 64; ++safety) {
        address = psx_mod_gpu_dma_resolve_address(address);
        const uint32_t header = psx_read_word(address), count = header >> 24;
        uint32_t words[255];
        for (uint32_t i = 0; i < count; ++i)
            words[i] = psx_read_word(psx_mod_gpu_dma_resolve_address(address + 4u + i * 4u));
        gpu_ws_native_list_node(address, header, words);
        if ((header & 0xFFFFFFu) == 0xFFFFFFu) {
            gpu_ws_native_list_end();
            return;
        }
        address = header & 0xFFFFFFu;
    }
    abort();
}

static void submit_ui(int native) {
    uint32_t address = OT_HEAD;
    gpu_ws_begin_linked_list();
    gpu_exec_reset_triangles();
    for (unsigned safety = 0; safety < 64; ++safety) {
        address = psx_mod_gpu_dma_resolve_address(address);
        const uint32_t header = psx_read_word(address), count = header >> 24;
        uint32_t words[255];
        for (uint32_t i = 0; i < count; ++i)
            words[i] = psx_read_word(psx_mod_gpu_dma_resolve_address(address + 4u + i * 4u));
        if (native) gpu_submit_gp0_node(address, count, words);
        else {
            gpu_ws_validate_linked_list_header(address, header);
            gpu_ws_validate_linked_list_node(address, count);
            gpu_set_gp0_linked_list_node(address, count);
            for (uint32_t i = 0; i < count; ++i) {
                gpu_set_gp0_source(psx_mod_gpu_dma_resolve_address(address + 4u + i * 4u));
                gpu_write_gp0(words[i]);
            }
        }
        if ((header & 0xFFFFFFu) == 0xFFFFFFu) {
            gpu_ws_end_linked_list();
            return;
        }
        address = header & 0xFFFFFFu;
    }
    abort();
}

int main(void) {
    assert(reference_ui_checks() == 0);
    unsigned cases = 0;
    for (int in_place = 0; in_place < 2; ++in_place)
    for (int aperture = 0; aperture < 2; ++aperture)
    for (int backing = 0; backing < 2; ++backing)
    for (int offset = 0; offset < 2; ++offset)
    for (int position = 0; position < 3; ++position) {
        if (aperture && backing) continue;
        WsUiPrepassItem *expected = NULL;
        uint32_t expected_count = 0;
        uint16_t expected_rank = 0;
        int expected_draw[3] = {0};
        for (int native = 0; native < 2; ++native) {
            gpu_init();
            test_aperture_used = 0;
            reset_state(in_place);
            draw_offset_x = offset ? 17 : 0;
            const int16_t x = (int16_t)(8 + position * 100);
            build_hud(x, x + 68, x + 4, x + 60);
            if (backing) add_backing_panel(x - 2, x + 70, 18, 38);
            if (aperture) move_hud_to_aperture();
            if (native) prepare_native_ui(OT_HEAD);
            else gpu_ws_prepass_linked_list(OT_HEAD);
            if (!native) {
                expected_count = ws_ui_prepass_count;
                expected_rank = ws_ui_prepass_rank;
                expected = malloc(expected_count * sizeof *expected);
                assert(expected);
                memcpy(expected, ws_ui_prepass, expected_count * sizeof *expected);
            } else {
                assert(ws_ui_prepass_count == expected_count);
                assert(ws_ui_prepass_rank == expected_rank);
                assert(!memcmp(expected, ws_ui_prepass, expected_count * sizeof *expected));
            }
            submit_ui(native);
            if (!native) {
                expected_draw[0] = gpu_exec_triangles.calls;
                expected_draw[1] = gpu_exec_triangles.min_x;
                expected_draw[2] = gpu_exec_triangles.max_x;
            } else {
                assert(gpu_exec_triangles.calls == expected_draw[0]);
                assert(gpu_exec_triangles.min_x == expected_draw[1]);
                assert(gpu_exec_triangles.max_x == expected_draw[2]);
            }
        }
        free(expected);
        ++cases;
    }
    printf("PASS: native UI grouping and submitted HUD geometry match in %u cases\n", cases);
    return 0;
}
