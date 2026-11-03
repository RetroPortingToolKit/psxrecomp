#include "ws_hud_anchor.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void make_guard(WsPrepassPacketGuard *guard,
                       const uint32_t *words, uint32_t count) {
    *guard = ws_prepass_packet_guard(words, count);
}

int main(void) {
    WsHudAnchorTag tags[WS_HUD_ANCHOR_TABLE_SIZE];
    uint32_t packet[] = {
        0x64010203u, 0x00100020u, 0x00040008u, 0x00100010u
    };
    WsPrepassPacketGuard guard;
    int anchor = 99;

    ws_hud_anchor_clear(tags, WS_HUD_ANCHOR_TABLE_SIZE);
    make_guard(&guard, packet, 4u);

    assert(ws_hud_anchor_native_delta(0, 53, -1) == 0);
    assert(ws_hud_anchor_native_delta(1, 0, 1) == 0);
    assert(ws_hud_anchor_native_delta(1, 53, -1) == -53);
    assert(ws_hud_anchor_native_delta(1, 53, 0) == 0);
    assert(ws_hud_anchor_native_delta(1, 53, 1) == 53);
    assert(ws_hud_anchor_apply_native_x(137, 0, 53, -1) == 137);
    assert(ws_hud_anchor_apply_native_x(137, 1, 0, 1) == 137);
    assert(ws_hud_anchor_apply_native_x(137, 1, 53, -1) == 84);
    assert(ws_hud_anchor_apply_native_x(137, 1, 53, 1) == 190);

    ws_hud_anchor_insert(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                         0x80010004u, 9, &guard, 10u);
    assert(ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                0x80010004u, packet, 4u, 10u, &anchor));
    assert(anchor == 1);

    anchor = 99;
    assert(ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                0x80010004u, packet, 4u, 12u, &anchor));
    assert(anchor == 1);

    assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                 0x80010004u, packet, 4u, 9u, &anchor));

    assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                 0x80010004u, packet, 4u, 13u, &anchor));

    packet[3] = 0x00200010u;
    assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                 0x80010004u, packet, 4u, 11u, &anchor));

    packet[3] = 0x00100010u;
    ws_hud_anchor_insert(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                         0x80010004u, -7, &guard, 20u);
    anchor = 99;
    assert(ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                0x80010004u, packet, 4u, 20u, &anchor));
    assert(anchor == -1);

    assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                 0x80010008u, packet, 4u, 20u, &anchor));

    /* GTE-projected tags keep their edge (native-wide still translates by it)
     * and carry the projected flag; plain tags do not. */
    {
        int projected = 7;
        ws_hud_anchor_insert_projected(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                       0x80010004u, 5, &guard, 20u);
        anchor = 0;
        assert(ws_hud_anchor_lookup_result_ex(
                   tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x80010004u, packet, 4u,
                   20u, &anchor, &projected) == WS_HUD_ANCHOR_HIT);
        assert(anchor == 1 && projected == 1);
        ws_hud_anchor_insert_projected(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                       0x80010004u, -3, &guard, 20u);
        assert(ws_hud_anchor_lookup_result_ex(
                   tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x80010004u, packet, 4u,
                   20u, &anchor, &projected) == WS_HUD_ANCHOR_HIT);
        assert(anchor == -1 && projected == 1);
        assert(ws_hud_anchor_native_delta(1, 53, anchor) == -53);
        /* A projected centre tag is not the same as no tag: it must stop
         * automatic layout from squashing the piece a second time. */
        ws_hud_anchor_insert_projected(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                       0x80010004u, 0, &guard, 20u);
        assert(ws_hud_anchor_lookup_result_ex(
                   tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x80010004u, packet, 4u,
                   20u, &anchor, &projected) == WS_HUD_ANCHOR_HIT);
        assert(anchor == 0 && projected == 1);
        ws_hud_anchor_insert(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                             0x80010004u, -7, &guard, 20u);
        assert(ws_hud_anchor_lookup_result_ex(
                   tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x80010004u, packet, 4u,
                   20u, &anchor, &projected) == WS_HUD_ANCHOR_HIT);
        assert(anchor == -1 && projected == 0);
    }

    ws_hud_anchor_clear(tags, WS_HUD_ANCHOR_TABLE_SIZE);
    assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                 0x80010004u, packet, 4u, 20u, &anchor));

    /* Two pending 1000-piece composites, with MMX6's 40-byte packet stride.
     * Losing early arm pieces splits Ilumina when world origin shifts. */
    for (uint32_t i = 0; i < 2000u; ++i)
        ws_hud_anchor_insert(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                             0x10004u + i * 40u, 0, &guard, 30u + i / 1000u);
    for (uint32_t i = 0; i < 2000u; ++i) {
        anchor = 99;
        assert(ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                    0x10004u + i * 40u, packet, 4u, 31u, &anchor));
        assert(anchor == 0);
    }

    puts("ws_hud_anchor_test: PASS");
    return 0;
}
