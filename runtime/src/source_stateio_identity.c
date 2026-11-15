/* source_stateio_identity.c: checkpoint identity dimensions.
 *
 * Its own translation unit so it can include <windows.h> for the executable
 * path without bringing it into headers shared with main.cpp. */
#include "source_stateio_identity.h"
#include "source_tas_stateio.h"   /* env allow-list and collection */
#include "psx_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <stdlib.h>       /* _environ */
#  define PSX_ENVIRON ((const char *const *)_environ)
#else
#  include <unistd.h>
extern char **environ;
#  define PSX_ENVIRON ((const char *const *)environ)
#endif

static void sha256_hex(const uint8_t dig[32], char out[65]) {
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", (unsigned)dig[i]);
    out[64] = '\0';
}

int source_stateio_file_sha256(const char *path, char out[65]) {
    FILE *f;
    psx_sha256_ctx ctx;
    uint8_t dig[32], buf[65536];
    size_t n;
    if (!out) return 0;
    out[0] = '\0';
    if (!path || !*path) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    psx_sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) psx_sha256_update(&ctx, buf, n);
    if (ferror(f)) { fclose(f); return 0; }
    fclose(f);
    psx_sha256_final(&ctx, dig);
    sha256_hex(dig, out);
    return 1;
}

/* The running executable and the route file cannot change during a run, and
 * hashing a large title executable at every capture is slow enough to stall a
 * replay. Each digest is computed once per process. */
int source_stateio_exe_sha256(char out[65]) {
    static char cached[65];
    char path[1024];
    if (!out) return 0;
    if (cached[0]) { memcpy(out, cached, 65); return 1; }
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path) { out[0] = '\0'; return 0; }
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0 || (size_t)n >= sizeof path) { out[0] = '\0'; return 0; }
    path[n] = '\0';
#endif
    if (!source_stateio_file_sha256(path, out)) return 0;
    memcpy(cached, out, 65);
    return 1;
}

int source_stateio_route_sha256(char out[65]) {
    static char cached[65];
    if (!out) return 0;
    if (cached[0]) { memcpy(out, cached, 65); return 1; }
    if (!source_stateio_file_sha256(getenv("PSX_INPUT_ROUTE_FILE"), out)) return 0;
    memcpy(cached, out, 65);
    return 1;
}

/* Sorted so enumeration order cannot change the result; a stream hash rather
 * than an XOR so two distinct configurations cannot cancel each other out. */
int source_stateio_config_digest_hex(char out[65]) {
    const char *entries[512];
    size_t n;
    psx_sha256_ctx ctx;
    uint8_t dig[32];
    if (!out) return 0;
    out[0] = '\0';
    n = source_tas_stateio_env_collect(PSX_ENVIRON, entries, 512);
    psx_sha256_init(&ctx);
    for (size_t i = 0; i < n; i++) {
        psx_sha256_update(&ctx, (const uint8_t *)entries[i], strlen(entries[i]));
        psx_sha256_update(&ctx, (const uint8_t *)"\n", 1);
    }
    psx_sha256_final(&ctx, dig);
    sha256_hex(dig, out);
    return 1;
}

int source_stateio_config_digest_accepts(const char *stored) {
    char now[65];
    return stored && source_stateio_config_digest_hex(now) && !strcmp(stored, now);
}
