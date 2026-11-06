#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "ws_reveal_edge.h"
#include "ws_hud_anchor.h"

/* Bands only extend an outer boundary that already reaches the canonical
 * edge, only on requested sides, never overlap the quad and never touch a
 * full-frame quad (the renderer's single full-screen path owns that). */
static void test_geometry(void) {
    WsScreenMaskBand b[2];
    /* A menu bar authored from x=-20 to 532 on a 512-wide display. */
    int32_t x[4] = {-20, 532, -20, 532}, y[4] = {-20, -20, 42, 42};
    assert(ws_reveal_edge_bands(x, y, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b) == 2);
    assert(b[0].x == -85 && b[0].w == 65 && b[0].y == -20 && b[0].h == 62);
    assert(b[1].x == 532 && b[1].w == 65 && b[1].y == -20 && b[1].h == 62);
    assert(ws_reveal_edge_bands(x, y, 512, 256, 85, WS_REVEAL_EDGE_LEFT, b) == 1);
    assert(b[0].x == -85 && b[0].w == 65);
    assert(ws_reveal_edge_bands(x, y, 512, 256, 85, WS_REVEAL_EDGE_RIGHT, b) == 1);
    assert(b[0].x == 532 && b[0].w == 65);
    assert(ws_reveal_edge_bands(x, y, 512, 256, 85, 0, b) == 0);

    /* Divider line: only the end at the display edge grows; the interior end
     * at x=40 (joined to other artwork) stays where it was authored. */
    int32_t lx[4] = {40, 532, 40, 532}, ly[4] = {20, 20, 21, 21};
    assert(ws_reveal_edge_bands(lx, ly, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b) == 1);
    assert(b[0].x == 532 && b[0].w == 65 && b[0].y == 20 && b[0].h == 1);

    /* Tint halves in the middle of the screen: each grows on its outer side. */
    int32_t tx[4] = {-24, 256, -24, 256}, ty[4] = {40, 40, 200, 200};
    assert(ws_reveal_edge_bands(tx, ty, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b) == 1);
    assert(b[0].x == -85 && b[0].w == 61 && b[0].y == 40 && b[0].h == 160);
    tx[0] = tx[2] = 256; tx[1] = tx[3] = 536;
    assert(ws_reveal_edge_bands(tx, ty, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b) == 1);
    assert(b[0].x == 536 && b[0].w == 61);

    /* Exactly on the edge grows by the whole margin. */
    int32_t ex[4] = {0, 512, 0, 512}, ey[4] = {10, 10, 20, 20};
    assert(ws_reveal_edge_bands(ex, ey, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b) == 2);
    assert(b[0].x == -85 && b[0].w == 85 && b[1].x == 512 && b[1].w == 85);

    /* Interior quad, or one already covering the reveal: nothing to add. */
    int32_t ix[4] = {1, 511, 1, 511};
    assert(!ws_reveal_edge_bands(ix, ey, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b));
    int32_t cx[4] = {-128, 641, -128, 641};
    assert(!ws_reveal_edge_bands(cx, ey, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b));
    cx[0] = cx[2] = -85; cx[1] = cx[3] = 597;
    assert(!ws_reveal_edge_bands(cx, ey, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b));

    /* Full-frame quad belongs to the renderer's full-screen expansion. */
    int32_t fx[4] = {0, 512, 0, 512}, fy[4] = {0, 0, 256, 256};
    assert(!ws_reveal_edge_bands(fx, fy, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b));
    fy[0] = fy[1] = 1;   /* not full height: a panel again */
    assert(ws_reveal_edge_bands(fx, fy, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b) == 2);

    /* Mirrored quads are still axis aligned. */
    int32_t mx[4] = {532, -20, 532, -20};
    assert(ws_reveal_edge_bands(mx, y, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b) == 2);

    /* Slanted, degenerate, no reveal, bad display. */
    int32_t sx[4] = {-20, 532, -10, 532};
    assert(!ws_reveal_edge_bands(sx, y, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b));
    int32_t sy[4] = {-20, -19, 42, 42};
    assert(!ws_reveal_edge_bands(x, sy, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b));
    int32_t zy[4] = {5, 5, 5, 5};
    assert(!ws_reveal_edge_bands(x, zy, 512, 256, 85, WS_REVEAL_EDGE_BOTH, b));
    assert(!ws_reveal_edge_bands(x, y, 512, 256, 0, WS_REVEAL_EDGE_BOTH, b));
    assert(!ws_reveal_edge_bands(x, y, 0, 256, 85, WS_REVEAL_EDGE_BOTH, b));
    assert(!ws_reveal_edge_bands(x, y, 512, 256, 85, WS_REVEAL_EDGE_BOTH, NULL));

    /* Every band stays inside [-margin, width + margin) and off the quad. */
    for (int margin = 1; margin < 400; margin += 7)
        for (int left = -margin - 5; left <= 5; left += 3) {
            int32_t qx[4] = {left, 600, left, 600}, qy[4] = {3, 3, 9, 9};
            int n = ws_reveal_edge_bands(qx, qy, 512, 256, margin,
                                         WS_REVEAL_EDGE_BOTH, b);
            for (int i = 0; i < n; i++) {
                assert(b[i].w > 0 && b[i].x >= -margin);
                assert(b[i].x + b[i].w <= 512 + margin);
                assert(b[i].x + b[i].w <= left || b[i].x >= 600);
            }
        }
}

