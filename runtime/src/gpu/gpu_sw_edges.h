#ifndef PSXRECOMP_GPU_SW_EDGES_H
#define PSXRECOMP_GPU_SW_EDGES_H

#include <stdint.h>

/* PS1 polygon coverage uses a biased 32.32 edge DDA with exclusive bottom
 * and right edges. Float-truncated inclusive spans make adjacent polygons
 * disagree by a pixel and draw a shared diagonal twice. */
static inline int64_t psx_edge_fp(int x) {
    return ((int64_t)x << 32) + ((1LL << 32) - (1 << 11));
}

static inline int64_t psx_edge_step(int dx, int dy) {
    int64_t rounding = dx < 0 ? -(int64_t)(dy - 1)
                              : (dx > 0 ? (int64_t)(dy - 1) : 0);
    return (((int64_t)dx << 32) + rounding) / dy;
}

static inline int psx_edge_unfp(int64_t fp) {
    return (int)((uint64_t)fp >> 32);
}

/* Vertices must already be sorted by Y. y is in [y0,y2). */
static inline void psx_triangle_edges_at_y(int x0, int y0, int x1, int y1,
                                           int x2, int y2, int y,
                                           int *long_x, int *short_x) {
    int64_t long_step = psx_edge_step(x2 - x0, y2 - y0);
    *long_x = psx_edge_unfp(psx_edge_fp(x0) + long_step * (y - y0));

    if (y < y1) {
        int64_t short_step = psx_edge_step(x1 - x0, y1 - y0);
        *short_x = psx_edge_unfp(psx_edge_fp(x0) + short_step * (y - y0));
    } else {
        int64_t short_step = psx_edge_step(x2 - x1, y2 - y1);
        *short_x = psx_edge_unfp(psx_edge_fp(x1) + short_step * (y - y1));
    }
}

#endif
