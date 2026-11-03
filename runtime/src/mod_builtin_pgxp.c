/*
 * Framework-owned PGXP precision mod, available to every game.
 *
 * Sub-pixel vertex precision + perspective-correct texturing is a property of
 * the emulated GTE/GPU pair, not of any particular disc, so it ships here and
 * mods/builtin/packages/psx.enhancement.pgxp targets game_id "*". Default off
 * (the faithful floor, docs/ENHANCEMENTS.md G1.9); enabling arms geometry and
 * texture correction on the value-propagation engine (pgxp.cpp), cpu-mode and
 * precise culling (G1.12) per the mod options. The title's other [video] PGXP
 * keys still apply. Unspecified tuning is dataflow-only/no-clamp in hook
 * builds, or position-cache/0.5px in base builds (pgxp_session.cpp).
 *
 * Activation only RECORDS the request (pgxp_mod_request). It runs before the
 * renderer setup in main.cpp, whose session arming (pgxp_session.cpp) takes
 * the request and arms the engine from it and the [video] baseline. Arming
 * here directly used to be undone by that baseline, so the mod did nothing
 * (docs/ENHANCEMENTS.md G1.11).
 *
 * Coverage note: the engine reaches near-total dataflow coverage on a binary
 * compiled with the PGXP hook variant (-DPSX_PGXP=1, PSX_PGXP_VARIANT); on a
 * base-flavour binary only the always-emitted swc2 tier and the interpreters
 * feed it, so the correction is weaker there but never harmful — every vertex
 * is validated against the exact packet word before it is believed.
 */
#include "mod_plugins.h"
#include "pgxp.h"

#include <string.h>

#define PKG_PGXP "psx.enhancement.pgxp"

static int pgxp_option_flag(const char* feature, const char* id) {
    char text[16] = "";
    return psx_mod_option_value(PKG_PGXP, feature, id, text, sizeof text) &&
           strcmp(text, "true") == 0;
}

static void builtin_pgxp_activate(void) {
    pgxp_mod_request(1, pgxp_option_flag("pgxp", "cpu_mode"),
                     pgxp_option_flag("pgxp", "culling"));
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_pgxp_plugin) {
    (void)psx_mod_register_activation_plugin("psx.pgxp",
                                             builtin_pgxp_activate);
}
