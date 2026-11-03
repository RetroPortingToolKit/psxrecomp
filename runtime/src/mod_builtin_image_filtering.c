/*
 * Framework-owned Image Filtering mod, available to every game.
 *
 * How the host scales and samples the picture -- the present-time smoothing
 * (linear vs nearest, the title's [video] antialiasing), texture filtering
 * (nearest / bilinear / xBR, [video] texture_filtering) and FMV
 * reconstruction ([video] fmv_filter) -- is a property of the host renderer,
 * not of any particular disc, so it ships here and mods/builtin/packages/
 * psx.enhancement.image-filtering targets game_id "*". Default off: the
 * title's [video] keys stay its baseline, and an option left at "Game
 * default" keeps that baseline value (host_enhancements.h). Smoothing is the
 * linear present scale, not the [video] antialiasing_mode FXAA pass.
 *
 * Activation only RECORDS the request (psx_host_enh_request_image); the
 * renderer setup in main.cpp resolves it over the baseline after activation
 * (the psx.enhancement.pgxp lesson, docs/ENHANCEMENTS.md G1.11). The in-game
 * menu's option changes apply live through the same request.
 */
#include "mod_plugins.h"
#include "host_enhancements.h"

#define PKG_IMAGE "psx.enhancement.image-filtering"

static void image_option(const char* id, char* out, unsigned size) {
    out[0] = '\0';
    (void)psx_mod_option_value(PKG_IMAGE, "image-filtering", id, out, size);
}

static void image_request(void) {
    char smoothing[16], texture[16], fmv[16];
    image_option("smoothing", smoothing, sizeof smoothing);
    image_option("texture_filtering", texture, sizeof texture);
    image_option("fmv_filtering", fmv, sizeof fmv);
    psx_host_enh_request_image(psx_host_enh_parse_smoothing(smoothing),
                               psx_host_enh_parse_texture_filter(texture),
                               psx_host_enh_parse_fmv_filter(fmv));
}

static void builtin_image_filtering_activate(void) {
    image_request();
}

static int builtin_image_filtering_option_changed(const char* option_id,
                                                  const char* value) {
    (void)option_id;
    (void)value;
    image_request();
    return psx_host_enh_apply_live();
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_image_filtering_plugin) {
    (void)psx_mod_register_activation_plugin("psx.image-filtering",
                                             builtin_image_filtering_activate);
    (void)psx_mod_register_option_changed_plugin(
        "psx.image-filtering", builtin_image_filtering_option_changed);
}
