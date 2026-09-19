#pragma once
#include <stdint.h>

/* Host presentation history, not guest hardware state. Never serialize it:
 * a restored canonical framebuffer cannot reconstruct discarded wide strips. */
typedef struct WsSceneHold {
    int valid, native_43, awaiting_flip;
    uint32_t held_origin;
} WsSceneHold;
static inline void ws_scene_hold_reset(WsSceneHold* state) {
    state->valid = 0; state->native_43 = 1; state->awaiting_flip = 0;
    state->held_origin = 0;
}
static inline int ws_scene_hold_classify(WsSceneHold* state, int hold,
                                        int native_43, int fmv,
                                        uint32_t display_origin) {
    /* FMV always wins, and cannot seed a later wide retained scene. */
    if (fmv) {
        state->valid = 1; state->native_43 = 1;
        state->awaiting_flip = 0;
        return 1;
    }
    if (hold == 1) {
        state->held_origin = display_origin;
        state->awaiting_flip = 1;
    } else if (hold != 1) {
        /* 2: the backbuffer is ready, but not necessarily on screen yet. */
        hold = hold == 2 && state->awaiting_flip &&
               display_origin == state->held_origin;
        if (!hold) state->awaiting_flip = 0;
    }
    if (hold && state->valid) return state->native_43;
    state->valid = 1; state->native_43 = native_43 != 0;
    return state->native_43;
}
