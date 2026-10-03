#ifndef PSXRECOMP_WS_SCREEN_MASK_H
#define PSXRECOMP_WS_SCREEN_MASK_H
#include <stdint.h>

/* Extend only an off-screen vertical boundary of a title-identified mask.
 * The interior/slanted light boundary stays at its original coordinates.
 * Result is wholly outside canonical VRAM; no triangle size limit applies. */
typedef struct { int x, y, w, h; } WsScreenMaskBand;
static inline int ws_screen_mask_band(const int32_t x[4], const int32_t y[4],
                                      int width, int height, int margin,
                                      WsScreenMaskBand *out) {
    if (!out || width <= 0 || height <= 0 || margin <= 0 ||
        y[0] != y[1] || y[2] != y[3] || y[0] != 0 || y[2] != height)
        return 0;
    if (x[0] == x[2] && x[0] <= 0 && x[1] > x[0] && x[3] > x[0] &&
        x[0] > -margin) {
        *out = (WsScreenMaskBand){-margin, 0, margin + x[0], height};
        return 1;
    }
    if (x[1] == x[3] && x[1] >= width && x[0] < x[1] && x[2] < x[1] &&
        x[1] < width + margin) {
        *out = (WsScreenMaskBand){x[1], 0, width + margin - x[1], height};
        return 1;
    }
    return 0;
}

/* A tagged horizontal letterbox panel covers the native width but only the
 * top or bottom of the picture. Extend both outer edges, keeping its animated
 * height and leaving the opening alone. Full-screen filters use the renderer's
 * existing single-rectangle path instead, avoiding a second blend in the sides.
 * At most two disjoint bands are returned, all outside canonical VRAM. */
static inline int ws_screen_mask_bands(const int32_t x[4], const int32_t y[4],
                                       int width, int height, int margin,
                                       WsScreenMaskBand out[2]) {
    if (!out || width <= 0 || height <= 0 || margin <= 0) return 0;
    if (x[0] == x[2] && x[1] == x[3] &&
        y[0] == y[1] && y[2] == y[3] &&
        x[0] <= 0 && x[1] >= width && y[2] > y[0]) {
        if (y[0] <= 0 && y[2] >= height) return 0;
        if (y[0] <= 0 || y[2] >= height) {
            int count = 0;
            if (x[0] > -margin)
                out[count++] = (WsScreenMaskBand){-margin, y[0],
                                                 margin + x[0], y[2] - y[0]};
            if (x[1] < width + margin)
                out[count++] = (WsScreenMaskBand){x[1], y[0],
                                                 width + margin - x[1], y[2] - y[0]};
            return count;
        }
    }
    return ws_screen_mask_band(x, y, width, height, margin, out);
}
#endif
