/*
 * Framework-owned Rewind mod, available to every game.
 *
 * Local rewind keeps whole-machine snapshots (2 MB RAM + 1 MB VRAM + 512 KB
 * SPU RAM each) on a frame cadence, so it is opt-in: enabling
 * mods/builtin/packages/psx.feature.rewind turns the ring on with the chosen
 * buffer depth and capture interval. It is a host tool, not an enhancement of
 * the game's output, hence the psx.feature.* id. The Rewind hotkey stays in
 * the launcher's Hotkeys page; a title can still refuse it while a mode it
 * cannot rewind is live (psx_mod_set_rewind_blocked), and netplay keeps it
 * off (psx_rewind.h).
 *
 * Activation only RECORDS the request; main.cpp arms the ring from it when
 * the session's machine is configured (host_enhancements.h). PSX_REWIND,
 * PSX_REWIND_DEPTH and PSX_REWIND_INTERVAL still outrank it. A build with
 * -DPSX_REWIND=OFF stages no psx.feature.rewind package at all
 * (runtime.cmake), so the feature is absent rather than inert.
 */
#include "mod_plugins.h"
#include "host_enhancements.h"

#define PKG_REWIND "psx.feature.rewind"

static void builtin_rewind_activate(void) {
    char depth[16] = "", interval[16] = "";
    (void)psx_mod_option_value(PKG_REWIND, "rewind", "buffer", depth, sizeof depth);
    (void)psx_mod_option_value(PKG_REWIND, "rewind", "interval", interval,
                               sizeof interval);
    psx_host_enh_request_rewind(psx_host_enh_parse_rewind_depth(depth),
                                psx_host_enh_parse_rewind_interval(interval));
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_rewind_plugin) {
    (void)psx_mod_register_activation_plugin("psx.rewind",
                                             builtin_rewind_activate);
}
