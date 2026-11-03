/*
 * Framework-owned Enhanced Audio mod, available to every game.
 *
 * The high-quality SPU mix (the float shadow, spu_shadow.h) re-renders the
 * output from the same guest voices, SPU RAM, envelopes and timing, so it is
 * a property of the host's audio output rather than of any disc; it ships
 * here and mods/builtin/packages/psx.enhancement.enhanced-audio targets
 * game_id "*". Default off: the title's [audio] spu_hq is the baseline, and
 * enabling the mod turns the high-quality mix on. PSX_AUDIO_SHADOW still
 * overrides both for a validation run.
 *
 * Activation only RECORDS the request; main.cpp applies it before spu_init()
 * at the renderer setup, after activation (host_enhancements.h).
 */
#include "mod_plugins.h"
#include "host_enhancements.h"

static void builtin_enhanced_audio_activate(void) {
    psx_host_enh_request_audio();
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_enhanced_audio_plugin) {
    (void)psx_mod_register_activation_plugin("psx.enhanced-audio",
                                             builtin_enhanced_audio_activate);
}
