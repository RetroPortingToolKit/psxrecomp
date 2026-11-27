#pragma once
#include <stddef.h>
#include <stdint.h>
#include <recomp_mod/sha256.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef rmod_sha256_state psx_sha256_ctx;

/* Compatibility names for the shared portable SHA-256 primitive. */
void psx_sha256_init(psx_sha256_ctx* ctx);
void psx_sha256_update(psx_sha256_ctx* ctx, const uint8_t* data, size_t len);
void psx_sha256_final(psx_sha256_ctx* ctx, uint8_t out[32]);
void psx_sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
