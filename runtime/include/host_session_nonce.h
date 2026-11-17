#ifndef PSX_HOST_SESSION_NONCE_H
#define PSX_HOST_SESSION_NONCE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Fresh OS entropy once at setup; no weak/time/PRNG fallback. */
int psx_host_session_nonce(uint8_t out[16]);
#ifdef __cplusplus
}
#endif
#endif
