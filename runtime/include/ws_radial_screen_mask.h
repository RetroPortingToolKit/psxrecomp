#ifndef PSXRECOMP_WS_RADIAL_SCREEN_MASK_H
#define PSXRECOMP_WS_RADIAL_SCREEN_MASK_H
#include "ws_hud_anchor.h"
#include <math.h>

/* Explicit title-owned screen-space rings, never a world-geometry heuristic.
 * Independent packet guards let adjacent solid and feathered ring segments
 * share one scale without allowing an arena's next use to inherit the effect. */
#define WS_RADIAL_MASK_TAG_COUNT 256u
typedef struct {
    WsHudAnchorTag packet;
    float scale;
} WsRadialScreenMaskTag;

static inline void ws_radial_mask_clear(WsRadialScreenMaskTag *tags) {
    for (unsigned i=0; i<WS_RADIAL_MASK_TAG_COUNT; ++i) tags[i].packet.used=0;
}
static inline void ws_radial_mask_insert(WsRadialScreenMaskTag *tags,
    uint32_t key, const WsPrepassPacketGuard *guard, uint32_t frame, float scale) {
    if (!guard || !isfinite(scale) || scale < 1.f || scale > 64.f) return;
    unsigned slot=(key >> 2) & (WS_RADIAL_MASK_TAG_COUNT-1u), victim=slot;
    for (unsigned i=0; i<WS_HUD_ANCHOR_PROBES; ++i) {
        unsigned j=(slot+i)&(WS_RADIAL_MASK_TAG_COUNT-1u);
        WsHudAnchorTag *p=&tags[j].packet;
        if (!p->used || p->command_addr==key) { victim=j; break; }
        if (frame-p->frame > WS_HUD_ANCHOR_FRESH_FRAMES) victim=j;
    }
    tags[victim].packet=(WsHudAnchorTag){.command_addr=key, .frame=frame,
                                       .used=1, .guard=*guard};
    tags[victim].scale=scale;
}
static inline int ws_radial_mask_lookup(const WsRadialScreenMaskTag *tags,
    uint32_t key, const uint32_t *words, uint32_t count, uint32_t frame,
    float *scale) {
    unsigned slot=(key >> 2)&(WS_RADIAL_MASK_TAG_COUNT-1u);
    for (unsigned i=0; i<WS_HUD_ANCHOR_PROBES; ++i) {
        const WsRadialScreenMaskTag *t=&tags[(slot+i)&(WS_RADIAL_MASK_TAG_COUNT-1u)];
        if (!t->packet.used || t->packet.command_addr != key) continue;
        if (frame<t->packet.frame || frame-t->packet.frame>WS_HUD_ANCHOR_FRESH_FRAMES ||
            !ws_prepass_packet_matches(&t->packet.guard,words,count)) return 0;
        *scale=t->scale;
        return 1;
    }
    return 0;
}
static inline void ws_radial_mask_transform(int32_t x[4], int32_t y[4],
    int width, int height, float scale) {
    if (width<=0 || height<=0 || !isfinite(scale) || scale<1.f || scale>64.f) return;
    double cx=width*.5, cy=height*.5;
    for (unsigned i=0; i<4; ++i) {
        x[i]=(int32_t)llround(cx+(x[i]-cx)*scale);
        y[i]=(int32_t)llround(cy+(y[i]-cy)*scale);
    }
}
#endif
