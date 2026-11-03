/* Synthetic handler over the real IRQ/scheduler implementation.
 * No ROM instructions, retail addresses, captures or reference-core bodies. */
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_scheduler.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint8_t g_test_ram[0x200000];
extern uint64_t psx_cycle_count;
extern uint32_t i_stat, i_mask;
extern uint32_t psx_read_word(uint32_t);
extern void psx_host_write_word(uint32_t, uint32_t);
#define HANDLER 0x80001000u
#define PCB 0x80000200u
#define TCB 0x80000300u
#define RESUME 0x80010000u
static int reason, alias_pc, scenario, deliveries, failures;
static uint32_t live_k0;
void psx_dispatch(CPUState *cpu, uint32_t pc) {
    if (pc != HANDLER) { fprintf(stderr, "unexpected dispatch %08X\n", pc); exit(2); }
    ++deliveries;
    i_stat = 0;
    for (unsigned i = 1; i < 32; ++i) cpu->gpr[i] = 0xD0000000u + i;
    cpu->gpr[26] = live_k0;
    cpu->hi = 0x11111111u;
    cpu->lo = 0x22222222u;
    uint32_t sr = cpu->cop0[12];
    cpu->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
    cpu->pc = cpu->cop0[14] ^ (alias_pc ? 0x20000000u : 0u);
    if (scenario == 1) psx_host_write_word(PCB, TCB + 0x100u);
    if (scenario == 2) cpu->pc = RESUME + 4u;
    if (scenario == 3) {
        g_exception_real_epc = RESUME + 4u;
        cpu->pc = RESUME + 4u;
    }
    if (scenario == 5) g_exception_real_epc = RESUME + 4u;
    g_exc_escape_reason = reason;
    psx_exception_longjmp();
}
static void check(const char *field, uint32_t want, uint32_t got) {
    if (want != got) {
        fprintf(stderr, "%s: expected %08X, got %08X\n", field, want, got);
        ++failures;
    }
}
static void run_case(int mode, int selected_reason, int tcb, int alias, unsigned value) {
    CPUState cpu;
    uint32_t saved[32];
    memset(&cpu, 0, sizeof(cpu));
    memset(g_test_ram, 0, sizeof(g_test_ram));
    cpu.read_word = psx_read_word;
    cpu.write_word = psx_host_write_word;
    psx_host_write_word(0x80u, 0x3C1A8000u);
    psx_host_write_word(0x84u, 0x275A1000u);
    psx_host_write_word(0x108u, tcb ? PCB : 0u);
    psx_host_write_word(PCB, TCB);
    for (unsigned i = 0; i < 32; ++i)
        cpu.gpr[i] = saved[i] = i ? 0x50000000u + i : 0u;
    cpu.hi = 0x33333333u;
    cpu.lo = 0x44444444u;
    cpu.cop0[12] = 0x401u;
    cpu.pc = RESUME;
    reason = selected_reason; alias_pc = alias;
    live_k0 = 0xE0000000u + value * 7919u;
    psx_cycle_count = 1000;
    i_stat = i_mask = 1;
    interrupts_resync_after_restore();
    psx_scheduler_top_level_resume_clear();
    if (scenario == 4) cpu.pc = 0;
    psx_check_interrupts_at(&cpu, scenario == 4 ? 0 : RESUME);
    const int pc_matches = scenario != 2 && scenario != 5;
    const int stable_tcb = tcb && scenario != 1;
    int restored = reason == PSX_EXC_ESCAPE_LEGACY_SENTINEL ||
                   (mode == 1 && pc_matches) ||
                   (mode == 2 && stable_tcb && pc_matches) ||
                   (mode == 3 && stable_tcb &&
                    (reason == PSX_EXC_ESCAPE_RFE_RETURN ||
                     reason == PSX_EXC_ESCAPE_SYSCALL_RETURN)) || scenario == 4;
    int keep = mode == 1 && stable_tcb && scenario == 0 &&
               reason == PSX_EXC_ESCAPE_RFE_RETURN;
    for (unsigned i = 1; i < 32; ++i)
        check("GPR", keep && i == 26 ? live_k0 :
              (restored ? saved[i] : (i == 26 ? live_k0 : 0xD0000000u + i)),
              cpu.gpr[i]);
    check("zero register", 0, cpu.gpr[0]);
    check("HI", restored ? 0x33333333u : 0x11111111u, cpu.hi);
    check("LO", restored ? 0x44444444u : 0x22222222u, cpu.lo);
    check("raw PC", scenario == 4 ? 0u :
          (scenario == 2 || scenario == 3 ? RESUME + 4u :
           RESUME ^ (alias ? 0x20000000u : 0u)), cpu.pc);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int mode = atoi(argv[1]);
    for (unsigned value = 0; value < 32; ++value)
        for (int alias = 0; alias < 2; ++alias)
            for (int tcb = 0; tcb < 2; ++tcb) {
                run_case(mode, PSX_EXC_ESCAPE_RFE_RETURN, tcb, alias, value);
                run_case(mode, PSX_EXC_ESCAPE_SYSCALL_RETURN, tcb, alias, value);
                run_case(mode, PSX_EXC_ESCAPE_LEGACY_SENTINEL, tcb, alias, value);
            }
    for (scenario = 1; scenario <= 5; ++scenario)
        run_case(mode, PSX_EXC_ESCAPE_RFE_RETURN, 1, 0, 123);
    check("deliveries", 389, (uint32_t)deliveries);
    printf("same-thread RFE mode %d: %d deliveries, %d failures\n",
           mode, deliveries, failures);
    return failures ? 1 : 0;
}
