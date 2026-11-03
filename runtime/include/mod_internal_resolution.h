#ifndef PSX_MOD_INTERNAL_RESOLUTION_H
#define PSX_MOD_INTERNAL_RESOLUTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime session plumbing; plugins use mod_plugins.h. Zero means no request. */
uint32_t psx_mod_internal_resolution_request(void);
void psx_mod_internal_resolution_reset(void);

#ifdef __cplusplus
}
#endif
#endif
