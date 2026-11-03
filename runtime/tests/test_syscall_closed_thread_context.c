/* SYS03 must preserve register RAM even after CloseTh marks the outgoing TCB
 * free. Generic register patterns only: no game addresses. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "cpu_state.h"
#include "psx_scheduler.h"

static uint32_t ram[0x10000 / 4];
static CPUState cpu;
uint64_t s_frame_count = 0;

static uint32_t rd(uint32_t a) { assert(a < sizeof ram && !(a & 3)); return ram[a / 4]; }
static void wr(uint32_t a, uint32_t v) { assert(a < sizeof ram && !(a & 3)); ram[a / 4] = v; }
int psx_get_in_exception(void) { return 0; }
int overlay_loader_shadow_native_thread_switch_bail(void) { return 0; }
int parity_trace_is_armed(void) { return 0; }
void debug_server_log_thread_event(uint32_t k, CPUState* c, uint32_t a, uint32_t b, uint32_t p)
{ (void)k; (void)c; (void)a; (void)b; (void)p; }
int psx_syscall(CPUState*, uint32_t);

int main(void)
{
    const uint32_t pcb = 0xe000, old = 0xe1f4, target = old + 0xc0;
    ram[0x108 / 4] = pcb;
    ram[0x110 / 4] = old;
    ram[0x114 / 4] = 4 * 0xc0;
    ram[pcb / 4] = old;
    ram[old / 4] = 0x1000;      /* Closed, but still the current thread until SYS03. */
    ram[target / 4] = 0x4000;
    cpu.read_word = rd;
    cpu.write_word = wr;
    for (unsigned i = 1; i < 32; i++) cpu.gpr[i] = 0x16160000u + i;
    cpu.gpr[4] = 3;
    cpu.gpr[5] = target;
    cpu.gpr[31] = 0x2104;
    cpu.cop0[12] = 0x401;
    cpu.hi = 0xaaaa;
    cpu.lo = 0xbbbb;
    if (setjmp(g_scheduler_jmpbuf) == 0) {
        psx_syscall(&cpu, 0);
        assert(!"must yield to scheduler");
    }
    assert(ram[old / 4] == 0x1000);   /* Saving never reopens the retired thread. */
    assert(ram[pcb / 4] == target);
    assert(ram[(old + 8 + 22 * 4) / 4] == 0x16160016u);
    assert(ram[(old + 8 + 128) / 4] == 0x2104);
    assert(ram[(old + 8 + 132) / 4] == 0xaaaa && ram[(old + 8 + 136) / 4] == 0xbbbb);
    assert(g_sched_escape.target_tcb == target);
    return 0;
}
