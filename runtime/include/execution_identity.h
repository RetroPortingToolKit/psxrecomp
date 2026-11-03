#ifndef PSX_EXECUTION_IDENTITY_H
#define PSX_EXECUTION_IDENTITY_H
#include "execution_profile.h"
#include "psx_sha256.h"
#include <string.h>

/* Bind every network session, including a no-mod LAN session, to its fixed
 * execution implementation. Empty profile IDs retain legacy fixture behavior.
 * Validate before hashing so malformed content cannot become a valid digest. */
static inline int psx_execution_content_identity(const char* content, char out[65]) {
    const char* input = content ? content : "";
    char normalized[65];
    size_t n = strlen(input);
    if (n && n != 64) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
        normalized[i] = c;
    }
    normalized[n] = 0;
    input = normalized;
    if (!PSX_EXECUTION_ID[0]) {
        memcpy(out, input, n + 1);
        return 1;
    }
    psx_sha256_ctx ctx;
    uint8_t bytes[32];
    const char salt[] = "psx-session-execution-v1:";
    const char hex[] = "0123456789abcdef";
    psx_sha256_init(&ctx);
    psx_sha256_update(&ctx, (const uint8_t*)salt, sizeof(salt) - 1);
    psx_sha256_update(&ctx, (const uint8_t*)PSX_EXECUTION_ID, strlen(PSX_EXECUTION_ID));
    psx_sha256_update(&ctx, (const uint8_t*)input, n);
    psx_sha256_final(&ctx, bytes);
    for (unsigned i = 0; i < 32; ++i) {
        out[2*i] = hex[bytes[i] >> 4];
        out[2*i+1] = hex[bytes[i] & 15];
    }
    out[64] = 0;
    return 1;
}
#endif
