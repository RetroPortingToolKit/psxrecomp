#ifndef PSX_WS_FEEDBACK_RECT_H
#define PSX_WS_FEEDBACK_RECT_H

#include <stdint.h>

/* A title-tagged sprite that copies a framebuffer 1:1 (motion blur, trails)
 * only covers the canonical columns. Each span below continues it into one
 * native-wide band, sampling the source buffer's own band the same way. */
typedef struct {
    int32_t dst_x;
    int32_t src_x;
    int32_t w;
} WsFeedbackSpan;

/* x/w: the sprite in VRAM X; src_x0: the source VRAM X drawn at x;
 * left/width: the canonical draw area; margin: the band width. Writes the
 * left band first; returns 0..2. */
static inline int ws_feedback_rect_spans(int32_t x, int32_t w, int32_t src_x0,
                                         int32_t left, int32_t width,
                                         int32_t margin, WsFeedbackSpan out[2]) {
    if (w <= 0 || width <= 0 || margin <= 0) return 0;
    int count = 0;
    if (x <= left) {
        out[count].dst_x = left - margin;
        out[count].src_x = src_x0 + (left - margin - x);
        out[count].w = margin;
        count++;
    }
    if (x + w >= left + width) {
        out[count].dst_x = left + width;
        out[count].src_x = src_x0 + (left + width - x);
        out[count].w = margin;
        count++;
    }
    return count;
}

#endif
