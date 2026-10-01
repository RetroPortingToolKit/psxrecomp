#include "mod_netplay.h"
#include <stddef.h>
#include <string.h>

static const PSXModNetplayProfile *title_profile;
static int profile_active;
static int profile_aspect;

int psx_mod_register_netplay_profile(const PSXModNetplayProfile *profile) {
    if (!profile || !profile->plugin_id || !profile->plugin_id[0] ||
        !profile->compatibility_id || !profile->compatibility_id[0] ||
        strlen(profile->compatibility_id) >= 64) return 0;
    if (title_profile && title_profile != profile) return 0;
    title_profile = profile;
    return 1;
}
const PSXModNetplayProfile *psx_mod_netplay_profile(void) { return title_profile; }
const char *psx_mod_netplay_version(const char *vanilla_version) {
    return title_profile ? title_profile->compatibility_id : vanilla_version;
}
int psx_mod_netplay_rollback_supported(void) {
    return !title_profile || title_profile->rollback_supported;
}
int psx_mod_netplay_savestates_supported(void) {
    return !profile_active || !title_profile || title_profile->savestates_supported;
}
void psx_mod_netplay_set_active(int active) { profile_active = active != 0; }
int psx_mod_netplay_is_active(void) { return profile_active; }
uint32_t psx_mod_netplay_session_id(uint32_t session_id) {
    uint32_t hash = 2166136261u;
    const unsigned char *p;
    if (!title_profile) return session_id;
    p = (const unsigned char *)title_profile->compatibility_id;
    while (*p) { hash ^= *p++; hash *= 16777619u; }
    /* Direct peers with different simulation view widths cannot start
     * together. Lobby launch sets the host's choice before transport boot. */
    if (title_profile->fixed_aspect_mask) {
        hash ^= (uint32_t)profile_aspect;
        hash *= 16777619u;
    }
    return session_id ^ hash;
}
int psx_mod_netplay_set_aspect(int index) {
    if (!title_profile || index < 0 || index > 2 ||
        !(title_profile->fixed_aspect_mask & (1u << index))) return 0;
    profile_aspect = index;
    return 1;
}
int psx_mod_netplay_aspect(void) { return profile_aspect; }
