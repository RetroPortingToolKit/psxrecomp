/* Host-only post effects. Activation records choices; renderer setup applies them. */
#include "mod_plugins.h"
#include "host_enhancements.h"
#include <stdlib.h>

#define PKG_POST "psx.enhancement.post-processing"

static void option(const char* id, char* value, unsigned size) {
    value[0] = '\0';
    (void)psx_mod_option_value(PKG_POST, "post-processing", id, value, size);
}

static void post_request(void) {
    char value[32], shader[4096] = "";
    option("antialiasing", value, sizeof value);
    const int aa = psx_host_enh_parse_post_aa(value);
    option("bloom", value, sizeof value);
    const int bloom = psx_host_enh_parse_tristate(value);
    option("bloom_strength", value, sizeof value);
    int strength = atoi(value);
    if (strength < 1 || strength > 200) strength = 25;
    option("shader", value, sizeof value);
    const int mode = psx_host_enh_parse_tristate(value);
    if (mode == 1) (void)psx_mod_current_resource_path("shader", shader, sizeof shader);
    psx_host_enh_request_post(aa, bloom, strength, mode, shader);
}

static int post_changed(const char* id, const char* value) {
    (void)id; (void)value;
    post_request();
    return psx_host_enh_apply_live();
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_post_processing_plugin) {
    (void)psx_mod_register_activation_plugin("psx.post-processing", post_request);
    (void)psx_mod_register_option_changed_plugin("psx.post-processing", post_changed);
}
