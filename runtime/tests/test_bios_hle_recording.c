#ifdef NDEBUG
#undef NDEBUG
#endif
#include "bios_hle.h"
#include "cpu_state.h"
#include "psx_bios_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

PsxBiosImageInfo psx_bios_image = {.shell_entry_phys=0x30000u};
uint64_t psx_cycle_count=100;
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
static unsigned reads;
static uint32_t read_word(uint32_t address) {
    assert(address==0xA0000108u);
    ++reads;
    return 0; /* no running PCB: only the first forensic attribution read */
}
int psx_get_in_exception(void) { return 0; }
int fntrace_is_game_started(void) { return 1; }

int main(void) {
    CPUState cpu={0};
    cpu.read_word=read_word;
    cpu.gpr[9]=0x17u; cpu.gpr[31]=0x80012340u;
    CPUState before=cpu;
    psx_bios_hle_configure(0,1);
    assert(g_psx_bios_hle_hook);
    assert(!g_psx_bios_hle_hook(&cpu,0xA0u));
    assert(!g_psx_bios_hle_hook(&cpu,0xA0u));
    assert(!memcmp(&before,&cpu,sizeof(cpu)) && psx_cycle_count==100);
#if EXPECT_RECORDING
    assert(reads==2 && psx_hle_ring_seq()==1);
    const PsxHleCallEntry *entry=psx_hle_ring_entry(0);
    assert(entry && entry->repeat==2 && entry->route==PSX_HLE_ROUTE_LLE);
#else
    assert(!reads && !psx_hle_ring_seq() && !psx_hle_ring_entry(0));
#endif
    psx_bios_hle_configure(0,0);
    assert(!g_psx_bios_hle_hook);
    puts("PASS BIOS recording: caller state/time preserved; diagnostic reads only in debug builds");
    return 0;
}
