/* A staged overlay toolchain from a different emitter than the runtime writes shards under its
 * own codegen hash (a cache directory the runtime never reads). autocompile_check_toolchain_hash
 * must refuse it loudly (degraded_reason), accept a matching one, and stay quiet when the header
 * is absent or unparseable. Build/run: ctest -R autocompile_toolchain_hash_test */
#include "autocompile.h"
#include "overlay_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct OverlayPreparedImage { unsigned id; };
OverlayPreparedImage *overlay_loader_prepare_published(const char *p) { (void)p; return NULL; }
int  overlay_loader_commit_published(OverlayPreparedImage *i) { (void)i; return 0; }
void overlay_loader_discard_prepared(OverlayPreparedImage *i) { (void)i; }
void overlay_loader_rescan(void) {}

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void write_header(const char *dir, const char *body) {
    char path[600];
    snprintf(path, sizeof path, "%s/overlay_codegen_hash.h", dir);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(body, f); fclose(f); }
}

int main(void) {
    char dir[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    strcat(dir, "psx_ac_tkhash_test");
    CreateDirectoryA(dir, NULL);

    /* No header: unknown -> usable, nothing recorded. */
    char missing[MAX_PATH + 16];
    snprintf(missing, sizeof missing, "%s/nope", dir);
    CHECK(autocompile_check_toolchain_hash(missing, 0xBA11DD86u) == 1);
    CHECK(autocompile_degraded_reason() == NULL);

    /* Unparseable header and runtime hash 0 (unknown build): usable. */
    write_header(dir, "// nothing here\n");
    CHECK(autocompile_check_toolchain_hash(dir, 0xBA11DD86u) == 1);
    write_header(dir, "#define PSX_OVERLAY_CODEGEN_HASH 0x0bea7895u\n");
    CHECK(autocompile_check_toolchain_hash(dir, 0u) == 1);
    CHECK(autocompile_degraded_reason() == NULL);

    /* Matching hash: usable. */
    write_header(dir, "#pragma once\n#define PSX_OVERLAY_CODEGEN_HASH 0xba11dd86u\n");
    CHECK(autocompile_check_toolchain_hash(dir, 0xBA11DD86u) == 1);
    CHECK(autocompile_degraded_reason() == NULL);

    /* The 2026-10-12 playtest case: toolchain from an older emitter. */
    write_header(dir, "#pragma once\n#define PSX_OVERLAY_CODEGEN_HASH 0x0bea7895u\n");
    CHECK(autocompile_check_toolchain_hash(dir, 0xBA11DD86u) == 0);
    const char *why = autocompile_degraded_reason();
    CHECK(why != NULL);
    CHECK(why && strstr(why, "0x0bea7895") && strstr(why, "0xba11dd86") && strstr(why, "stale overlay toolchain"));
    char json[4096];
    autocompile_status_json(json, (int)sizeof json);
    CHECK(strstr(json, "\"degraded\":1") != NULL);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("autocompile_toolchain_hash_test: ok\n");
    return 0;
}
