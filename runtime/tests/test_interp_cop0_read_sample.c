/* MFC0/CFC0 in the dirty-RAM interpreter read COP0 as it stood when the
 * instruction began.
 *
 * The interpreter charges an instruction's fetch and base cycle before its
 * body runs. When that charge reaches a device deadline, the device event runs
 * first and may raise CAUSE.IP2 (the interrupt line). The IRQ belongs to the
 * next instruction boundary, which is where compiled code, batching its
 * charges, makes it visible. This runs the production decoder
 * (interp_cop0_decoder.c) with cycle charging on and a device service that
 * raises IP2 on the read's own cycle:
 *
 *   - the read with the event due on its own cycle sees CAUSE without IP2,
 *     and CAUSE holds IP2 once the instruction has finished;
 *   - controls: no event, and an IRQ already raised before the instruction.
 *
 * test_interp_cop0_read_sample.py links it; every seam not defined here is an
 * abort() stub. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"

/* No mod hooks are active in this interpreter fixture. */
uint32_t g_psx_mod_instruction_hooks = 0;

int interp_test_step(CPUState *cpu, uint32_t pc, uint32_t insn, uint32_t *next);

/* Clock, cache and device seams read on the charged path. */
uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle;
uint64_t g_psx_cycle_fast_limit;
int      psx_in_device_service;
int      g_event_step_conservative;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
int      g_psx_cyc_bb_defer;
uint32_t *g_psx_cyc_local_acc;
int      g_psx_icache_active;          /* 0: no fetch cost */
uint32_t g_psx_icache_tv[1024];

static CPUState *s_device_cpu;
static unsigned  s_services;
void psx_devices_service_to_now(void) {
    ++s_services;
    s_device_cpu->cop0[13] |= 0x400u;   /* the device raises CAUSE.IP2 */
    psx_next_service_cycle = UINT64_MAX;
}

static int failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

enum { A1 = 5, CAUSE = 13 };
#define CAUSE_SYSCALL 0x20u   /* ExcCode 8 */
#define CAUSE_IP2     0x400u

/* COP0 reads now invalidate a stale PGXP destination shadow. Observe that
 * seam explicitly; every unrelated runtime seam remains an abort stub. */
static unsigned s_pgxp_calls;
static uint32_t s_pgxp_instr, s_pgxp_result;
void psx_pgxp_alu(CPUState *cpu, uint32_t instr, uint32_t result,
                  uint32_t s1, uint32_t s2) {
    ++s_pgxp_calls;
    s_pgxp_instr = instr;
    s_pgxp_result = result;
    CHECK(cpu == s_device_cpu, "PGXP received the wrong CPU");
    CHECK(result == cpu->gpr[A1], "PGXP did not receive the written GPR");
    CHECK(s1 == 0u && s2 == 0u, "COP0 read inherited ALU source provenance");
}

/* Runs `insn` (MFC0/CFC0 a1,$13) once and returns what it read. The device
 * deadline falls on the instruction's own cycle, or never. */
static uint32_t read_cause(uint32_t insn, int due_on_own_cycle, int raised_before) {
    static CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x80020000u;               /* outside the BIOS load-delay window */
    cpu.read_fudge = 32;
    cpu.ld_which_t = 32;
    cpu.cop0[CAUSE] = CAUSE_SYSCALL | (raised_before ? CAUSE_IP2 : 0u);
    s_device_cpu = &cpu;
    s_services = 0;
    s_pgxp_calls = 0;
    psx_cycle_count = 100000u;
    psx_next_service_cycle = due_on_own_cycle ? psx_cycle_count + 1u : UINT64_MAX;

    uint32_t next = 0;
    CHECK(interp_test_step(&cpu, cpu.pc, insn, &next) == 0,
          "0x%08X transferred control", (unsigned)insn);
    CHECK(s_services == (due_on_own_cycle ? 1u : 0u),
          "0x%08X: %u device services, expected %d", (unsigned)insn, s_services,
          due_on_own_cycle);
    const uint32_t cause_after = CAUSE_SYSCALL |
        ((due_on_own_cycle || raised_before) ? CAUSE_IP2 : 0u);
    CHECK(cpu.cop0[CAUSE] == cause_after,
          "0x%08X: CAUSE after the instruction is 0x%08X", (unsigned)insn,
          (unsigned)cpu.cop0[CAUSE]);
    CHECK(s_pgxp_calls == 1u && s_pgxp_instr == insn &&
          s_pgxp_result == cpu.gpr[A1],
          "0x%08X: missing/duplicate/wrong COP0 destination invalidation",
          (unsigned)insn);
    return cpu.gpr[A1];
}

int main(void) {
    const uint32_t mfc0 = 0x40056800u;   /* mfc0 a1,$13 */
    const uint32_t cfc0 = 0x40456800u;   /* cfc0 a1,$13 */
    for (int i = 0; i < 2; i++) {
        const uint32_t insn = i ? cfc0 : mfc0;
        uint32_t got = read_cause(insn, 1, 0);
        CHECK(got == CAUSE_SYSCALL,
              "0x%08X with the IRQ due on its own cycle read 0x%08X",
              (unsigned)insn, (unsigned)got);
        got = read_cause(insn, 0, 0);
        CHECK(got == CAUSE_SYSCALL, "0x%08X with no event read 0x%08X",
              (unsigned)insn, (unsigned)got);
        got = read_cause(insn, 0, 1);
        CHECK(got == (CAUSE_SYSCALL | CAUSE_IP2),
              "0x%08X with the IRQ raised before it read 0x%08X",
              (unsigned)insn, (unsigned)got);
    }
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS MFC0/CFC0 read CAUSE from before their own cycle charge");
    return 0;
}
