#pragma once
/* ws_scene_cache.h -- the per-VBlank cache behind
 * [widescreen] scene_predicate_per_frame (gpu.c). Header-only so the rule is
 * unit-tested on its own (tests/test_ws_scene_cache.c). */
#include <stdint.h>

typedef struct WsSceneCache {
    int      valid;
    uint32_t frame;
    int      value;
} WsSceneCache;

static inline void ws_scene_cache_invalidate(WsSceneCache *c) { c->valid = 0; }

/* The predicate's answer for `frame`: evaluated on the first call of each
 * frame (or after an invalidation), reused for every later call in it. With
 * `enabled` 0 it is evaluated every time, exactly as without the cache. */
static inline int ws_scene_cache_get(WsSceneCache *c, int enabled, uint32_t frame,
                                     int (*predicate)(void)) {
    if (!enabled) return predicate() ? 1 : 0;
    if (!c->valid || c->frame != frame) {
        c->value = predicate() ? 1 : 0;
        c->frame = frame;
        c->valid = 1;
    }
    return c->value;
}
