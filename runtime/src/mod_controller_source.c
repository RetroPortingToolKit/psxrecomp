#include "mod_controller_source.h"
#include "sio.h"
#include <string.h>
static PSXModControllerSource s_source[PSX_MAX_PLAYERS];
static uint8_t s_release[PSX_MAX_PLAYERS];
int psx_mod_set_controller_source(uint32_t player, PSXModControllerSource source) {
    if (player >= PSX_MAX_PLAYERS) return 0;
    if(s_source[player] && !source)s_release[player]=1;
    s_source[player] = source; return 1;
}
void mod_controller_source_reset(void) { memset(s_source, 0, sizeof s_source); memset(s_release,0,sizeof s_release); }
int mod_controller_source_present(uint32_t player) {
    return player < PSX_MAX_PLAYERS && s_source[player] != NULL;
}
int mod_controller_source_sample(uint32_t player, PSXModControllerState *state) {
    if (!state || player>=PSX_MAX_PLAYERS || (!s_source[player] && !s_release[player])) return 0;
    PSXModControllerState neutral = {sizeof neutral, 0xffff, 128,128,128,128,1};
    *state = neutral;
    if(s_release[player]) { s_release[player]=0; return 1; }
    if (!s_source[player](state) || state->struct_size != sizeof *state ||
        state->buttons > 0xffff || state->lx > 255 || state->ly > 255 ||
        state->rx > 255 || state->ry > 255 || state->analog > 1) *state = neutral;
    if (!state->analog) state->lx = state->ly = state->rx = state->ry = 128;
    return 1;
}
