#include "cpu_state.h"
#include "psx_execution.h"
#include <stdlib.h>
#include <setjmp.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
PSX_THREAD_LOCAL PsxExecutionDispatch g_psx_execution_dispatch;
PSX_THREAD_LOCAL PsxExecutionAbort g_psx_execution_abort;
PSX_THREAD_LOCAL int g_psx_dispatch_depth, g_psx_call_bail;
PSX_THREAD_LOCAL uint64_t g_psx_bail_first, g_psx_bail_resolved, g_psx_bail_flattened, g_psx_bail_anomaly;
PSX_THREAD_LOCAL void (*g_overlay_flush_pending_cycles)(void);
PSX_THREAD_LOCAL uint32_t g_debug_current_func_addr;
static unsigned visits, flushed;
static jmp_buf landing;
static void flush(void) { ++flushed; }
static void failure(const char *reason, uint32_t addr) {
    CHECK(reason && addr == 0xDEAD); longjmp(landing, 1);
}
static void leaf(CPUState *cpu) {
    CHECK(g_overlay_flush_pending_cycles == flush);
    ++visits; cpu->gpr[2] += 17; cpu->pc = cpu->gpr[31];
}
static void nested(CPUState *cpu) {
    uint32_t ra = cpu->gpr[31];
    cpu->gpr[31] = 0x104;
    g_psx_execution_dispatch(cpu, 0x200, 0x104);
    CHECK(!g_psx_call_bail && !cpu->pc);
    cpu->gpr[31] = ra; cpu->pc = ra;
}
static void wild(CPUState *cpu) { cpu->gpr[31] = 0x400; cpu->pc = 0; }
static void final(CPUState *cpu) { ++visits; cpu->gpr[31] = 0x800; cpu->pc = 0x800; }
static int lookup(void *user, CPUState *cpu, uint32_t pc, PsxExecutionCode *code, void (**out_flush)(void)) {
    CHECK(user == &visits && cpu);
    code->resume_pc = pc; *out_flush = flush;
    switch (pc) {
    case 0x100: code->function = nested; break;
    case 0x200: code->function = leaf; break;
    case 0x300: code->function = wild; break;
    case 0x400: code->function = final; break;
    case 0x500: code->resume_pc = cpu->gpr[31]; return 2;
    default: return 0;
    }
    return 1;
}
int main(void) {
    CPUState cpu = {0}; cpu.gpr[29] = 0x10000; cpu.gpr[31] = 0x800;
    psx_execution_begin(&visits, lookup, failure);
    g_psx_execution_dispatch(&cpu, 0x100, 0x800);
    CHECK(visits == 1 && cpu.gpr[2] == 17 && !cpu.pc && !g_psx_dispatch_depth);
    CHECK(flushed >= 3 && !g_overlay_flush_pending_cycles);
    g_psx_execution_dispatch(&cpu, 0x300, 0x800);
    CHECK(visits == 2 && g_psx_bail_first == 1 && g_psx_bail_flattened == 1);
    g_psx_execution_dispatch(&cpu, 0x500, 0x800);
    CHECK(!cpu.pc && !g_psx_dispatch_depth);
    if (!setjmp(landing)) { g_psx_execution_dispatch(&cpu, 0xDEAD, 0x800); abort(); }
    psx_execution_end();
    CHECK(!g_psx_execution_dispatch && !g_psx_execution_abort && !g_psx_dispatch_depth);
    return 0;
}