static void test_packets(void) {
    /* Flat quad, opaque and translucent. */
    uint32_t f4[5] = {0x28604020u, 0, 0, 0, 0};
    assert(ws_reveal_edge_packet_ok(f4, 5));
    f4[0] = 0x2A604020u;
    assert(ws_reveal_edge_packet_ok(f4, 5));
    assert(!ws_reveal_edge_packet_ok(f4, 4));
    /* Textured quad with one texel (16,24) in all four UVs: CLUT (512,480),
     * texture page 5. */
    uint32_t ft4[9] = {0x2C808080u, 0, 0x78201810u, 0, 0x00051810u,
                       0, 0x00001810u, 0, 0x00001810u};
    assert(ws_reveal_edge_packet_ok(ft4, 9));
    ft4[0] = 0x2E808080u;
    assert(ws_reveal_edge_packet_ok(ft4, 9));
    for (int i = 2; i <= 8; i += 2) {   /* any differing texel rejects */
        ft4[i] ^= 1u;
        assert(!ws_reveal_edge_packet_ok(ft4, 9));
        ft4[i] ^= 1u;
    }
    assert(!ws_reveal_edge_packet_ok(ft4, 8));
    /* Other primitives: triangles, Gouraud, sprites, lines. */
    const uint32_t ops[] = {0x20, 0x24, 0x30, 0x38, 0x3C, 0x40, 0x60, 0x64};
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        uint32_t w[12] = {ops[i] << 24};
        for (uint32_t n = 1; n <= 12; n++) assert(!ws_reveal_edge_packet_ok(w, n));
    }
    assert(!ws_reveal_edge_packet_ok(NULL, 5));
}

static void test_tag_storage(void) {
    /* The side set survives the clamped anchor field of the tag table. */
    for (int e = WS_REVEAL_EDGE_LEFT; e <= WS_REVEAL_EDGE_BOTH; e++)
        assert(ws_reveal_edge_from_anchor(
                   ws_hud_anchor_clamp(ws_reveal_edge_to_anchor(e))) == e);

    /* Packet guard and lifetime are the shared anchor-table rules. */
    WsHudAnchorTag tags[WS_HUD_ANCHOR_TABLE_SIZE] = {0};
    uint32_t words[5] = {0x2A604020u, 0x0028FFE8u, 0x00280100u,
                         0x00C8FFE8u, 0x00C80100u};
    WsPrepassPacketGuard guard = ws_prepass_packet_guard(words, 5);
    ws_hud_anchor_insert(tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x012340u,
                         ws_reveal_edge_to_anchor(WS_REVEAL_EDGE_RIGHT),
                         &guard, 40);
    int anchor = 0;
    assert(ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x012340u,
                                words, 5, 40, &anchor));
    assert(ws_reveal_edge_from_anchor(anchor) == WS_REVEAL_EDGE_RIGHT);
    assert(ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x012340u,
                                words, 5, 40 + WS_HUD_ANCHOR_FRESH_FRAMES, NULL));
    assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x012340u,
                                 words, 5, 41 + WS_HUD_ANCHOR_FRESH_FRAMES, NULL));
    for (int i = 0; i < 5; i++) {
        words[i] ^= 0x10u;
        assert(!ws_hud_anchor_lookup(tags, WS_HUD_ANCHOR_TABLE_SIZE, 0x012340u,
                                     words, 5, 40, NULL));
        words[i] ^= 0x10u;
    }
}

int main(void) {
    test_geometry();
    test_packets();
    test_tag_storage();
    puts("ws_reveal_edge_test: PASS");
    return 0;
}
