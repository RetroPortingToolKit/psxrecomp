/* Reuse the production-interpreter fixture's inactive device seams. */
#define main refused_text_main
#define psx_get_in_exception original_exception
#define psx_game_text_native_ok original_native_ok
#define psx_dispatch_game_compiled original_compiled
#define psx_check_interrupts original_interrupts
#include "test_interp_refused_game_text.c"
#undef main
#undef psx_get_in_exception
#undef psx_game_text_native_ok
#undef psx_dispatch_game_compiled
#undef psx_check_interrupts
#include "dirty_ram_interp.h"
#include "psx_static_calls.h"
#include <assert.h>

int g_psx_dispatch_depth = 1;
static int started = 1, exception, allowed = 1, calls, pumps, pump_mode;
static uint32_t next_pc = 0x80042010u;
static uint64_t retire;
extern int g_precise_mode, g_ls_replay_active, g_exec_phase, g_dirty_interp_active;
extern uint64_t g_dirty_ram_insns_run;
int fntrace_is_game_started(void) { return started; }
int psx_get_in_exception(void) { return exception; }
int psx_game_text_native_ok(uint32_t addr) { (void)addr; return allowed; }
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr) {
    assert(addr == 0x80042000u && g_exec_phase == 3 && g_dirty_interp_active == 1);
    ++calls;
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
    puts("static native entry scope, fallback and dirty IRQ postlude passed");
    return 0;
}
