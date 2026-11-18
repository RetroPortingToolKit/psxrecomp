/* Exercise the production CRC gate beyond cache capacity, then change live
 * code and restore it. A bounded replacement cache must never inherit a
 * different occupant's successful page-generation result. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "crc32.h"
#include "overlay_loader.h"
#include "psx_memory.h"

uint32_t g_psx_ram_size=0x200000, g_psx_ram_mask=0x1fffff;
static uint8_t ram[0x200000];
static uint32_t generation;
static uint32_t ranges[65536][2];
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t overlay_watch_pagegen_sum(uint32_t p,uint32_t n) { (void)p;(void)n;return generation; }
void overlay_watch_set_range(uint32_t p,uint32_t n) { (void)p;(void)n; }
extern void overlay_loader_static_match_stats(uint64_t*,uint64_t*,uint64_t*);
extern int psx_overlay_static_code_matches(const uint32_t*,uint32_t,uint32_t);
extern uint32_t psx_overlay_resident_crc_at(uint32_t,int*);
extern uint64_t overlay_loader_resident_crc_scans(void);

int main(void) {
    const uint32_t good=crc32_update(0xffffffff,ram+0x10000,4)^0xffffffff;
    for(unsigned i=0;i<65536;i++) {
        ranges[i][0]=0x80010000; ranges[i][1]=4;
        assert(psx_overlay_static_code_matches(ranges[i],1,good));
        /* Cache negative occupants too; identical generations don't make
         * these mismatching fingerprints safe to execute. */
        assert(!psx_overlay_static_code_matches(ranges[i],1,good^1));
    }
    const uint32_t *hot=ranges[65535];
    assert(psx_overlay_static_code_matches(hot,1,good));
    uint64_t a,b,c,prev;
    overlay_loader_static_match_stats(&a,&b,&prev);
    for(unsigned i=0;i<10000;i++)
        assert(psx_overlay_static_code_matches(hot,1,good));
    overlay_loader_static_match_stats(&a,&b,&c);
    assert(c-prev==10000); /* A result remains cached after saturation. */
    ram[0x10000]=1; generation++;
    assert(!psx_overlay_static_code_matches(hot,1,good));
    ram[0x10000]=0; generation++;
    assert(psx_overlay_static_code_matches(hot,1,good));
    const uint32_t bad[]={0x801fffff,4};
    assert(!psx_overlay_static_code_matches(bad,1,good));
    assert(!psx_overlay_static_code_matches(ranges[0],0,good));
    /* Occupant fingerprint: inside the cached ranges it names the variant;
     * outside every range ever cached it answers "none" without scanning
     * the cache, with the same result the full scan gives. */
    int valid=-1;
    assert(psx_overlay_resident_crc_at(0x10000,&valid)==good && valid==1);
    valid=-1;
    const uint64_t scans=overlay_loader_resident_crc_scans();
    const uint32_t none=psx_overlay_resident_crc_at(0x150000,&valid);
    assert(valid==0);
    assert(overlay_loader_resident_crc_scans()==scans); /* no 16,384-entry scan */
    valid=-1;
    assert(psx_overlay_resident_crc_at(0x8000,&valid)==none && valid==0);
    puts("static cache saturation, eviction and code replacement passed");
    return 0;
}
