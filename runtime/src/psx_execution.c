/* Native dispatch for a sealed, private guest execution. */
#include "psx_execution.h"
#include "cpu_state.h"
#include "psx_bios_backend.h"
#include <stdlib.h>
static PSX_THREAD_LOCAL void *t_user;
static PSX_THREAD_LOCAL PsxExecutionLookup t_lookup;
extern PSX_THREAD_LOCAL void (*g_overlay_flush_pending_cycles)(void);
extern PSX_THREAD_LOCAL uint32_t g_debug_current_func_addr;
static void execution_fault(const char *reason, uint32_t addr) {
    if (g_psx_execution_abort) g_psx_execution_abort(reason, addr);
    abort();
}
static void dispatch_private(CPUState *cpu, uint32_t addr, uint32_t stop) {
    if (!t_lookup || !cpu) execution_fault("execution_unbound", addr);
    const uint32_t sp = cpu->gpr[29];
    const int outermost = ++g_psx_dispatch_depth == 1;
    if (g_psx_dispatch_depth > 4096) execution_fault("dispatch_depth", addr);
    for (;;) {
        PsxExecutionCode code = {0};
        void (*flush)(void) = NULL;
        int result = t_lookup(t_user, cpu, addr, &code, &flush);
        if (!result) execution_fault("dispatch_miss", addr);
        if (result == 2) {
            cpu->pc = code.resume_pc;
        } else {
            if (!code.function) execution_fault("native_body_missing", addr);
            void (*previous)(void) = g_overlay_flush_pending_cycles;
            if (previous) previous();
            g_overlay_flush_pending_cycles = flush;
            g_debug_current_func_addr = addr;
            cpu->pc = code.resume_pc;
            code.function(cpu);
            if (flush) flush();
            g_overlay_flush_pending_cycles = previous;
        }
        if (g_psx_call_bail) {
            if (stop && cpu->pc == stop && cpu->gpr[29] == sp) {
                g_psx_call_bail = 0; ++g_psx_bail_resolved; cpu->pc = 0;
                --g_psx_dispatch_depth; return;
            }
            if (!outermost) { --g_psx_dispatch_depth; return; }
            g_psx_call_bail = 0; ++g_psx_bail_flattened;
            addr = cpu->pc; continue;
        }
        if (!cpu->pc) {
            if (stop && (cpu->gpr[29] != sp || cpu->gpr[31] != stop)) {
                g_psx_call_bail = 1; ++g_psx_bail_first;
                cpu->pc = cpu->gpr[31];
                if (outermost) {
                    g_psx_call_bail = 0; ++g_psx_bail_flattened;
                    addr = cpu->pc; continue;
                }
            }
            --g_psx_dispatch_depth; return;
        }
        if (stop && cpu->pc == stop && cpu->gpr[29] == sp) {
            cpu->pc = 0; --g_psx_dispatch_depth; return;
        }
        addr = cpu->pc;
    }
}
void psx_execution_begin(void *user, PsxExecutionLookup lookup, PsxExecutionAbort failure) {
    if (!lookup || !failure || g_psx_execution_dispatch) abort();
    t_user = user; t_lookup = lookup;
    g_psx_execution_abort = failure;
    g_psx_execution_dispatch = dispatch_private;
    g_psx_dispatch_depth = 0; g_psx_call_bail = 0;
}
void psx_execution_end(void) {
    /* An abort can skip generated cleanups. Settle the current DLL before
     * its frozen clock and callback ownership are released by the worker. */
    if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();
    g_overlay_flush_pending_cycles = NULL;
    g_psx_dispatch_depth = 0; g_psx_call_bail = 0;
    g_psx_execution_dispatch = NULL; g_psx_execution_abort = NULL;
    t_user = NULL; t_lookup = NULL;
}
