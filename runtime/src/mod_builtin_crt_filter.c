/*
 * Framework-owned CRT Filter mod, available to every game.
 *
 * The present-time screen-colour model (Composite / CRT / Trinitron) and the
 * scanline post-process are properties of the host's presentation, not of any
 * particular disc, so they ship here and mods/builtin/packages/
 * psx.enhancement.crt-filter targets game_id "*". Default off: the title's
 * [video] screen_kind / scanlines / scanline_strength stay its baseline, and
 * an option left at "Game default" keeps that baseline value
 * (host_enhancements.h).
 *
 * Activation only RECORDS the request (psx_host_enh_request_crt). The renderer
 * setup in main.cpp resolves it over the [video] baseline after activation;
 * applying it here would be undone by that baseline, the lesson of
 * psx.enhancement.pgxp (docs/ENHANCEMENTS.md G1.11).
 *
 * The in-game menu's option changes apply live (psx_mod_set_option_live):
 * the callback records the new request and asks main.cpp to apply it.
 */
#include "mod_plugins.h"
#include "host_enhancements.h"

#define PKG_CRT "psx.enhancement.crt-filter"

static void crt_option(const char* id, char* out, unsigned size) {
    out[0] = '\0';
    (void)psx_mod_option_value(PKG_CRT, "crt-filter", id, out, size);
}

static void crt_request(void) {
    char model[32], scanlines[16], strength[16];
    crt_option("screen_model", model, sizeof model);
    crt_option("scanlines", scanlines, sizeof scanlines);
    crt_option("scanline_strength", strength, sizeof strength);
    psx_host_enh_request_crt(psx_host_enh_parse_screen_kind(model),
                             psx_host_enh_parse_tristate(scanlines),
                             psx_host_enh_parse_strength(strength));
}

static void builtin_crt_filter_activate(void) {
    crt_request();
}

static int builtin_crt_filter_option_changed(const char* option_id,
                                             const char* value) {
    (void)option_id;
    (void)value;
    /* psx_mod_option_value already answers the new value here. */
    crt_request();
    return psx_host_enh_apply_live();
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_crt_filter_plugin) {
    (void)psx_mod_register_activation_plugin("psx.crt-filter",
                                             builtin_crt_filter_activate);
    (void)psx_mod_register_option_changed_plugin(
        "psx.crt-filter", builtin_crt_filter_option_changed);
}
