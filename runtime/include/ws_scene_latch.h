#ifndef PSX_WS_SCENE_LATCH_H
#define PSX_WS_SCENE_LATCH_H
#include <stdint.h>

/* Overhang proves there is revealable world content. Projection activity can
 * maintain that proof, but MUST NOT establish it: 2D room jingles also use GTE.
 * The CPU/GTE can build a frame across multiple vblanks before submitting any
 * terrain packets, so gaps in GP0 overhang are not evidence of a scene change. */
typedef struct {
    uint32_t last_frame, last_activity;
    int initialized, confirmed;
} WsSceneLatch;

static inline int ws_scene_is_2d(WsSceneLatch* state, uint32_t frame,
                                int confirmed_overhang, int projecting_world,
                                uint32_t grace) {
    if (state->initialized && (int32_t)(frame - state->last_frame) < 0)
        state->confirmed = 0;
    state->initialized = 1;
    state->last_frame = frame;
    if (confirmed_overhang) state->confirmed = 1;
    if (confirmed_overhang || (state->confirmed && projecting_world))
        state->last_activity = frame;
    if (state->confirmed && frame - state->last_activity > grace)
        state->confirmed = 0;
    return !state->confirmed;
}

/* A short backdrop is not proof of a flat room when a sustained, world-scale
 * mesh is projecting beyond the canonical viewport. Keep the two witnesses
 * independent: sparse billboard props still cannot override finite artwork.
 * 512 is above the observed event-title projection bursts (up to 424), while
 * the mixed terrain scenes submit thousands. Retained UI remains authoritative.
 */
#define WS_WORLD_SCALE_MIN_VERTS 512u
typedef struct { uint32_t last_frame, sustained_frame; } WsWorldScaleWitness;
static inline void ws_world_scale_reset(WsWorldScaleWitness* s, uint32_t f) {
    s->last_frame = s->sustained_frame = f - 1000u;
}
static inline void ws_world_scale_note(WsWorldScaleWitness* s, uint32_t f,
                                       uint32_t projected_vertices) {
    if (projected_vertices < WS_WORLD_SCALE_MIN_VERTS || s->last_frame == f) return;
    if (s->last_frame + 1u == f) s->sustained_frame = f;
    s->last_frame = f;
}
static inline int ws_backdrop_keeps_native_43(int short_backdrop,
        const WsWorldScaleWitness* s, uint32_t f, int natural_overhang) {
    return short_backdrop && !(natural_overhang && f - s->sustained_frame <= 2u);
}

#endif
