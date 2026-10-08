/* Framework implementation shared by title-owned HD texture pack manifests.
 * The owning feature declares directory resource "pack" and boolean options
 * "replacements" (default true) and "dump" (default false). */
#include "mod_plugins.h"

#include <string.h>

static int hd_texture_option(const char* id, int fallback) {
    char value[16] = "";
    if (!psx_mod_current_option_value(id, value, sizeof(value))) return fallback;
    return strcmp(value, "true") == 0;
}

static void builtin_hd_textures_activate(void) {
    (void)psx_mod_set_hd_texture_pack("pack", hd_texture_option("replacements", 1),
                                    hd_texture_option("dump", 0));
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_hd_textures_plugin) {
    (void)psx_mod_register_activation_plugin("psx.hd-textures",
                                             builtin_hd_textures_activate);
}
