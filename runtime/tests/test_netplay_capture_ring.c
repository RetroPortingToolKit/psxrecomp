/* Real writer/loader + PSX netplay adapter + rbengine ownership. Shared
 * deterministic device doubles; no transport or two-peer claim. */
#include <stdlib.h>
#include <assert.h>
static int fail_allocation;
static unsigned allocation_calls;
static void *capture_malloc(size_t n) {
    ++allocation_calls;
    if (fail_allocation) { fail_allocation=0; return NULL; }
    return malloc(n);
}
static void *capture_realloc(void *p,size_t n) {
    ++allocation_calls;
    if (fail_allocation) { fail_allocation=0; return NULL; }
    return realloc(p,n);
}
#define malloc capture_malloc
#define realloc capture_realloc
#define main handoff_main
#define PSX_CAPTURE_WRITER_ONLY 1
#include "test_boot_state_handoff.c"
#undef main
#undef malloc
#undef realloc
#include "netplay_snap_ring.h"
int psx_netplay_local_slot(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }

int main(void) {
    CPUState cpu={0};
    NetplaySnapRing *ring=netplay_snap_ring_create(3);
    assert(ring);
    ram_bytes=2*1024*1024;arena_bytes=0;pending_arena_bytes=-1;
    vram_tracking=1;boot_state_vram_mirror_reset();
    cpu.pc=0x80050044;fntrace_restore_game_started(1);
    for(unsigned tick=1;tick<=7;++tick) {
        unsigned before=allocation_calls;
        cpu.gpr[4]=tick;ram[0]=(uint8_t)tick;psx_cycle_count=tick*12345;
        assert(netplay_snap_ring_save(ring,tick,&cpu,123,0x801b0000));
        assert(netplay_snap_ring_count(ring)==(tick<3?tick:3));
#if PSX_NATIVE_CAPTURE_JOBS
        if(tick>4)assert(allocation_calls==before);
#endif
    }
    assert(!netplay_snap_ring_has(ring,4) && netplay_snap_ring_has(ring,5));
    cpu.gpr[4]=77;ram[0]=77;
    assert(netplay_snap_ring_save(ring,7,&cpu,123,0x801b0000));
    assert(netplay_snap_ring_count(ring)==3);
    /* Failed capture preserves all visible history. Expanded RAM forces an
     * actual native spare growth allocation rather than an artificial seam. */
    size_t old_len;const uint8_t *old=netplay_snap_ring_peek(ring,7,&old_len);
    uint8_t *copy=malloc(old_len);assert(copy);memcpy(copy,old,old_len);
    ram_bytes=sizeof ram;fail_allocation=1;
    assert(!netplay_snap_ring_save(ring,7,&cpu,123,0x801b0000));
    assert(!fail_allocation && netplay_snap_ring_count(ring)==3);
    size_t len;const uint8_t *data=netplay_snap_ring_peek(ring,7,&len);
    assert(data==old && len==old_len && !memcmp(data,copy,len));free(copy);
    assert(netplay_snap_ring_has(ring,5) && netplay_snap_ring_has(ring,6));
    ram_bytes=2*1024*1024;ram[0]=0;cpu.gpr[4]=0;
    assert(netplay_snap_ring_load(ring,7,&cpu,123,0x801b0000));
    assert(cpu.gpr[4]==77 && ram[0]==77);
    assert(!netplay_snap_ring_load(ring,99,&cpu,123,0x801b0000));
    ram_bytes=sizeof ram;cpu.gpr[4]=88;ram[ram_bytes-1]=0x59;
    assert(netplay_snap_ring_save(ring,8,&cpu,123,0x801b0000));
    ram[ram_bytes-1]=0;cpu.gpr[4]=0;
    assert(netplay_snap_ring_load(ring,8,&cpu,123,0x801b0000));
    assert(cpu.gpr[4]==88 && ram[ram_bytes-1]==0x59);
    assert(!netplay_snap_ring_has(ring,5));
    netplay_snap_ring_drop_after(ring,7);
    assert(netplay_snap_ring_count(ring)==2 && !netplay_snap_ring_has(ring,8));
    netplay_snap_ring_clear(ring);assert(netplay_snap_ring_count(ring)==0);
    assert(netplay_snap_ring_save(ring,9,&cpu,123,0x801b0000));
    netplay_snap_ring_destroy(ring);
    puts("actual netplay capture/save/load/overwrite/failure/drop/clear passed");
    return 0;
}
