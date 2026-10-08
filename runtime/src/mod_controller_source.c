#include "mod_controller_source.h"
#include "sio.h"
#include <stdio.h>
#include <string.h>
static PSXModControllerSource s_source[PSX_MAX_PLAYERS];
static uint8_t s_release[PSX_MAX_PLAYERS];
/* One sample per player per frame: the early and the low-latency re-sample in
 * main.cpp share the result so a stateful callback runs once. */
static PSXModControllerState s_cache[PSX_MAX_PLAYERS];
static uint8_t s_cache_valid[PSX_MAX_PLAYERS];
static uint8_t s_cache_released[PSX_MAX_PLAYERS];
static uint32_t s_bad_count[PSX_MAX_PLAYERS];
#define BAD_LOG_INTERVAL 600u
int psx_mod_set_controller_source(uint32_t player, PSXModControllerSource source) {
    if (player >= PSX_MAX_PLAYERS) return 0;
    if (s_source[player] && !source) s_release[player] = 1;
    else if (source) s_release[player] = 0;
    s_source[player] = source;
    s_cache_valid[player] = 0;
    s_bad_count[player] = 0;
    return 1;
}
/* Detach every source but keep a neutral release pending for each port that
 * had one, so held buttons/sticks are cleared even on a port with no device. */
void mod_controller_source_reset(void) {
    for (uint32_t i = 0; i < PSX_MAX_PLAYERS; i++) {
        if (s_source[i]) s_release[i] = 1;
        s_source[i] = NULL;
    }
    memset(s_cache_valid, 0, sizeof s_cache_valid);
    memset(s_bad_count, 0, sizeof s_bad_count);
}
void mod_controller_source_begin_frame(void) {
    memset(s_cache_valid, 0, sizeof s_cache_valid);
}
int mod_controller_source_present(uint32_t player) {
    return player < PSX_MAX_PLAYERS && s_source[player] != NULL;
}
static void note_bad(uint32_t player, const PSXModControllerState *s) {
    if (s_bad_count[player]++ % BAD_LOG_INTERVAL) return;
    fprintf(stderr,
        "psxrecomp: mod controller source (player %u) returned an invalid "
        "state (struct_size %u, expected %u; buttons 0x%x lx %u ly %u rx %u "
        "ry %u analog %u); delivering neutral (%u so far)\n",
        (unsigned)player, (unsigned)s->struct_size,
        (unsigned)sizeof *s, (unsigned)s->buttons, (unsigned)s->lx,
        (unsigned)s->ly, (unsigned)s->rx, (unsigned)s->ry,
        (unsigned)s->analog, (unsigned)s_bad_count[player]);
}
int mod_controller_source_sample(uint32_t player, PSXModControllerState *state,
                                 int *released) {
    if (released) *released = 0;
    if (!state || player>=PSX_MAX_PLAYERS || (!s_source[player] && !s_release[player] && !s_cache_valid[player])) return 0;
    if (s_cache_valid[player]) {
        *state = s_cache[player];
        if (released) *released = s_cache_released[player];
        return 1;
    }
    PSXModControllerState neutral = {sizeof neutral, 0xffff, 128,128,128,128,1};
    int rel = 0;
    *state = neutral;
    if (s_release[player]) { s_release[player] = 0; rel = 1; }
    else {
        if (!s_source[player](state)) *state = neutral;  /* declined: neutral */
        else if (state->struct_size != sizeof *state ||
                 state->buttons > 0xffff || state->lx > 255 || state->ly > 255 ||
                 state->rx > 255 || state->ry > 255 || state->analog > 1) {
            note_bad(player, state);
            *state = neutral;
        }
        if (!state->analog) state->lx = state->ly = state->rx = state->ry = 128;
    }
    s_cache[player] = *state;
    s_cache_released[player] = (uint8_t)rel;
    s_cache_valid[player] = 1;
    if (released) *released = rel;
    return 1;
}
