/* code_provider.c — the CodeProvider implementation + active selection.
 * See code_provider.h for the abstraction.
 *
 * The "spawn compiler" provider is a thin pass-through over the validated
 * autocompile_* path (compile emitted overlay C to a DLL and load it); it backs
 * BOTH the gcc and tcc tiers, which differ only in which compiler the pipeline
 * invokes. Which behavior is active is decided once at init by
 * overlay_backend_resolve() in overlay_backend.c (env PSX_OVERLAY_BACKEND >
 * [runtime] overlay_backend > auto). The auto tier order is:
 *   static -> gcc (if gcc on PATH) -> tcc (bundled toolchain-free fallback).
 * (The in-process sljit JIT provider was removed 2026-07-15; overlay gaps fall
 * to the interpreter.) */

#include <stddef.h>

#include "code_provider.h"
#include "autocompile.h"
#include "overlay_backend.h"
#include "overlay_loader.h"
#include "psx_cycles.h"

int code_provider_session_interpreted_sealed(void) {
    return overlay_loader_session_interpreted_sealed() &&
           autocompile_session_interpreted_sealed();
}
int code_provider_session_seal_interpreted(void) {
    if (code_provider_session_interpreted_sealed()) return 1;
    if (psx_get_cycle_count() != 0 || !overlay_loader_session_can_seal_interpreted()) return 0;
    if (!autocompile_session_set_interpreted(1)) return 0;
    if (!overlay_loader_session_set_interpreted(1)) {
        (void)autocompile_session_set_interpreted(0);
        return 0;
    }
    return 1;
}
int code_provider_session_end_interpreted(void) {
    int compiler_was_sealed = autocompile_session_interpreted_sealed();
    if (!compiler_was_sealed && !overlay_loader_session_interpreted_sealed()) return 1;
    if (!autocompile_session_set_interpreted(0)) return 0;
    if (!overlay_loader_session_set_interpreted(0)) {
        (void)autocompile_session_set_interpreted(compiler_was_sealed);
        return 0;
    }
    return 1;
}

/* ---- gcc provider: pass-through to the validated spawn->DLL->rescan path --- */
/* The hook signatures match autocompile_* exactly, so we point straight at
 * them — no wrappers, no behavior change. compile_fragment stays NULL: a
 * compiler spawn is orders of magnitude too slow for the dispatch path, so gcc
 * never produces a fragment synchronously (it self-improves via the batch path
 * + autocapture instead). */
static const CodeProvider s_gcc = {
    /* name             */ "gcc",
    /* available        */ autocompile_configured,
    /* request          */ autocompile_request,
    /* busy             */ autocompile_busy,
    /* poll_main        */ autocompile_poll_main,
};

/* ---- active selection ------------------------------------------------------ */
/* Default to gcc so any caller before code_provider_init() (there should be
 * none — init runs at overlay-cache setup) gets the proven path. */
static const CodeProvider *s_active = &s_gcc;

void code_provider_init(const char *cfg_backend, int gcc_configured) {
    if (code_provider_session_interpreted_sealed()) return;
    /* overlay_backend_resolve resolves AUTO to GCC or TCC (never AUTO). Both the
     * gcc and tcc tiers are the same spawn-a-compiler->DLL->rescan provider
     * (the actual compiler is whatever overlay_autocompile_cmd runs, selected in
     * main.cpp by the resolved backend), so there is a single provider now. The
     * call still runs for its side effect of logging the resolved backend
     * (surfaced via the overlay_status debug command). */
    (void)overlay_backend_resolve(cfg_backend, gcc_configured);
    s_active = &s_gcc;
}

const CodeProvider *code_provider_active(void) { return s_active; }
