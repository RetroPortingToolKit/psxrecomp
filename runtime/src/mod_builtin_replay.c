#include "mod_plugins.h"
#include "replay_frontend.h"

static void replay_off(void) { replay_recording_request(0); }
static void replay_on(void) { replay_recording_request(1); }

PSX_MOD_CONSTRUCTOR(psx_register_builtin_replay_plugin) {
    (void)psx_mod_register_session_reset_plugin("psx.playable-replays", replay_off);
    (void)psx_mod_register_activation_plugin("psx.playable-replays", replay_on);
}
