#ifndef PSX_WS_PRIMITIVE_ROLES_H
#define PSX_WS_PRIMITIVE_ROLES_H
#include <stdint.h>
#include <stddef.h>
#include "psx_memory.h"

/* Title-supplied packet identity, independent of draw order and sprite tags.
 * Keys name PsyQ P_TAG headers; GPU lookup names the following command word. */
#define WS_ROLE_BUCKETS 4096u
typedef struct {
    uint32_t key, stamp;
    int8_t hud_edge;
    uint8_t world;
} WsPrimitiveRole;

static inline WsPrimitiveRole* ws_role_write(WsPrimitiveRole* tags,
                                             uint32_t primitive, uint32_t now) {
    uint32_t key = psx_ram_canonical_offset(primitive) & ~3u;
    if (!key) return NULL;
    uint32_t slot = (key >> 2) & (WS_ROLE_BUCKETS - 1u), victim = slot;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t j = (slot + i) & (WS_ROLE_BUCKETS - 1u);
        if (tags[j].key == key || !tags[j].key) { victim = j; break; }
        if (now - tags[j].stamp > 2u) victim = j;
    }
    WsPrimitiveRole* tag = &tags[victim];
    if (tag->key != key || tag->stamp != now) {
        tag->hud_edge = 0;
        tag->world = 0;
    }
    tag->key = key;
    tag->stamp = now;
    return tag;
}

static inline const WsPrimitiveRole* ws_role_read(const WsPrimitiveRole* tags,
                                                  uint32_t source, uint32_t now) {
    if (source == 0xFFFFFFFFu) return NULL;
    uint32_t key = psx_ram_canonical_offset(source - 4u) & ~3u;
    if (!key) return NULL;
    uint32_t slot = (key >> 2) & (WS_ROLE_BUCKETS - 1u);
    for (unsigned i = 0; i < 4; ++i) {
        const WsPrimitiveRole* tag = &tags[(slot + i) & (WS_ROLE_BUCKETS - 1u)];
        if (tag->key == key && now - tag->stamp <= 2u) return tag;
    }
    return NULL;
}
#endif
