/* Production SYS01/02 entry only; no BIOS, game data, or timing stubs.
 * LTO discards unrelated scheduler branches for the constant selectors. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "../src/traps.c"

static int nested;
int psx_get_in_exception(void) { return nested; }
int source_gpu_runtime_active(void) { return 0; }

static inline __attribute__((always_inline)) void check(unsigned func, unsigned sr, int enabled) {
    CPUState cpu = {0}, expected;
    for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x12340000u + i;
    cpu.gpr[4] = func;
    cpu.pc = 0x80012340u;
    cpu.cop0[12] = sr;
    cpu.cop0[13] = 0x8000057Cu;
    cpu.cop0[14] = 0x80099990u;
    cpu.hi = 0xAABBCCDDu; cpu.lo = 0xEEFF0011u;
    expected = cpu;
    if (enabled) {
        expected.cop0[14] = cpu.pc;
        expected.cop0[13] = 0x520u;
        expected.cop0[12] = (sr & ~0x3Fu) | ((sr & 15u) << 2);
        expected.pc = sr & 0x400000u ? 0xBFC00180u : 0x80000080u;
    } else {
        expected.cop0[12] = func == 1 ? sr & ~1u : sr | 0x401u;
        if (func == 1) expected.gpr[2] = sr & 1u;
        expected.pc = 0;
    }
    int continuation = 0;
    if (!psx_syscall(&cpu, 0)) continuation = 1;
    assert(continuation == !enabled);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
}

int main(int argc, char** argv) {
    int enabled = argc > 1 && strcmp(argv[1], "exception") == 0;
#ifdef _WIN32
    if (enabled) _putenv("PSX_CRITICAL_SECTION_MODEL=exception");
    else _putenv("PSX_CRITICAL_SECTION_MODEL=");
#else
    if (enabled) setenv("PSX_CRITICAL_SECTION_MODEL", "exception", 1);
    else unsetenv("PSX_CRITICAL_SECTION_MODEL");
#endif
    nested = argc > 2;
    for (unsigned i = 0; i < 64; ++i) {
        check(1, 0x40000000u | i, enabled);
        check(2, 0x40400400u | i, enabled);
    }
    puts("critical_exception: PASS (128 entry/continuation states)");
    return 0;
}
