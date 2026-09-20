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
#endif
