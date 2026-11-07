#ifndef PSXRECOMP_WS_REVEAL_EDGE_H
#define PSXRECOMP_WS_REVEAL_EDGE_H
#include <stdint.h>
#include "ws_screen_mask.h"

/* Reveal-edge quads: a title-identified, axis-aligned screen-space quad (a
 * menu bar, a divider line, a tint panel) whose outer vertical boundary
 * already lies on or beyond the canonical display edge grows into the
 * visible native-wide reveal. Only the missing part outside the authored
 * quad is returned, so the original pixels, every interior boundary and the
 * quad's own blend are untouched and a translucent quad never blends twice.
 * Unlike screen masks the quad may sit anywhere vertically. */
enum {
    WS_REVEAL_EDGE_LEFT = 1,
    WS_REVEAL_EDGE_RIGHT = 2,
    WS_REVEAL_EDGE_BOTH = WS_REVEAL_EDGE_LEFT | WS_REVEAL_EDGE_RIGHT
};

/* The tag table stores a clamped anchor (-1, 0, +1): left only, both, right
 * only. */
static inline int ws_reveal_edge_to_anchor(int edges) {
    edges &= WS_REVEAL_EDGE_BOTH;
    if (edges == WS_REVEAL_EDGE_LEFT) return -1;
    if (edges == WS_REVEAL_EDGE_RIGHT) return 1;
    return 0;
}
static inline int ws_reveal_edge_from_anchor(int anchor) {
    if (anchor < 0) return WS_REVEAL_EDGE_LEFT;
    if (anchor > 0) return WS_REVEAL_EDGE_RIGHT;
    return WS_REVEAL_EDGE_BOTH;
}

/* Admissible packets: a flat quad (GP0 0x28-0x2B, 5 words) or a textured
 * quad (0x2C-0x2F, 9 words) whose four texture coordinates name one texel,
 * so an extension of any width samples exactly the authored colour. */
static inline int ws_reveal_edge_packet_ok(const uint32_t *words,
                                           uint32_t count) {
    if (!words) return 0;
    const uint32_t op = (words[0] >> 24) & 0xFCu;
    if (op == 0x28u) return count == 5u;
    if (op != 0x2Cu || count != 9u) return 0;
    const uint32_t uv = words[2] & 0xFFFFu;
    return (words[4] & 0xFFFFu) == uv && (words[6] & 0xFFFFu) == uv &&
           (words[8] & 0xFFFFu) == uv;
}

/* At most one band per requested side, each wholly outside the quad and
 * inside [-margin, width + margin). A full-frame quad is left to the
 * renderer's existing single full-screen expansion, which would otherwise
 * blend the sides twice. */
static inline int ws_reveal_edge_bands(const int32_t x[4], const int32_t y[4],
                                       int width, int height, int margin,
                                       int edges, WsScreenMaskBand out[2]) {
    if (!out || width <= 0 || height <= 0 || margin <= 0) return 0;
    if (x[0] != x[2] || x[1] != x[3] || y[0] != y[1] || y[2] != y[3])
        return 0;
    const int32_t left = x[0] < x[1] ? x[0] : x[1];
    const int32_t right = x[0] < x[1] ? x[1] : x[0];
    const int32_t top = y[0] < y[2] ? y[0] : y[2];
    const int32_t bottom = y[0] < y[2] ? y[2] : y[0];
    if (right <= left || bottom <= top) return 0;
    if (left <= 0 && right >= width && top <= 0 && bottom >= height) return 0;
    int count = 0;
    if ((edges & WS_REVEAL_EDGE_LEFT) && left <= 0 && left > -margin)
        out[count++] = (WsScreenMaskBand){-margin, top, margin + left,
                                          bottom - top};
    if ((edges & WS_REVEAL_EDGE_RIGHT) && right >= width &&
        right < width + margin)
        out[count++] = (WsScreenMaskBand){right, top, width + margin - right,
                                          bottom - top};
    return count;
}
#endif
