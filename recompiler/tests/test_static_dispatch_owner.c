/* The test runner supplies owner.inc from actual generated BIOS output.
 * Only the BIOS lookup table and backend/device callbacks are doubles. */
#include "cpu_state.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*PsxRecompFunc)(CPUState *);
typedef struct { uint32_t addr; PsxRecompFunc func; } DispatchEntry;
static void unused_body(CPUState *cpu) { (void)cpu; abort(); }
static const DispatchEntry dispatch_table[] = {{0xffffffffu, unused_body}};
int g_psx_dispatch_depth, g_psx_call_bail;
uint64_t g_psx_bail_resolved, g_psx_bail_flattened, g_psx_bail_first;
uint64_t g_dispatch_static_hits;
uint32_t g_debug_current_func_addr;
int g_psx_mod_guest_functions = 1;
static int tier, mode, graph_calls, native_calls, fallback_calls, trace_calls;
static int boundary_redirect, escape;
static jmp_buf escape_jmp;
static char hooks[16];
static unsigned hook_count;
static void hook(char c) { if (hook_count < sizeof hooks - 1) hooks[hook_count++] = c; }
static uint32_t normalize(uint32_t addr) { return addr & 0x1fffffffu; }
static int fntrace_is_game_started(void) { return 1; }
static void fntrace_record(CPUState *cpu, uint32_t addr) {
    (void)cpu; (void)addr; ++trace_calls;
    assert(trace_calls < 100100);
}
static int psx_mod_dispatch_guest_function(CPUState *cpu, uint32_t addr) {
    (void)addr; hook('G');
    if (tier != 1) return 0;
    cpu->pc = cpu->gpr[31]; return 1;
}
static int hle(CPUState *cpu, uint32_t phys) { (void)cpu; (void)phys; hook('H'); return tier == 2; }
static int (*g_psx_bios_hle_hook)(CPUState *, uint32_t) = hle;
static int psx_bios_try_native_call_stub(CPUState *cpu, uint32_t addr) {
    (void)addr; hook('S'); if (tier != 3) return 0;
    cpu->pc = cpu->gpr[31]; return 1;
}
static int override(CPUState *cpu, uint32_t phys) { (void)cpu; (void)phys; hook('O'); return tier == 4; }
static int (*g_psx_func_override_hook)(CPUState *, uint32_t) = override;
static int psx_game_address_in_text(uint32_t addr) { (void)addr; return 1; }
static int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 1; }
static int dirty_ram_text_native_ok(uint32_t phys) { (void)phys; return 1; }
static int psx_kernel_bless_dispatchable(uint32_t phys) { (void)phys; return 0; }
static void debug_server_trace_dispatch(uint32_t addr) { (void)addr; abort(); }
static PsxRecompFunc psx_bios_hit_body(CPUState *cpu, uint32_t a, uint32_t p, PsxRecompFunc fn) {
    (void)cpu; (void)a; (void)p; return fn;
}
static void psx_unknown_dispatch(CPUState *cpu, uint32_t a, uint32_t p) {
    (void)cpu; (void)a; (void)p; abort();
}
static void graph(CPUState *cpu, uint32_t addr) {
    ++graph_calls;
    if (mode == 1 && cpu->gpr[4]-- != 0) { /* guest recursion stays in one owner */
        cpu->gpr[29] -= 16; cpu->pc = addr; return;
    }
    if (mode == 2 && graph_calls == 1) { /* same PC, wrong guest frame */
        cpu->gpr[29] -= 16; cpu->gpr[31] = 0x80010004u; cpu->pc = 0x80010008u; return;
    }
    if (mode == 3 && graph_calls == 1) { /* a thread switch escapes its C caller */
        cpu->gpr[29] = 0x80123400u; g_psx_call_bail = 1;
        cpu->pc = 0x80020000u; return;
    }
    if (mode == 4 && graph_calls == 1) { /* post-call continuation */
        cpu->pc = 0x80010004u; return;
    }
    if (mode == 5) { cpu->pc = 0x80020000u; g_psx_call_bail = 1; return; }
    if (mode == 6) { /* legitimate task/exception stack straddle */
        cpu->gpr[29] = 0x1f800100u; cpu->pc = cpu->gpr[31]; return;
    }
    if (mode == 1 || mode == 2) cpu->gpr[29] = 0x801ff000u;
    if (mode == 2) assert(addr == 0x80010008u);
    if (mode == 4) { assert(addr == 0x80010004u); ++cpu->gpr[2]; }
    cpu->pc = mode == 3 ? 0u : 0x80010008u;
    cpu->gpr[31] = cpu->pc;
}
static int dirty_ram_dispatch_game_direct(CPUState *cpu, uint32_t addr) {
    hook('N');
    if (mode == 7 || g_psx_dispatch_depth != 1) return 0;
    ++native_calls; graph(cpu, addr); return 1;
}
static int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop) {
    (void)stop; hook('D'); ++fallback_calls; graph(cpu, addr); return 1;
}
static void psx_rfe_escape_check(CPUState *cpu) {
    (void)cpu; if (escape) longjmp(escape_jmp, 1);
}
static void psx_check_interrupts_at(CPUState *cpu, uint32_t pc) {
    (void)pc; if (boundary_redirect) cpu->pc = 0x80030000u;
}
static void psx_check_interrupts(CPUState *cpu) { (void)cpu; }
#define PSX_BLOCK_EDGE(cpu, pc) psx_check_interrupts_at((cpu), (pc))

