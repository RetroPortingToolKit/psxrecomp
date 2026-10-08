#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Executable-owned netplay policy for a trusted simulation plugin. It never
 * imports the player's offline package selection. The descriptor and strings
 * must live for the process lifetime. Change compatibility_id whenever the
 * plugin's shared simulation/state contract changes (maximum 63 bytes).
 *
 * state_digest is optional, read-only and emulation-thread only. Hash a
 * canonical encoding of deterministic host-owned gameplay state; exclude
 * pointers, struct padding, rendering caches and local-only input state. It
 * joins the normal netplay core digest while this profile's plan is active. */
typedef uint32_t (*PSXModNetplayStateDigest)(void);
typedef struct PSXModNetplayProfile {
    const char *plugin_id;
    const char *compatibility_id;
    int rollback_supported;
    int savestates_supported;
    int skip_bios_intro; /* Fixed boot policy: skip animation, kernel calls LLE. */
    unsigned fixed_aspect_mask; /* bit 0=4:3, 1=16:9, 2=21:9; zero=no policy */
    const char *widescreen_plugin_id; /* Optional forced renderer for wide. */
    PSXModNetplayStateDigest state_digest;
} PSXModNetplayProfile;

int psx_mod_register_netplay_profile(const PSXModNetplayProfile *profile);
const PSXModNetplayProfile *psx_mod_netplay_profile(void);
const char *psx_mod_netplay_version(const char *vanilla_version);
int psx_mod_netplay_rollback_supported(void);
int psx_mod_netplay_savestates_supported(void);
void psx_mod_netplay_set_active(int active);
int psx_mod_netplay_is_active(void);
uint32_t psx_mod_netplay_session_id(uint32_t session_id);
/* Extend the modern execution/content handshake with a canonical SHA-256
 * profile identity. This rejects mismatches even if the 32-bit transport
 * namespace happens to collide. Input must be empty or a 64-digit hex SHA. */
int psx_mod_netplay_content_identity(const char *execution_content, char out[65]);
int psx_mod_netplay_set_aspect(int index);
int psx_mod_netplay_aspect(void);
int psx_mod_netplay_has_state_digest(void);
uint32_t psx_mod_netplay_state_digest(void);

#ifdef __cplusplus
}
#endif
