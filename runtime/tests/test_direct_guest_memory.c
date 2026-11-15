/* Actual memory.c plus its real page registrations, card capture and parity
 * writer provenance. The Python runner supplies only unrelated device seams. */
#include "harness_common.h"
#include "psx_native_span.h"
#include "card_data_writes.h"
#include "parity_trace.h"
#include "psx_icache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *memory_get_ram_ptr(void);
uint8_t *memory_get_scratchpad_ptr(void);
void memory_set_sr_ptr(const uint32_t *p);
uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint8_t psx_read_byte(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t value);
void psx_write_half(uint32_t addr, uint16_t value);
void psx_write_byte(uint32_t addr, uint8_t value);
extern unsigned g_fixture_isc;
void overlay_watch_set_range(uint32_t phys,uint32_t len);
void overlay_watch_clear_range(uint32_t phys,uint32_t len);
uint32_t overlay_watch_pagegen_sum(uint32_t phys,uint32_t len);

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#if PSX_DIRECT_GUEST_MEMORY && defined(PSX_NO_DEBUG_TOOLS)
#define FAST 1
#else
#define FAST 0
#endif

static unsigned reads, custom_writes, flushes;
static unsigned helper_writes(void) {
    PSXDirectMemoryStats *s=&g_psx_direct_memory_stats;
    return (unsigned)(s->store_helper_ram+s->store_helper_scratch+s->store_helper_mmio+s->store_helper_other);
}
static uint32_t read_word(uint32_t a) { ++reads; return psx_read_word(a); }
static uint16_t read_half(uint32_t a) { ++reads; return psx_read_half(a); }
static uint8_t read_byte(uint32_t a) { ++reads; return psx_read_byte(a); }
static void write_word(uint32_t a,uint32_t v) { (void)a; (void)v; ++custom_writes; }
static void flush(void) { ++flushes; }
uint32_t parity_host_frame(void) { return 7; }
uint64_t parity_host_cycle(void) { return 123; }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }

static void require_helper(CPUState *cpu, uint32_t addr) {
    unsigned n = helper_writes();
    PSX_STORE_WORD(cpu, addr, 0xAABBCCDDu);
    CHECK(helper_writes() == n + 1);
}

