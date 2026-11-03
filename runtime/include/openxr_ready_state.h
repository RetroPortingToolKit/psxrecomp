#pragma once

#include <stdint.h>

typedef struct PSXOpenXRReadyLoadRequest {
    uint64_t sequence;
    uint64_t execution_generation;
    uint32_t status_generation_before;
    int slot;
    const char *error;
} PSXOpenXRReadyLoadRequest;

typedef struct PSXOpenXRReadyLoadStatus {
    uint32_t generation;
    int pending;
    int last_ok;
    int last_load;
    int last_slot;
} PSXOpenXRReadyLoadStatus;

typedef struct PSXOpenXRReadyLoadState {
    uint64_t execution_generation;
    uint64_t request_sequence;
    uint32_t generation_floor;
    int expected;
    int request_seen;
    int resolved;
    int last_ok;
    int slot;
    const char *error;
} PSXOpenXRReadyLoadState;

static inline void psx_openxr_ready_load_begin_execution(
    PSXOpenXRReadyLoadState *state, uint64_t execution_generation,
    int expected, int slot) {
    if (!state) return;
    state->execution_generation = execution_generation;
    state->request_sequence = 0;
    state->generation_floor = 0;
    state->expected = !!expected;
    state->request_seen = 0;
    state->resolved = !state->expected;
    state->last_ok = 0;
    state->slot = slot;
    state->error = 0;
}

/* Returns nonzero when this is a new request for the current execution. */
static inline int psx_openxr_ready_load_observe_request(
    PSXOpenXRReadyLoadState *state,
    const PSXOpenXRReadyLoadRequest *request) {
    if (!state || !request || !state->expected ||
        request->execution_generation != state->execution_generation ||
        request->sequence == 0 || request->sequence == state->request_sequence)
        return 0;
    state->request_sequence = request->sequence;
    state->request_seen = 1;
    state->generation_floor = request->status_generation_before;
    state->slot = request->slot;
    state->error = request->error;
    state->resolved = request->error != 0;
    state->last_ok = 0;
    return 1;
}

/* Only a settled load after the request-generation floor can resolve success. */
static inline int psx_openxr_ready_load_observe_status(
    PSXOpenXRReadyLoadState *state,
    const PSXOpenXRReadyLoadStatus *status) {
    if (!state || !status || !state->expected || !state->request_seen ||
        state->resolved ||
        status->pending || status->generation <= state->generation_floor ||
        !status->last_load || status->last_slot != state->slot)
        return 0;
    state->resolved = 1;
    state->last_ok = status->last_ok;
    return 1;
}

static inline int psx_openxr_ready_load_ok(
    const PSXOpenXRReadyLoadState *state) {
    return state && (!state->expected || (state->resolved && state->last_ok));
}

static inline int psx_openxr_ready_load_failed(
    const PSXOpenXRReadyLoadState *state) {
    return state && state->expected && state->resolved && !state->last_ok;
}
