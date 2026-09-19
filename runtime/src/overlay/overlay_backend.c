/* overlay_backend.c — compiler-neutral overlay tier-selection policy.
 * See overlay_backend.h. (Originally split out of overlay_sljit.c, whose
 * sljit tier was removed 2026-07-15: tier selection was never sljit's job.) */

#include "overlay_backend.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static OverlayBackend s_active   = OVERLAY_BACKEND_AUTO;
static int            s_resolved = 0;
static char           s_last_msg[256];

static void backend_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
}

const char *overlay_backend_name(OverlayBackend b) {
    switch (b) {
        case OVERLAY_BACKEND_GCC:         return "gcc";
        case OVERLAY_BACKEND_TCC:         return "tcc";
        case OVERLAY_BACKEND_AUTO_NO_GCC: return "auto-no-gcc";
        default:                          return "auto";
    }
}

static OverlayBackend parse_backend(const char *s, OverlayBackend dflt) {
    if (!s || !*s) return dflt;
    if (!strcmp(s, "gcc"))         return OVERLAY_BACKEND_GCC;
    /* "sljit" (removed 2026-07-15) is no longer recognized — a stale config
     * value falls through to `dflt` (AUTO), so old game.toml still boots. */
    if (!strcmp(s, "tcc"))         return OVERLAY_BACKEND_TCC;
    if (!strcmp(s, "auto-no-gcc")) return OVERLAY_BACKEND_AUTO_NO_GCC;
    if (!strcmp(s, "auto"))        return OVERLAY_BACKEND_AUTO;
    return dflt;
}

OverlayBackend overlay_backend_resolve(const char *cfg, int gcc_toolchain_available) {
    /* Precedence: env PSX_OVERLAY_BACKEND > game.toml [runtime] overlay_backend
     * (cfg) > AUTO. Tier order is static > gcc > tcc > interp. AUTO prefers gcc
     * when a gcc TOOLCHAIN is actually present (a dev /
     * production box), else tcc (the bundled toolchain-free user fallback).
     * AUTO_NO_GCC forces the tcc branch even when gcc IS present — it simulates a
     * toolchain-less user box on a dev machine (gcc shards still LOAD via the
     * loader; only the gap-fill compiler is forced to tcc). */
    OverlayBackend want = parse_backend(getenv("PSX_OVERLAY_BACKEND"),
                                        parse_backend(cfg, OVERLAY_BACKEND_AUTO));
    OverlayBackend eff = want;
    if (want == OVERLAY_BACKEND_AUTO)
        eff = gcc_toolchain_available ? OVERLAY_BACKEND_GCC : OVERLAY_BACKEND_TCC;
    else if (want == OVERLAY_BACKEND_AUTO_NO_GCC)
        eff = OVERLAY_BACKEND_TCC;

    s_active   = eff;
    s_resolved = 1;
    backend_log("backend resolved: want=%s effective=%s (gcc_toolchain=%d)",
                overlay_backend_name(want), overlay_backend_name(eff),
                gcc_toolchain_available);
    return eff;
}

OverlayBackend overlay_backend_active(void) { return s_active; }
