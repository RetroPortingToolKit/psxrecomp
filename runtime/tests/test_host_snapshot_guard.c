#include "psx_thread_local.h"
#include "interrupts.c"

int psx_mod_function_entry_active(void) { return 0; }
int g_cosim_dirty_pump_site;
PSX_THREAD_LOCAL int g_call_unit_depth;
PSX_THREAD_LOCAL int g_psx_dispatch_depth;
uint32_t g_dirty_safe_resume_pc;

static void check(int ok, const char* message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

int main(void) {
    uint32_t pc = 0x80025070u;
    check(psx_irq_resume_context_snapshot_safe_at(pc), "native boundary admitted");
    psx_snapshot_host_call_begin();
    psx_snapshot_host_call_begin();
    check(!psx_irq_resume_context_snapshot_safe_at(pc), "nested host adapter deferred");
    psx_snapshot_host_call_end();
    check(!psx_irq_resume_context_snapshot_safe_at(pc), "outer adapter still active");
    psx_snapshot_host_call_end();
    check(psx_irq_resume_context_snapshot_safe_at(pc), "register restore admits snapshot");
    g_cosim_dirty_pump_site = 1;
    g_dirty_safe_resume_pc = pc;
    check(psx_irq_resume_context_snapshot_safe_at(pc), "retired interpreter boundary admitted");
    psx_snapshot_host_call_begin();
    check(!psx_irq_resume_context_snapshot_safe_at(pc), "interpreter adapter deferred");
    psx_snapshot_host_call_reset(); /* scheduler longjmp abandoned the adapter */
    check(psx_irq_resume_context_snapshot_safe_at(pc), "scheduler escape releases abandoned scope");
    psx_snapshot_host_call_end(); /* no underflow */
    check(psx_irq_resume_context_snapshot_safe_at(pc), "unmatched end stays safe");
    psx_snapshot_host_call_begin();
    const unsigned outer_depth = psx_snapshot_host_call_depth();
    jmp_buf handler;
    if (setjmp(handler) == 0) {
        psx_snapshot_host_call_begin();
        longjmp(handler, 1); /* handler adapter's cleanup is skipped */
    }
    psx_snapshot_host_call_restore(outer_depth);
    check(!psx_irq_resume_context_snapshot_safe_at(pc), "exception preserves live outer adapter");
    psx_snapshot_host_call_end();
    check(psx_irq_resume_context_snapshot_safe_at(pc), "outer adapter returns without a leaked gate");
    check(!psx_irq_resume_context_snapshot_safe_at(pc + 4), "existing resume-PC guard retained");
    g_call_unit_depth = 1;
    check(!psx_irq_resume_context_snapshot_safe_at(pc), "existing call-unit guard retained");
    puts("host snapshot continuation checks passed");
    return 0;
}
