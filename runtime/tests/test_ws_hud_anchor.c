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

    ws_hud_anchor_clear(tags, WS_HUD_ANCHOR_TABLE_SIZE);
    assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE,
                                 0x80010004u, packet, 4u, 20u, &anchor));

    puts("ws_hud_anchor_test: PASS");
    return 0;
}
