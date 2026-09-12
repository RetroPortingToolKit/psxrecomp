#ifndef PSXRECOMP_WS_HUD_ANCHOR_H
#define PSXRECOMP_WS_HUD_ANCHOR_H

#include "ws_prepass_guard.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_HUD_ANCHOR_TABLE_SIZE 256u
#define WS_HUD_ANCHOR_PROBES 8u
#define WS_HUD_ANCHOR_FRESH_FRAMES 2u

typedef struct {
    uint32_t command_addr;
    uint32_t frame;
    int8_t anchor;
    uint8_t used;
    WsPrepassPacketGuard guard;
} WsHudAnchorTag;

static inline int ws_hud_anchor_clamp(int anchor) {
    if (anchor < 0) return -1;
    if (anchor > 0) return 1;
    return 0;
}

static inline int32_t ws_hud_anchor_native_delta(int native_active,
                                                 int32_t offset,
                                                 int anchor) {
    if (!native_active || offset <= 0) return 0;
    return (int32_t)ws_hud_anchor_clamp(anchor) * offset;
}

static inline int32_t ws_hud_anchor_apply_native_x(int32_t x,
                                                   int native_active,
                                                   int32_t offset,
                                                   int anchor) {
    return x + ws_hud_anchor_native_delta(native_active, offset, anchor);
}

static inline uint32_t ws_hud_anchor_slot(uint32_t command_addr) {
    return (command_addr >> 2) & (WS_HUD_ANCHOR_TABLE_SIZE - 1u);
}

static inline void ws_hud_anchor_clear(WsHudAnchorTag *tags,
                                       uint32_t count) {
    if (!tags) return;
    for (uint32_t i = 0; i < count; i++)
        tags[i].used = 0;
}

static inline void ws_hud_anchor_insert(WsHudAnchorTag *tags, uint32_t count,
                                        uint32_t command_addr, int anchor,
                                        const WsPrepassPacketGuard *guard,
                                        uint32_t frame) {
    if (!tags || !count || !guard) return;
    uint32_t idx = ws_hud_anchor_slot(command_addr) & (count - 1u);
    uint32_t victim = idx;
    for (uint32_t i = 0; i < WS_HUD_ANCHOR_PROBES && i < count; i++) {
        uint32_t j = (idx + i) & (count - 1u);
        WsHudAnchorTag *tag = &tags[j];
        if (!tag->used || tag->command_addr == command_addr) {
            victim = j;
            break;
        }
        if (frame - tag->frame > WS_HUD_ANCHOR_FRESH_FRAMES)
            victim = j;
    }
    tags[victim].used = 1;
    tags[victim].command_addr = command_addr;
    tags[victim].frame = frame;
    tags[victim].anchor = (int8_t)ws_hud_anchor_clamp(anchor);
    tags[victim].guard = *guard;
}

static inline int ws_hud_anchor_lookup(const WsHudAnchorTag *tags,
                                       uint32_t count,
                                       uint32_t command_addr,
                                       const uint32_t *words,
                                       uint32_t word_count,
                                       uint32_t frame,
                                       int *out_anchor) {
    if (!tags || !count || !words) return 0;
    uint32_t idx = ws_hud_anchor_slot(command_addr) & (count - 1u);
    for (uint32_t i = 0; i < WS_HUD_ANCHOR_PROBES && i < count; i++) {
        const WsHudAnchorTag *tag = &tags[(idx + i) & (count - 1u)];
        if (!tag->used || tag->command_addr != command_addr)
            continue;
        if (tag->frame > frame ||
            frame - tag->frame > WS_HUD_ANCHOR_FRESH_FRAMES)
            return 0;
        if (!ws_prepass_packet_matches(&tag->guard, words, word_count))
            return 0;
        if (out_anchor) *out_anchor = tag->anchor;
        return 1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
