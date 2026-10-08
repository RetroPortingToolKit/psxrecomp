#include "mod_netplay.h"
#include "psx_sha256.h"
#include <stddef.h>
#include <string.h>

static const PSXModNetplayProfile *title_profile;
static int profile_active;
static int profile_aspect;

int psx_mod_register_netplay_profile(const PSXModNetplayProfile *profile) {
    unsigned mask;
    if (!profile || !profile->plugin_id || !profile->plugin_id[0] ||
        !profile->compatibility_id || !profile->compatibility_id[0] ||
        strlen(profile->compatibility_id) >= 64 ||
        (profile->fixed_aspect_mask & ~7u) ||
        (profile->widescreen_plugin_id && !profile->widescreen_plugin_id[0]))
        return 0;
    if (title_profile) return title_profile == profile;
    title_profile = profile;
    mask = profile->fixed_aspect_mask;
    /* Native is the default whenever supported; otherwise first allowed view. */
    profile_aspect = (mask && !(mask & 1u)) ? ((mask & 2u) ? 1 : 2) : 0;
    return 1;
}
const PSXModNetplayProfile *psx_mod_netplay_profile(void) { return title_profile; }
const char *psx_mod_netplay_version(const char *vanilla_version) {
    return title_profile ? title_profile->compatibility_id : vanilla_version;
}
int psx_mod_netplay_rollback_supported(void) {
    return !title_profile || title_profile->rollback_supported != 0;
}
int psx_mod_netplay_savestates_supported(void) {
    return !profile_active || !title_profile || title_profile->savestates_supported != 0;
}
void psx_mod_netplay_set_active(int active) {
    profile_active = active && title_profile;
}
int psx_mod_netplay_is_active(void) { return profile_active; }
uint32_t psx_mod_netplay_session_id(uint32_t session_id) {
    uint32_t hash = 2166136261u;
    const unsigned char *p;
    if (!title_profile) return session_id;
    p = (const unsigned char *)title_profile->compatibility_id;
    while (*p) { hash ^= *p++; hash *= 16777619u; }
    if (title_profile->fixed_aspect_mask) {
        hash ^= (uint32_t)profile_aspect;
        hash *= 16777619u;
    }
    return session_id ^ hash;
}
int psx_mod_netplay_content_identity(const char *execution_content, char out[65]) {
    const char *input = execution_content ? execution_content : "";
    char normalized[65];
    size_t n = strlen(input), i;
    psx_sha256_ctx ctx;
    uint8_t bytes[32], policy[5];
    static const char domain[] = "psx-trusted-netplay-profile-v1";
    static const char hex[] = "0123456789abcdef";
    const char *renderer;
    if (!out || (n != 0 && n != 64)) return 0;
    for (i = 0; i < n; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
        normalized[i] = c;
    }
    normalized[n] = 0;
    if (!title_profile) { memcpy(out, normalized, n + 1); return 1; }
    renderer = profile_aspect && title_profile->widescreen_plugin_id
        ? title_profile->widescreen_plugin_id : "";
    /* Explicit fields and NUL-delimited strings, never ABI-dependent struct
     * bytes. Include runtime policy to catch incorrect reuse of a revision. */
    policy[0] = (uint8_t)title_profile->fixed_aspect_mask;
    policy[1] = policy[0] ? (uint8_t)profile_aspect : 0u;
    policy[2] = title_profile->rollback_supported != 0;
    policy[3] = title_profile->savestates_supported != 0;
    policy[4] = title_profile->skip_bios_intro != 0;
    psx_sha256_init(&ctx);
    psx_sha256_update(&ctx, (const uint8_t*)domain, sizeof(domain));
    psx_sha256_update(&ctx, (const uint8_t*)normalized, n + 1);
    psx_sha256_update(&ctx, (const uint8_t*)title_profile->plugin_id,
                       strlen(title_profile->plugin_id) + 1);
    psx_sha256_update(&ctx, (const uint8_t*)title_profile->compatibility_id,
                       strlen(title_profile->compatibility_id) + 1);
    psx_sha256_update(&ctx, policy, sizeof(policy));
    psx_sha256_update(&ctx, (const uint8_t*)renderer, strlen(renderer) + 1);
    psx_sha256_final(&ctx, bytes);
    for (i = 0; i < 32; ++i) {
        out[2*i] = hex[bytes[i] >> 4];
        out[2*i + 1] = hex[bytes[i] & 15];
    }
    out[64] = 0;
    return 1;
}
int psx_mod_netplay_set_aspect(int index) {
    if (!title_profile || index < 0 || index > 2 ||
        !(title_profile->fixed_aspect_mask & (1u << index))) return 0;
    profile_aspect = index;
    return 1;
}
int psx_mod_netplay_aspect(void) { return profile_aspect; }
int psx_mod_netplay_has_state_digest(void) {
    return profile_active && title_profile && title_profile->state_digest;
}
uint32_t psx_mod_netplay_state_digest(void) {
    return psx_mod_netplay_has_state_digest() ? title_profile->state_digest() : 0u;
}
