/* A replay records each native unit with the arch-abi folder and library
 * extension of the host that recorded it. Another host must look for its
 * own build of the same unit: same tier, codegen tag, segment, address and
 * hash, its own folder and extension. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "overlay_loader.h"

uint32_t g_psx_ram_size=0x200000, g_psx_ram_mask=0x1fffff;
static uint8_t ram[0x200000];
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t overlay_watch_pagegen_sum(uint32_t p,uint32_t n) { (void)p;(void)n;return 0; }
void overlay_watch_set_range(uint32_t p,uint32_t n) { (void)p;(void)n; }

#ifdef _WIN32
#  define EXT ".dll"
#else
#  define EXT ".so"
#endif

static void local(const char *in, const char *want) {
    char out[768];
    assert(overlay_loader_unit_local(in, out, sizeof(out)));
    if (strcmp(out, want)) {
        fprintf(stderr, "%s -> %s, want %s\n", in, out, want);
        assert(0);
    }
}

int main(void) {
    const char *abi = PSX_OVERLAY_ARCH_ABI;
    char want[768], small[8];
    /* Each host's spelling of one unit becomes this host's. */
    snprintf(want, sizeof(want), "gcc/%s/cg18_68c1a5c3_gcc15c8ed0_f0/00021000_7B536661" EXT, abi);
    local("gcc/win-x64/cg18_68c1a5c3_gcc15c8ed0_f0/00021000_7B536661.dll", want);
    local("gcc/linux-x64/cg18_68c1a5c3_gcc15c8ed0_f0/00021000_7B536661.so", want);
    local("gcc/macos-arm64/cg18_68c1a5c3_gcc15c8ed0_f0/00021000_7B536661.so", want);
    local(want, want);
    /* A segment folder is kept. */
    snprintf(want, sizeof(want), "gcc/%s/cg18_68c1a5c3_gcc15c8ed0_f0/seg-kuseg/00002000_4EE95325" EXT, abi);
    local("gcc/win-x64/cg18_68c1a5c3_gcc15c8ed0_f0/seg-kuseg/00002000_4EE95325.dll", want);
    /* Names that do not have the cache layout are left alone. */
    local("gcc/x64/cg18/00021000_7B536661.dll", "gcc/x64/cg18/00021000_7B536661.dll");
    local("gcc/win-/cg18/00021000_7B536661.dll", "gcc/win-/cg18/00021000_7B536661.dll");
    local("gcc/win-x64/cg18.1/00021000_7B536661", "gcc/win-x64/cg18.1/00021000_7B536661");
    local("gcc/win-x64/cg18/00021000_7B536661.ranges", "gcc/win-x64/cg18/00021000_7B536661.ranges");
    local("00021000_7B536661.dll", "00021000_7B536661.dll");
    /* A result that does not fit is refused, not cut short. */
    assert(!overlay_loader_unit_local("gcc/win-x64/cg18/00021000_7B536661.dll", small, sizeof(small)));
    assert(!overlay_loader_unit_local(NULL, want, sizeof(want)));
    printf("unit names translate to %s\n", abi);
    return 0;
}
