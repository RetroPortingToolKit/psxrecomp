#include "psx_sha256.h"
void psx_sha256_init(psx_sha256_ctx *ctx) { rmod_sha256_init(ctx); }
void psx_sha256_update(psx_sha256_ctx *ctx, const uint8_t *data, size_t len) {
    rmod_sha256_update(ctx, data, len);
}
void psx_sha256_final(psx_sha256_ctx *ctx, uint8_t out[32]) { rmod_sha256_final(ctx, out); }
void psx_sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
    rmod_sha256_compute(data, len, out);
}
