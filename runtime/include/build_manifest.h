#ifndef PSX_BUILD_MANIFEST_H
#define PSX_BUILD_MANIFEST_H
/* Separate conservative build-input identity. Never replaces execution,
 * title/content identity or supplies cross-build qualification by itself. */
#ifdef PSX_BUILD_MANIFEST_CONFIG
#include PSX_BUILD_MANIFEST_CONFIG
#else
#define PSX_BUILD_MANIFEST_SHA256 ""
#define PSX_BUILD_MANIFEST_SCHEMA 0
#endif
static inline const char *psx_compiled_build_manifest_sha256(void) {
    return PSX_BUILD_MANIFEST_SHA256;
}
static inline unsigned psx_compiled_build_manifest_schema(void) {
    return PSX_BUILD_MANIFEST_SCHEMA;
}
/* Empty SHA/schema0 means unavailable. Schema1 covers declared inputs only. */
#endif
