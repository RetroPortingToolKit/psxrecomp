/* Reuse the production-interpreter fixture's inactive device seams. */
#define main refused_text_main
#define psx_get_in_exception original_exception
#define psx_game_text_native_ok original_native_ok
#define psx_dispatch_game_compiled original_compiled
#define psx_check_interrupts original_interrupts
#define psx_rfe_escape_check original_rfe_escape
#include "test_interp_refused_game_text.c"
#undef main
#undef psx_get_in_exception
#undef psx_game_text_native_ok
#undef psx_dispatch_game_compiled
#undef psx_check_interrupts
#undef psx_rfe_escape_check
#include "dirty_ram_interp.h"
#include "psx_static_calls.h"
#include <assert.h>
#include <setjmp.h>

int g_psx_dispatch_depth = 1;
static int started = 1, exception, allowed = 1, calls, pumps, pump_mode;
static uint32_t next_pc = 0x80042010u;
static uint64_t retire;
static int tail_escape;
static jmp_buf tail_escape_jmp;
void psx_rfe_escape_check(CPUState *cpu) {
    (void)cpu;
    if (tail_escape) longjmp(tail_escape_jmp, 1);
}

extern int g_precise_mode, g_ls_replay_active, g_exec_phase, g_dirty_interp_active;
extern uint64_t g_dirty_ram_insns_run;
int fntrace_is_game_started(void) { return started; }
int psx_get_in_exception(void) { return exception; }
int psx_game_text_native_ok(uint32_t addr) { (void)addr; return allowed; }
#if defined(PSX_NATIVE_TAIL_CHAINS) && PSX_NATIVE_TAIL_CHAINS
int g_psx_guest_time_frozen, g_psx_call_bail;
volatile int g_ds_recording;
uint32_t *g_psx_cyc_local_acc;
static int tail_mode;
static unsigned tail_calls, tail_hooks_seen;
static uintptr_t tail_stack_lo = UINTPTR_MAX, tail_stack_hi;
static void tail_other(CPUState *cpu) { (void)cpu; }
/* Observe a real stack slot in a completed helper invocation. A slot owned by
 * tail_body would still be alive when its address escapes across musttail;
 * GCC correctly rejects that lifetime even when we retain only the number. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void tail_probe_stack(void) {
    volatile unsigned stack_local = 0;
    uintptr_t stack = (uintptr_t)&stack_local;
    if (stack < tail_stack_lo) tail_stack_lo = stack;
    if (stack > tail_stack_hi) tail_stack_hi = stack;
}
static void tail_body(CPUState *cpu) {
    tail_probe_stack();
    ++tail_calls;
    if (!cpu->gpr[4]) { cpu->pc = next_pc; return; }
    --cpu->gpr[4]; cpu->pc = 0x80042000u;
    if (tail_mode == 5) g_precise_mode = 1;
    if (tail_mode == 6) g_psx_guest_time_frozen = 1;
    PSX_TRY_NATIVE_TAIL(cpu, 0x80042000u,
        tail_mode == 3 ? tail_other : tail_body, tail_body);
}
static int tail_hooks(CPUState *cpu, uint32_t target) {
    assert(target == 0x80042000u && g_exec_phase == 7 && g_dirty_interp_active == 9);
    ++tail_hooks_seen; cpu->pc = 0;
    if (tail_mode == 2) { ++cpu->gpr[2]; cpu->pc = next_pc; return 1; }
    return 0;
}
int psx_game_native_tail_enter(CPUState *cpu, uint32_t target, PsxNativeTailBody body) {
    assert(target == 0x80042000u && body == tail_body && allowed);
    assert(g_exec_phase == 3 && g_dirty_interp_active == 1);
    cpu->pc = 0; return 1;
}
#endif
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr) {
    assert(addr == 0x80042000u && g_exec_phase == 3 && g_dirty_interp_active == 1);
    ++calls;
#if defined(PSX_NATIVE_TAIL_CHAINS) && PSX_NATIVE_TAIL_CHAINS
    if (tail_mode) {
        psx_native_tail_note_body(cpu, tail_body);
        tail_body(cpu);
        return 1;
    }
#endif
    cpu->pc = next_pc;
    g_dirty_ram_insns_run += retire;
    return 1;
}
void psx_check_interrupts(CPUState *cpu) {
    ++pumps;
    assert(g_dirty_interp_active == 1);
    cpu->pc = pump_mode ? 0u : 0x80043000u;
}
static int enter(CPUState *cpu) {
    const int r = dirty_ram_dispatch_game_direct(cpu, 0x80042000u);
    assert(g_exec_phase == 7 && g_dirty_interp_active == 9);
    return r;
}
int main(void) {
    CPUState cpu = {0};
    g_exec_phase = 7; g_dirty_interp_active = 9;
    assert(enter(&cpu) && calls == 1 && cpu.pc == next_pc);
    started = 0; assert(!enter(&cpu)); started = 1;
    exception = 1; assert(!enter(&cpu)); exception = 0;
    g_precise_mode = 1; assert(!enter(&cpu)); g_precise_mode = 0;
    g_ls_replay_active = 1; assert(!enter(&cpu)); g_ls_replay_active = 0;
    g_psx_dispatch_depth = 2; assert(!enter(&cpu)); g_psx_dispatch_depth = 1;
    dirty_ram_span_set(0x42000u, 0x42004u);
    assert(!enter(&cpu)); dirty_ram_span_set(0u, 0u);
    allowed = 0; assert(!enter(&cpu)); allowed = 1;
    assert(calls == 1);
    /* Original shared postlude must pump an owed dirty budget and preserve
     * both a redirected interrupt PC and the null-PC RFE recovery contract. */
    retire = 4096;
    assert(enter(&cpu) && pumps == 1 && cpu.pc == 0x80043000u);
    pump_mode = 1;
    assert(enter(&cpu) && pumps == 2 && cpu.pc == next_pc);
    PsxStaticCallStats stats;
    psx_static_call_get_stats(&stats);
    assert(stats.fast_entries == 3u && stats.fast_misses == 1u);
