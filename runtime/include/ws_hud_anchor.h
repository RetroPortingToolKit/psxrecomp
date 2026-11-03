#ifndef PSXRECOMP_WS_HUD_ANCHOR_H
#define PSXRECOMP_WS_HUD_ANCHOR_H

#include "ws_prepass_guard.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Large screen-space composites can exceed 256 packets (MMX6 Ilumina).
 * Keep both pending display buffers' pieces anchored together. */
#define WS_HUD_ANCHOR_TABLE_SIZE 4096u
#define WS_HUD_ANCHOR_PROBES 8u
#define WS_HUD_ANCHOR_FRESH_FRAMES 2u

typedef struct {
    uint32_t command_addr;
    uint32_t frame;
    int8_t anchor;
    uint8_t used;
    /* The primitive's X came out of the GTE and already carries the
     * widescreen squash about the display centre (see ws_gte_sxy.h). The
     * projection path then translates it to the anchor instead of scaling it
     * about the anchor, and never lets automatic UI layout squash it again. */
    uint8_t projected;
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

static inline void ws_hud_anchor_insert_stored(WsHudAnchorTag *tags,
                                               uint32_t count,
                                               uint32_t command_addr,
                                               int anchor, int projected,
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
    tags[victim].projected = projected ? 1u : 0u;
    tags[victim].guard = *guard;
}

static inline void ws_hud_anchor_insert(WsHudAnchorTag *tags, uint32_t count,
                                        uint32_t command_addr, int anchor,
                                        const WsPrepassPacketGuard *guard,
                                        uint32_t frame) {
    ws_hud_anchor_insert_stored(tags, count, command_addr, anchor, 0, guard,
                                frame);
}

/* Tag a GTE-projected piece (see WsHudAnchorTag.projected). */
static inline void ws_hud_anchor_insert_projected(WsHudAnchorTag *tags,
                                                  uint32_t count,
                                                  uint32_t command_addr,
                                                  int edge,
                                                  const WsPrepassPacketGuard *guard,
                                                  uint32_t frame) {
    ws_hud_anchor_insert_stored(tags, count, command_addr, edge, 1, guard,
                                frame);
}

/* Lookup outcome, so callers can keep always-on counters that say WHY a tagged
 * primitive was or was not honoured (absent, expired, or packet rewritten). */
enum {
    WS_HUD_ANCHOR_HIT = 1,
    WS_HUD_ANCHOR_ABSENT = 0,
    WS_HUD_ANCHOR_STALE = -1,
    WS_HUD_ANCHOR_GUARD_MISMATCH = -2
};

static inline int ws_hud_anchor_lookup_result_ex(const WsHudAnchorTag *tags,
                                                 uint32_t count,
                                                 uint32_t command_addr,
                                                 const uint32_t *words,
                                                 uint32_t word_count,
                                                 uint32_t frame,
                                                 int *out_anchor,
                                                 int *out_projected) {
    if (!tags || !count || !words) return WS_HUD_ANCHOR_ABSENT;
    uint32_t idx = ws_hud_anchor_slot(command_addr) & (count - 1u);
    for (uint32_t i = 0; i < WS_HUD_ANCHOR_PROBES && i < count; i++) {
        const WsHudAnchorTag *tag = &tags[(idx + i) & (count - 1u)];
        if (!tag->used || tag->command_addr != command_addr)
            continue;
        if (tag->frame > frame ||
            frame - tag->frame > WS_HUD_ANCHOR_FRESH_FRAMES)
            return WS_HUD_ANCHOR_STALE;
        if (!ws_prepass_packet_matches(&tag->guard, words, word_count))
            return WS_HUD_ANCHOR_GUARD_MISMATCH;
        if (out_anchor) *out_anchor = tag->anchor;
        if (out_projected) *out_projected = tag->projected;
        return WS_HUD_ANCHOR_HIT;
    }
    return WS_HUD_ANCHOR_ABSENT;
}

static inline int ws_hud_anchor_lookup_result(const WsHudAnchorTag *tags,
                                              uint32_t count,
                                              uint32_t command_addr,
                                              const uint32_t *words,
                                              uint32_t word_count,
                                              uint32_t frame,
                                              int *out_anchor) {
    return ws_hud_anchor_lookup_result_ex(tags, count, command_addr, words,
                                          word_count, frame, out_anchor, 0);
}

static inline int ws_hud_anchor_lookup(const WsHudAnchorTag *tags,
                                       uint32_t count,
                                       uint32_t command_addr,
                                       const uint32_t *words,
                                       uint32_t word_count,
                                       uint32_t frame,
                                       int *out_anchor) {
    return ws_hud_anchor_lookup_result(tags, count, command_addr, words,
                                       word_count, frame, out_anchor) ==
           WS_HUD_ANCHOR_HIT;
}

#ifdef __cplusplus
}
#endif

#endif