#include "owner.inc"

static void reset(CPUState *cpu) {
    memset(cpu, 0, sizeof *cpu);
    cpu->gpr[29] = 0x801ff000u; cpu->gpr[31] = 0x80010008u;
    hooks[0] = 0; hook_count = 0;
    tier = mode = graph_calls = native_calls = fallback_calls = trace_calls = 0;
    g_psx_dispatch_depth = g_psx_call_bail = boundary_redirect = escape = 0;
}
int main(void) {
    CPUState cpu;
    const char *expected[] = {"GHSO", "G", "GH", "GHS", "GHSO"};
    for (int t = 1; t <= 4; ++t) {
        reset(&cpu); tier = t;
        psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
        hooks[hook_count] = 0;
        assert(strcmp(hooks, expected[t]) == 0 && graph_calls == 0 && cpu.pc == 0);
    }
    reset(&cpu); mode = 4;
    psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
    assert(graph_calls == 2 && cpu.gpr[2] == 1 && cpu.pc == 0);
#if PSX_NATIVE_STATIC_CALLS && !defined(PSX_BIOS_MODULE_BUILD)
    assert(native_calls == 2 && fallback_calls == 0);
#else
    assert(native_calls == 0 && fallback_calls == 2);
#endif
    reset(&cpu); mode = 1; cpu.gpr[4] = 50000;
    psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
    assert(graph_calls == 50001 && g_psx_dispatch_depth == 0 && cpu.pc == 0);
    reset(&cpu); mode = 2;
    psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
    assert(graph_calls == 2 && cpu.pc == 0);
    reset(&cpu); mode = 3;
    psx_dispatch_impl(&cpu, 0x80010000u, 0);
    assert(graph_calls == 2 && cpu.gpr[29] == 0x80123400u && !g_psx_call_bail);
    reset(&cpu); mode = 5; g_psx_dispatch_depth = 1;
    psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
    assert(graph_calls == 1 && cpu.pc == 0x80020000u && g_psx_call_bail && g_psx_dispatch_depth == 1);
    reset(&cpu); mode = 6;
    psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
    assert(graph_calls == 1 && cpu.pc == 0);
    reset(&cpu); mode = 7;
    psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
    assert(native_calls == 0 && fallback_calls == 1 && cpu.pc == 0);
    reset(&cpu); boundary_redirect = 1;
    psx_dispatch_impl(&cpu, 0x80010000u, 0x80010008u);
    assert(cpu.pc == 0x80030000u && g_psx_dispatch_depth == 0);
    reset(&cpu); escape = 1;
    if (!setjmp(escape_jmp)) { psx_dispatch_impl(&cpu, 0x80010000u, 0); abort(); }
    assert(cpu.pc == 0x80010008u);
    puts("emitted owner hooks, continuations, recursion, thread, stop and IRQ checks passed");
    return 0;
}