#if defined(PSX_NATIVE_TAIL_CHAINS) && PSX_NATIVE_TAIL_CHAINS
    retire = 0; tail_mode = 1; tail_calls = tail_hooks_seen = 0;
    cpu.gpr[4] = 50000;
    assert(dirty_ram_dispatch_game_chain(&cpu, 0x80042000u, 1, tail_hooks));
    assert(tail_calls == 50001 && tail_hooks_seen == 50000);
    assert(cpu.pc == next_pc && g_exec_phase == 7 && g_dirty_interp_active == 9);
    assert(tail_stack_hi - tail_stack_lo < 16384u);
    /* An override is handled once and does not call the native callee. */
    tail_mode = 2; tail_calls = tail_hooks_seen = 0; cpu.gpr[4] = 2; cpu.gpr[2] = 0;
    assert(dirty_ram_dispatch_game_chain(&cpu, 0x80042000u, 1, tail_hooks));
    assert(tail_calls == 1 && tail_hooks_seen == 1 && cpu.gpr[2] == 1 && cpu.pc == next_pc);
    /* Source mismatch, stop-bearing/nested scope and active precise/frozen
     * execution surface the original CPS target without executing hooks. */
    for (int mode = 3; mode <= 6; ++mode) {
        tail_mode = mode; tail_calls = tail_hooks_seen = 0; cpu.gpr[4] = 10;
        assert(dirty_ram_dispatch_game_chain(&cpu, 0x80042000u, mode != 4, tail_hooks));
        assert(tail_calls == 1 && tail_hooks_seen == 0 && cpu.pc == 0x80042000u);
        g_precise_mode = g_psx_guest_time_frozen = 0;
    }
    tail_mode = 1; tail_escape = 1; cpu.gpr[4] = 2;
    if (!setjmp(tail_escape_jmp)) {
        dirty_ram_dispatch_game_chain(&cpu, 0x80042000u, 1, tail_hooks);
        assert(!"RFE must escape before a chained entry");
    }
    /* Mirror the existing IRQ landing's ambient reset, then exercise a
     * legacy/non-chain handoff. It must consume no abandoned permission. */
    extern int g_mixed_depth;
    g_mixed_depth = 0; g_exec_phase = 7; g_dirty_interp_active = 9;
    tail_escape = 0; tail_calls = tail_hooks_seen = 0; cpu.gpr[4] = 10;
    assert(enter(&cpu));
    assert(tail_calls == 1 && tail_hooks_seen == 0 && cpu.pc == 0x80042000u);
    tail_mode = 0;
    puts("guaranteed 50000-entry chain stays bounded; logical hooks, raw-caller/stop/mode refusals preserved");
#endif
    puts("static native entry scope, fallback and dirty IRQ postlude passed");
    return 0;
}
