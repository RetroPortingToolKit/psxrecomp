#ifndef PSXRECOMP_WS_REPEAT_RECT_H
#define PSXRECOMP_WS_REPEAT_RECT_H

#include "ws_prepass_guard.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_REPEAT_RECT_TAG_TABLE_SIZE 256u
#define WS_REPEAT_RECT_TAG_PROBES 8u
#define WS_REPEAT_RECT_TAG_FRESH_FRAMES 2u
#define WS_REPEAT_RECT_MAX_SCAN 128

typedef struct {
    int32_t x;
    int32_t u;
    int32_t w;
} WsRepeatSpan;

typedef struct {
    uint32_t command_addr;
    uint32_t frame;
    int32_t period;
    uint8_t used;
    WsPrepassPacketGuard guard;
} WsRepeatRectTag;

static inline int ws_repeat_rect_valid_inputs(int32_t x, int32_t u, int32_t w,
                                              int32_t period,
                                              int32_t canonical_left,
                                              int32_t canonical_width,
                                              int32_t margin, int capacity) {
    return x >= -8192 && x <= 8192 &&
           u >= 0 && u <= 255 &&
           w >= 1 && w <= 256 &&
           u + w <= 256 &&
           period >= 1 && period <= 4096 &&
           canonical_left >= 0 && canonical_left <= 1023 &&
           canonical_width >= 1 && canonical_width <= 1024 &&
           margin >= 0 && margin <= 2048 &&
           capacity >= 0;
}

static inline int64_t ws_repeat_rect_floor_div(int64_t num, int64_t den) {
    int64_t q = num / den;
    int64_t r = num % den;
    return (r != 0 && ((r < 0) != (den < 0))) ? q - 1 : q;
}

static inline int64_t ws_repeat_rect_ceil_div(int64_t num, int64_t den) {
    return -ws_repeat_rect_floor_div(-num, den);
}

static inline int ws_repeat_rect_append_span(WsRepeatSpan *out, int capacity,
                                             int write, int count,
                                             int64_t rect_x, int32_t rect_u,
                                             int32_t rect_w,
                                             int64_t band_left,
                                             int64_t band_right) {
    int64_t rect_left = rect_x;
    int64_t rect_right = rect_x + rect_w;
    int64_t clip_left = rect_left > band_left ? rect_left : band_left;
    int64_t clip_right = rect_right < band_right ? rect_right : band_right;
    if (clip_left >= clip_right)
        return count;

    if (write && count < capacity) {
        out[count].x = (int32_t)clip_left;
        out[count].u = rect_u + (int32_t)(clip_left - rect_left);
        out[count].w = (int32_t)(clip_right - clip_left);
    }
    return count + 1;
}

static inline int ws_repeat_rect_spans(int32_t x, int32_t u, int32_t w,
                                       int32_t period,
                                       int32_t canonical_left,
                                       int32_t canonical_width,
                                       int32_t margin, WsRepeatSpan *out,
                                       int capacity) {
    if (!ws_repeat_rect_valid_inputs(x, u, w, period, canonical_left,
                                     canonical_width, margin, capacity))
        return 0;
    if (margin == 0)
        return 0;
    if (!out)
        return 0;

    int64_t left = canonical_left;
    int64_t right = left + canonical_width;
    int64_t scan_left = left - margin;
    int64_t scan_right = right + margin;
    int64_t n_min = ws_repeat_rect_ceil_div(scan_left - w + 1 - x, period);
    int64_t n_max = ws_repeat_rect_floor_div(scan_right - 1 - x, period);
    if (n_max < n_min)
        return 0;
    if (n_max - n_min + 1 > WS_REPEAT_RECT_MAX_SCAN)
        return 0;

    int count = 0;
    for (int pass = 0; pass < 2; pass++) {
        int write = pass != 0;
        count = 0;
        for (int64_t n = n_min; n <= n_max; n++) {
            int64_t rect_x = (int64_t)x + n * (int64_t)period;
            count = ws_repeat_rect_append_span(out, capacity, write, count,
                                               rect_x, u, w, scan_left, left);
            count = ws_repeat_rect_append_span(out, capacity, write, count,
                                               rect_x, u, w, right,
                                               scan_right);
            if (!write && count > capacity)
                return 0;
        }
    }

    return count;
}

static inline uint32_t ws_repeat_rect_tag_slot(uint32_t command_addr) {
    return (command_addr >> 2) & (WS_REPEAT_RECT_TAG_TABLE_SIZE - 1u);
}

static inline void ws_repeat_rect_tag_clear(WsRepeatRectTag *tags) {
    if (!tags) return;
    for (uint32_t i = 0; i < WS_REPEAT_RECT_TAG_TABLE_SIZE; i++)
        tags[i].used = 0;
}

static inline void ws_repeat_rect_tag_insert(WsRepeatRectTag *tags,
                                             uint32_t command_addr,
                                             int32_t period,
                                             const WsPrepassPacketGuard *guard,
                                             uint32_t frame) {
    if (!tags || !guard || period <= 0)
        return;
    uint32_t idx = ws_repeat_rect_tag_slot(command_addr);
    uint32_t victim = idx;
    for (uint32_t i = 0; i < WS_REPEAT_RECT_TAG_PROBES; i++) {
        uint32_t j = (idx + i) & (WS_REPEAT_RECT_TAG_TABLE_SIZE - 1u);
        WsRepeatRectTag *tag = &tags[j];
        if (!tag->used || tag->command_addr == command_addr) {
            victim = j;
            break;
        }
        if (frame - tag->frame > WS_REPEAT_RECT_TAG_FRESH_FRAMES)
            victim = j;
    }

    tags[victim].used = 1;
    tags[victim].command_addr = command_addr;
    tags[victim].frame = frame;
    tags[victim].period = period;
    tags[victim].guard = *guard;
}

static inline int32_t ws_repeat_rect_tag_lookup(const WsRepeatRectTag *tags,
                                                uint32_t command_addr,
                                                const uint32_t *words,
                                                uint32_t word_count,
                                                uint32_t frame) {
    if (!tags || !words)
        return 0;
    uint32_t idx = ws_repeat_rect_tag_slot(command_addr);
    for (uint32_t i = 0; i < WS_REPEAT_RECT_TAG_PROBES; i++) {
        const WsRepeatRectTag *tag =
            &tags[(idx + i) & (WS_REPEAT_RECT_TAG_TABLE_SIZE - 1u)];
        if (!tag->used || tag->command_addr != command_addr)
            continue;
        if (tag->frame > frame ||
            frame - tag->frame > WS_REPEAT_RECT_TAG_FRESH_FRAMES)
            return 0;
        if (!ws_prepass_packet_matches(&tag->guard, words, word_count))
            return 0;
        return tag->period;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
