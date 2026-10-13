#define PSX_OVERLAY_DLL_BUILD 1
#include "overlay_dispatch_preamble.c.inc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t charged, calls;
static CPUState* expected_cpu;
static void charge(uint32_t n) { charged += n; }
static uint32_t redirect(const CPUState* cpu, uint32_t pc, uint32_t word, uint32_t address) {
    CHECK(cpu == expected_cpu && pc == 0x80010000u && word == 0x8ca30014u);
    CHECK(charged == 7u); /* Pending DLL cycles publish before the host callback. */
    ++calls;
    return address + 0x1000u;
}
int main(void) {
    CPUState cpu = {0}; expected_cpu = &cpu;
    OverlayCallbacks callbacks = {0}; callbacks.cpu_charge = charge;
    callbacks.mod_memory_address = redirect;
    overlay_init(&callbacks);
    psx_cpu_charge(7);
    CHECK(psx_mod_memory_address(&cpu, 0x80010000u, 0x8ca30014u, 0x80002003u) == 0x80003003u);
    CHECK(calls == 1 && charged == 7);
    overlay_flush_cycles(); CHECK(charged == 7);
    callbacks.mod_memory_address = NULL;
    overlay_init(&callbacks);
    CHECK(psx_mod_memory_address(&cpu, 0x80010000u, 0x8ca30014u, 0x80002003u) == 0x80002003u);
    CHECK(calls == 1 && overlay_abi() == PSX_OVERLAY_ABI_TAG);
    puts("overlay address callback and cycle publication: passed");
    return 0;
}
