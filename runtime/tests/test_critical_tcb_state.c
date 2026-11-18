/* Memory-visible critical calls followed by TCB reuse. OpenThread replaces
 * EPC/SP/GP but leaves SR and the remaining saved registers intact. */
#include "cpu_state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__attribute__((always_inline)) inline int psx_syscall(CPUState *cpu, uint32_t code);
#include "../src/traps.c"

static uint32_t ram[0x10000 / 4];
uint64_t s_frame_count;
int psx_get_in_exception(void) { return 0; }
int source_gpu_runtime_active(void) { return 0; }
void psx_fatal_halt(const char *reason) { (void)reason; abort(); }
void psx_irq_refresh_cause_ip2(void) {}
void debug_server_log_thread_event(uint32_t kind, CPUState *cpu,
    uint32_t from, uint32_t to, uint32_t pc)
{
    (void)kind; (void)cpu; (void)from; (void)to; (void)pc;
}
static uint32_t read_ram(uint32_t address) { return ram[(address & 0xffffu) / 4u]; }
static void write_ram(uint32_t address, uint32_t value) { ram[(address & 0xffffu) / 4u] = value; }

static inline __attribute__((always_inline)) int check_call(
    unsigned func, uint32_t status, uint32_t result)
{
    CPUState cpu = {0}, expected, resumed = {0};
    const uint32_t tcb = 0x800020c0u, pc = 0x80012340u, entry = 0x80034560u;
    memset(ram, 0, sizeof(ram));
    write_ram(0x108, 0x80001000u); write_ram(0x1000, tcb);
    write_ram(0x110, 0x80002000u); write_ram(0x114, 0x300u);
    /* SFEX calls SYS02 while the current valid block is already closed. */
    write_ram(tcb, func == 2 ? 0x1000u : 0x4000u);
    write_ram(tcb + 0x94, 0x40000401u);
    for (unsigned i = 1; i < 32; ++i) cpu.gpr[i] = 0x12340000u + i;
    cpu.gpr[2] = result; cpu.gpr[4] = func; cpu.pc = pc;
    cpu.hi = 0xaabbccddu; cpu.lo = 0x11223344u;
    cpu.cop0[12] = status; cpu.cop0[13] = 0x8000047cu;
    cpu.read_word = read_ram; cpu.write_word = write_ram;
    expected = cpu;
    expected.pc = 0;
    expected.cop0[12] = func == 1 ? status & ~1u : status | 0x401u;
    expected.gpr[2] = func == 1 ? status & 1u : result;
    if (psx_syscall(&cpu, 0) != 0 || memcmp(&cpu, &expected, sizeof(cpu))) {
        fprintf(stderr, "critical_tcb_state: SYS%02u changed live contract\n", func);
        return 1;
    }
    /* Exception entry pushes the status stack. The installed kernel edits
     * saved IEp/IM2 and tests both enable bits for the saved SYS01 result.
     * These memory effects are distinct from the preserved legacy live API. */
    uint32_t saved_status = (status & ~0x3fu) | ((status & 15u) << 2);
    saved_status = func == 1 ? saved_status & ~0x404u : saved_status | 0x404u;
    uint32_t saved_result = func == 1 ? ((status & 0x401u) == 0x401u) : result;
    if (read_ram(tcb + 0x94) != saved_status || read_ram(tcb + 0x10) != saved_result) {
        fprintf(stderr, "critical_tcb_state: SYS%02u incomingSR=%08X savedSR=%08X expectedSR=%08X savedv0=%08X expectedv0=%08X\n",
            func, status, read_ram(tcb + 0x94), saved_status,
            read_ram(tcb + 0x10), saved_result);
        return 1;
    }
    if (read_ram(tcb + 0x88) != pc + 4 || read_ram(tcb + 0x98) != 0x420u ||
        read_ram(tcb + 0x8c) != cpu.hi || read_ram(tcb + 0x90) != cpu.lo) {
        fprintf(stderr, "critical_tcb_state: SYS%02u saved EPC/Cause/HI/LO mismatch\n", func);
        return 1;
    }
    write_ram(tcb, 0x1000u); write_ram(tcb, 0x4000u);
    write_ram(tcb + 0x88, entry); write_ram(tcb + 0x7c, 0x8000f000u);
    resumed.read_word = read_ram; resumed.write_word = write_ram;
    uint32_t target = psx_restore_context_from_tcb(&resumed, tcb);
    uint32_t restored_status = func == 1 ? status & ~0x401u : status | 0x401u;
    if (target != entry || resumed.cop0[12] != restored_status ||
        resumed.gpr[2] != saved_result || resumed.gpr[31] != expected.gpr[31] ||
        resumed.gpr[29] != 0x8000f000u || resumed.hi != cpu.hi || resumed.lo != cpu.lo) {
        fprintf(stderr, "critical_tcb_state: SYS%02u closed/reopened context mismatch\n", func);
        return 1;
    }
    return 0;
}
int main(void)
{
    const uint32_t statuses[] = {0, 1, 0x400u, 0x401u,
        0x40000000u, 0x40000001u, 0x40000400u, 0x40000401u};
    const uint32_t results[] = {0, 1, 0x80010000u, UINT32_MAX};
    unsigned failures = 0;
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j) {
        failures += check_call(1, statuses[i], results[j]);
        failures += check_call(2, statuses[i], results[j]);
    }
    if (failures) {
        fprintf(stderr, "critical_tcb_state: FAIL (%u/64 memory-backed SYS01/SYS02 cases)\n", failures);
        return 1;
    }
    puts("critical_tcb_state: PASS (64 memory-backed SYS01/SYS02 close/reopen cases)");
    return 0;
}
