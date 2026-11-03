#include "interrupts.c"

int psx_mod_function_entry_active(void) { return 0; }
int g_cosim_dirty_pump_site;
int g_call_unit_depth;
int g_psx_dispatch_depth;
uint32_t g_dirty_safe_resume_pc;
int g_psx_savestate_pending;
static unsigned snapshot_polls;
static uint32_t snapshot_pc;
void savestate_poll(CPUState* cpu, uint32_t pc) {
    (void)cpu; ++snapshot_polls; snapshot_pc = pc;
}
uint32_t sio_get_seq(void) { return 0; }
uint64_t psx_get_cycle_count(void) { return 0; }
int g_ls_suppress_record;
void gpu_vblank_clear_deferred_present(void) {}

static void check(int ok, const char* message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

int main(void) {
    uint32_t pc = 0x80025070u;
    CPUState cpu = {0};
    s_compiled_interrupt_resume_pc = pc;
    poll_staged_user_snapshot(&cpu);
    check(snapshot_polls == 0, "idle snapshot work stays amortized");
    g_psx_savestate_pending = 1;
    poll_staged_user_snapshot(&cpu);
    check(snapshot_polls == 1 && snapshot_pc == pc,
          "staged request reaches the next native boundary");
    in_exception = 1;
    poll_staged_user_snapshot(&cpu);
    check(snapshot_polls == 1, "exception cannot service a user snapshot");
    in_exception = 0;
    g_dirty_safe_resume_pc = pc + 4;
    poll_staged_user_snapshot(&cpu);
    check(snapshot_polls == 2 && snapshot_pc == pc + 4,
          "staged dirty-code request uses its own resume boundary");
    g_psx_savestate_pending = 0;
    poll_staged_user_snapshot(&cpu);
    check(snapshot_polls == 2, "completed request stops urgent polling");
    g_dirty_safe_resume_pc = 0;
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
    g_call_unit_depth = 0;
    /* Model a restore's scheduler longjmp abandoning a dirty pump epilogue.
     * The next native menu boundary has no dirty PC and must be admissible. */
    g_cosim_dirty_pump_site = 1;
    g_dirty_safe_resume_pc = pc;
    interrupts_resync_after_restore();
    check(g_dirty_safe_resume_pc == 0 && g_cosim_dirty_pump_site == 0,
          "restore clears both abandoned dirty-pump scope fields");
    check(psx_irq_resume_context_snapshot_safe_at(pc),
          "native save after dirty load remains admissible");
    puts("host snapshot continuation checks passed");
    return 0;
}
