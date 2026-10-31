/*
 * Framework-owned Internal Resolution mod, available to every game.
 *
 * The host's 3D render scale -- fixed 1x..32x, a legacy height preset,
 * Match display, or Auto (Match display with dynamic resolution) --
 * is a property of the host renderer, not of any particular disc, so it ships
 * here and mods/builtin/packages/psx.enhancement.internal-resolution targets
 * game_id "*". Fresh profiles default to fixed 4x, independent of the title's
 * old height preset. Saved scales/presets and Game default remain explicit
 * choices. Disabling the feature selects Native (host_enhancements.h).
 *
 * Activation only RECORDS the request (psx_host_enh_request_resolution); the
 * renderer setup in main.cpp resolves it over the baseline after activation
 * (the psx.enhancement.pgxp lesson, docs/ENHANCEMENTS.md G1.11). The in-game
 * menu's option changes apply live through main.cpp's resolution applier when
 * the backend can change its scale on a running game; otherwise they apply at
 * the next start.
 */
#include "mod_plugins.h"
#include "host_enhancements.h"

#define PKG_RESOLUTION "psx.enhancement.internal-resolution"

static void resolution_request(void) {
    char value[16];
    value[0] = '\0';
    (void)psx_mod_option_value(PKG_RESOLUTION, "internal-resolution", "resolution",
                               value, sizeof value);
    psx_host_enh_request_resolution(psx_host_enh_parse_internal_resolution(value));
}

static void builtin_internal_resolution_activate(void) {
    resolution_request();
}

static int builtin_internal_resolution_option_changed(const char* option_id,
                                                      const char* value) {
    (void)option_id;
    (void)value;
    resolution_request();
    return psx_host_enh_apply_resolution_live();
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_internal_resolution_plugin) {
    (void)psx_mod_register_activation_plugin("psx.internal-resolution",
                                             builtin_internal_resolution_activate);
    (void)psx_mod_register_option_changed_plugin(
        "psx.internal-resolution", builtin_internal_resolution_option_changed);
}
