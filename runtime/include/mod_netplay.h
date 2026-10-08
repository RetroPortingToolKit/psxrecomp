#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A title may force a statically linked simulation plugin and an optional
 * renderer companion for netplay.
 * This is executable-owned policy, never a package or persisted selection.
 * Bump compatibility_id whenever that simulation's wire/state contract changes.
 * Strings and the descriptor must live for the process lifetime. */
typedef struct PSXModNetplayProfile {
    const char *plugin_id;
    const char *compatibility_id;
    int rollback_supported;
    int savestates_supported;
    /* Fixed session boot policy: skip shell animation, keep kernel calls LLE.
     * This overrides local HLE/intro preferences so every peer boots alike. */
    int skip_bios_intro;
    /* Fixed netplay views: bit 0=4:3, bit 1=16:9, bit 2=21:9. No adaptive. */
    unsigned fixed_aspect_mask;
    /* Optional executable-owned renderer plugin, forced for wide sessions. */
    const char *widescreen_plugin_id;
    /* Optional executable-owned loading feature, selected by the lobby host. */
    const char *loading_package_id;
    const char *loading_feature_id;
    const char *loading_plugin_id;
} PSXModNetplayProfile;

int psx_mod_register_netplay_profile(const PSXModNetplayProfile *profile);
const PSXModNetplayProfile *psx_mod_netplay_profile(void);
const char *psx_mod_netplay_version(const char *vanilla_version);
int psx_mod_netplay_rollback_supported(void);
/* Active session policy also covers synchronized state transfers. */
int psx_mod_netplay_savestates_supported(void);
void psx_mod_netplay_set_active(int active);
int psx_mod_netplay_is_active(void);
uint32_t psx_mod_netplay_session_id(uint32_t session_id);
int psx_mod_netplay_set_aspect(int index);
int psx_mod_netplay_aspect(void);
int psx_mod_netplay_loading_feature(const char *package_id, const char *feature_id);
void psx_mod_netplay_set_loading(int enabled);
int psx_mod_netplay_loading(void);

#ifdef __cplusplus
}
#endif
