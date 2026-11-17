#ifndef PSXRECOMP_GPU_SW_EDGES_H
#define PSXRECOMP_GPU_SW_EDGES_H

#include <stdint.h>

/* PS1 polygon coverage uses a biased 32.32 edge DDA with exclusive bottom
 * and right edges. Float-truncated inclusive spans make adjacent polygons
 * disagree by a pixel and draw a shared diagonal twice. */
static inline int64_t psx_edge_fp(int x) {
    return (int64_t)x * (INT64_C(1) << 32) + ((INT64_C(1) << 32) - (1 << 11));
}

static inline int64_t psx_edge_step(int dx, int dy) {
    /* dy is positive. Round the 32.32 quotient away from zero. Adding
     * a bias before division can overflow when dx is INT32_MIN. */
    int64_t scaled = (int64_t)dx * (INT64_C(1) << 32);
    int64_t step = scaled / dy;
    return step + (scaled % dy != 0 ? (dx < 0 ? -1 : 1) : 0);
}

static inline int psx_edge_unfp(int64_t fp) {
    return (int)((uint64_t)fp >> 32);
}

/* Vertices are sorted by Y; y is in [y0,y2), with y0 < y2.
 * Coordinate differences must fit int (32 bits on supported targets). */
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
