/* code_provider.h — the backend-agnostic code-production seam.
 *
 * One interface the overlay capture/dispatch spine talks to instead of calling
 * a specific backend directly. The validated gcc/tcc spawn->DLL->rescan path is
 * its implementation. Everything that makes overlays correct — content-keyed
 * multi-candidate dispatch, per-call live-byte validation, the self-mod
 * blacklist, the coverage manifest — is backend-blind.
 *
 * Production is BATCH / ASYNC (request / busy / poll_main): kicked by the
 * autocapture tick, spawns a compiler off-thread, applied later via a cache
 * rescan on the emu thread. Seconds of latency; many fragments per run. A
 * provider sets the hooks it supports and leaves the rest NULL; callers
 * null-check before invoking.
 *
 * (The in-process sljit JIT provider — a synchronous on-miss producer with a
 * compile_fragment hook — was removed 2026-07-15; it had been disabled by
 * default since 2026-06-25 due to an emitter bug. Gaps gcc/tcc have not yet
 * covered fall to the interpreter, the always-safe precision-over-recall floor.)
 */
#ifndef PSXRECOMP_CODE_PROVIDER_H
#define PSXRECOMP_CODE_PROVIDER_H

#include "cpu_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native shard ABI — identical to overlay_loader's OverlayFn. */
typedef void (*CodeProviderFn)(CPUState *);

typedef struct CodeProvider {
    const char *name;                 /* "gcc" (spawns gcc OR tcc)             */
    int  (*available)(void);          /* a compile cmd is configured           */

    /* Batch/async production (autocapture-driven). request() returns 1 if a
     * compile was started. busy() is 1 while one is in flight. poll_main() runs
     * on the emu thread to apply a finished compile (cache rescan). Any may be
     * NULL on a provider that does not do batch production. */
    int  (*request)(void);
    int  (*busy)(void);
    void (*poll_main)(void);
} CodeProvider;

/* Resolve the active backend (overlay_backend_resolve) and cache the matching
 * provider. cfg_backend = [runtime] overlay_backend ("auto"|"gcc"|"tcc"|
 * "auto-no-gcc", may be NULL/empty for auto); gcc_configured =
 * autocompile_configured(). Call once at overlay-cache init, on the emu thread,
 * before the run loop starts. */
void code_provider_init(const char *cfg_backend, int gcc_configured);

/* The active/primary provider (owns the batch path + default selection). Never
 * NULL — defaults to the gcc provider before code_provider_init() runs. */
const CodeProvider *code_provider_active(void);

/* Fixed interpreted-overlay session policy. Startup: owner/emulation thread,
 * cache initialization joined, autocompile_shutdown joined/discarded, before
 * first guest cycle/overlay dispatch. Busy/late returns0 without policy change.
 * End only after guest/fiber execution has stopped. Never unloads native code
 * or changes offline preferences; compiled main and trusted plugins remain. */
int code_provider_session_seal_interpreted(void);
int code_provider_session_end_interpreted(void);
int code_provider_session_interpreted_sealed(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CODE_PROVIDER_H */
