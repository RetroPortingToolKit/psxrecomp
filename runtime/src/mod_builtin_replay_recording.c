/*
 * Framework-owned Record Replays mod, available to every game and enabled by
 * default. While it is enabled, each session records P1 from power-on into a
 * replay file (replay_session.h); the Mods page keeps the player's on/off
 * choice in mods/state.toml like every other feature.
 *
 * Activation only RECORDS the request; main.cpp reads it when the session
 * starts (host_enhancements.h). Netplay, a multitap or memory cards a replay
 * cannot hold still refuse the recording (replay_host_can_record).
 */
#include "mod_plugins.h"
#include "host_enhancements.h"

static void builtin_replay_recording_activate(void) {
    psx_host_enh_request_replay_recording();
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_replay_recording_plugin) {
    (void)psx_mod_register_activation_plugin("psx.replay-recording",
                                             builtin_replay_recording_activate);
}
