#ifndef PSX_NETPLAY_HOST_CATALOG_H
#define PSX_NETPLAY_HOST_CATALOG_H
#include "mod_netplay.h"
#ifdef __cplusplus
extern "C" {
#endif
/* A vanilla title can register qualification metadata without installing a
 * gameplay plugin. The callback lives in the existing netplay policy owner.
 * A custom simulation profile always owns its own catalog: never substitute
 * the vanilla policy for an unqualified mod profile. */
int psx_mod_register_vanilla_host_profiles(PSXModNetplayHostProfiles);
PSXModNetplayHostProfiles psx_mod_netplay_host_profiles(void);
#ifdef __cplusplus
}
#endif
#endif