int main(void) {
    CPUState cpu = {0};
    cpu.read_word=read_word; cpu.read_half=read_half; cpu.read_byte=read_byte;
    cpu.write_word=psx_write_word; cpu.write_half=psx_write_half; cpu.write_byte=psx_write_byte;
    uint8_t *ram=memory_get_ram_ptr(), *scratch=memory_get_scratchpad_ptr();
    uint32_t sr=0;
    memory_set_sr_ptr(&sr);

    PSX_STORE_WORD(&cpu, 0x80020000u, 0x78563412u);
    PSX_STORE_HALF(&cpu, 0xA0220004u, 0xBC9Au);
    PSX_STORE_BYTE(&cpu, 0x00620006u, 0xDEu);
    CHECK(ram[0x20000]==0x12 && ram[0x20003]==0x78 && ram[0x20006]==0xDE);
    CHECK(helper_writes() == (FAST ? 0u : 3u));
    CHECK(psx_cyc_load_word(&cpu,0x80020000u,2,0)==0x78563412u);
    CHECK(psx_cyc_load_half(&cpu,0xA0220004u,2,0)==0xBC9Au);
    CHECK(psx_cyc_load_byte(&cpu,0x00620006u,2,0)==0xDEu && reads==0);

    unsigned n=helper_writes();
    PSX_STORE_WORD(&cpu,0x9F800040u,0x10203040u);
    PSX_STORE_HALF(&cpu,0xBF800044u,0x5060u);
    PSX_STORE_BYTE(&cpu,0x1F800046u,0x70u);
    CHECK(helper_writes() == n + (FAST ? 0u : 3u));
    n=reads;
    CHECK(psx_cyc_load_word(&cpu,0xBF800040u,2,0)==0x10203040u);
    CHECK(psx_cyc_load_half(&cpu,0x1F800044u,2,0)==0x5060u);
    CHECK(psx_cyc_load_byte(&cpu,0x9F800046u,2,0)==0x70u);
    CHECK(reads == n + (PSX_DIRECT_GUEST_MEMORY ? 0u : 3u));
    CHECK(g_guest_store_count==6u);

    // Store-count and final-store metadata are still the idle-loop contract.
    g_psx_track_last_store=1;
    PSX_STORE_WORD(&cpu,0xA0020100u,0x12345678u);
    CHECK(g_guest_last_store_addr==0xA0020100u && g_guest_last_store_val==0x12345678u);
    PSX_STORE_HALF(&cpu,0x80020104u,1);
    CHECK(g_guest_last_store_addr==UINT32_MAX);
    g_psx_track_last_store=0;

    // 8 MiB mode has unique storage; retail aliases must not persist in it.
    g_psx_ram_mask=PSX_MAIN_RAM_EXPANDED_BYTES-1u;
    g_psx_ram_size=PSX_MAIN_RAM_EXPANDED_BYTES;
    PSX_STORE_WORD(&cpu,0x80220000u,0xDEADBEEFu);
    CHECK(psx_cyc_load_word(&cpu,0x80220000u,2,0)==0xDEADBEEFu);
    CHECK(psx_cyc_load_word(&cpu,0x80020000u,2,0)==0x78563412u);
    g_psx_ram_mask=PSX_MAIN_RAM_RETAIL_BYTES-1u;
    g_psx_ram_size=PSX_MAIN_RAM_RETAIL_BYTES;

    // Dynamic observer ownership uses the maintained flags, not a boot guess.
    int *guards[]={&g_ls_mode,&g_ls_replay_active,&g_dma_exec_depth,&g_host_store_depth};
    for(unsigned i=0;i<sizeof guards/sizeof guards[0];++i) {
        *guards[i]=1; require_helper(&cpu,0x80020200u); *guards[i]=0;
    }
    g_ds_recording=1; require_helper(&cpu,0x80020200u); g_ds_recording=0;
    g_overlay_flush_pending_cycles=flush;
    require_helper(&cpu,0x80020200u);
    CHECK(flushes==1);
    g_overlay_flush_pending_cycles=NULL;
    g_ls_mode=2;
    uint8_t before=ram[0x20200];
    require_helper(&cpu,0x80020200u);
    CHECK(ram[0x20200]==before);
    g_ls_mode=0;
    g_psx_render_pass_active=1;
    CHECK(psx_direct_store_view(0x80020200u,4)==NULL);
    g_psx_render_pass_active=0;

    int *read_guards[]={&g_ls_mode,&g_ls_replay_active,&g_dma_exec_depth,&g_ram_read_watch_active};
    for(unsigned i=0;i<sizeof read_guards/sizeof read_guards[0];++i) {
        *read_guards[i]=1; n=reads;
        CHECK(psx_cyc_load_word(&cpu,0x1F800040u,2,0)==0x10203040u);
        CHECK(reads==n+1); *read_guards[i]=0;
    }
    g_ds_recording=1; n=reads;
    CHECK(psx_cyc_load_word(&cpu,0x1F800040u,2,0)==0x10203040u && reads==n+1);
    g_ds_recording=0;

    // IsC is owned by the memory system's actual SR pointer. The original
    // cache write reaches scratchpad and never the addressed main RAM.
    psx_write_word(0xFFFE0130u,PSX_BIU_DCACHE_ENABLE);
    sr=0x10000u; before=ram[0x20100];
    require_helper(&cpu,0x80020100u);
    CHECK(g_fixture_isc==1u && ram[0x20100]==before && scratch[0x100]==0xDD);
    sr=0;
    n=helper_writes(); PSX_STORE_WORD(&cpu,0xC0020000u,0);
    CHECK(helper_writes()==n+1 && ram[0x20000]==0x12);
    require_helper(&cpu,0x1F801100u);
    CHECK(psx_direct_store_view(0x001FFFFEu,4)==NULL);
    CHECK(psx_direct_store_view(0x1F8003FFu,2)==NULL);
    CHECK(psx_native_span_ram(0x1F8003FFu,2)==NULL);
    CHECK(psx_direct_store_view(0x8002FFFFu,4)==NULL);

    // Custom bus callbacks (including readonly host-output views) remain
    // authoritative even for addresses otherwise eligible for an inline store.
    cpu.write_word=write_word; before=ram[0x20340u];
    PSX_STORE_WORD(&cpu,0x80020340u,0x99u);
    CHECK(custom_writes==1 && ram[0x20340u]==before);
    cpu.write_word=psx_write_word;

    // The real card capture remains late-join history, and is rechecked at
    // every store while pending, including when its destination is data RAM.
    card_data_writes_reset();
    card_data_writes_arm(0x55u,12,13,0);
    require_helper(&cpu,0x80020300u); // unrelated low byte must not disarm it
    CHECK(g_card_data_write_armed);
    PSX_STORE_BYTE(&cpu,0x80020304u,0x55u);
    const CardDataWriteEntry *card; uint64_t total; uint32_t head;
    CHECK(card_data_writes_get(&card,&total,&head)==1 && total==1 && head==1);
    CHECK(card[0].addr==0x20304u && card[0].value==0x55u && !g_card_data_write_armed);

    // Preserve real parity writer attribution through an active watch.
    uint32_t watch=0x80020310u;
    parity_trace_config(0,0,0x88,0,&watch,1); parity_trace_arm(1);
    g_debug_last_store_pc=0x80012340u;
    require_helper(&cpu,watch);
    parity_trace_note_read(watch,0xAABBCCDDu,0x80012380u);
    ParityEntry row;
    CHECK(parity_trace_reads_get(&row,1)==1 && row.watch_wpc[0]==g_debug_last_store_pc);
    parity_trace_arm(0);
    g_debug_last_store_pc=0x00001CE4u;
    require_helper(&cpu,0x80020320u); // keyed BIOS card-event writer stays canonical
    g_debug_last_store_pc=0;

    // Kernel install/patch stores still invalidate their executable page.
    dirty_ram_reset_for_boot();
    CHECK(!dirty_ram_is_dirty(0x5000u));
    require_helper(&cpu,0x80005000u);
    CHECK(dirty_ram_is_dirty(0x5000u));

    // A late overlay watch and unknown first-execution witness cannot be
    // bypassed by a data-page eligibility decision made before registration.
    overlay_watch_set_range(0x40000u,4096);
    uint32_t generation=overlay_watch_pagegen_sum(0x40000u,4096);
    require_helper(&cpu,0x80040000u);
    CHECK(overlay_watch_pagegen_sum(0x40000u,4096)==generation+1u);
    overlay_watch_clear_range(0x40000u,4096);
    if(FAST) CHECK(psx_direct_store_view(0x80040000u,4)==ram+0x40000u);
    g_dirty_ram_exec_page_bitmap[0x50u>>5]|=1u<<(0x50u&31u);
    g_dirty_ram_exec_pc_bitmap[0x50000u/4u/32u]=1;
    require_helper(&cpu,0x80050000u);
    CHECK(!(g_dirty_ram_exec_page_bitmap[0x50u>>5]&(1u<<(0x50u&31u))));
    CHECK(!g_dirty_ram_exec_pc_bitmap[0x50000u/4u/32u]);

    // CMR2-class self-modification: the actual registered text guard rejects
    // changed bytes, then re-derives eligibility after restoring prior RAM.
    static uint8_t reference[16]={1,2,3,4};
    memcpy(ram+0x60000u,reference,sizeof reference);
    dirty_ram_register_text_image(0x60000u,reference,sizeof reference);
    CHECK(dirty_ram_text_native_ok(0x60000u));
    require_helper(&cpu,0x80060000u);
    CHECK(!dirty_ram_text_native_ok(0x60000u));
    memcpy(ram+0x60000u,reference,sizeof reference);
    dirty_ram_text_guard_resync_after_restore();
    CHECK(dirty_ram_text_native_ok(0x60000u));

    PSXDirectMemoryStats stats; psx_direct_memory_get_stats(&stats);
    CHECK(stats.load_inline_ram>=3 && stats.store_helper_ram>0);
    CHECK((stats.store_inline_ram>0)==FAST && (stats.store_inline_scratch>0)==FAST);
    CHECK((stats.load_inline_scratch>0)==!!PSX_DIRECT_GUEST_MEMORY);
    printf("PASS: direct=%d fast=%d bytes, aliasing, IsC, card/parity, kernel, overlay, text and restore\n",PSX_DIRECT_GUEST_MEMORY,FAST);
    return 0;
}
