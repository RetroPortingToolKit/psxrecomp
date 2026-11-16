/* Overlay DLLs must update the runtime's store-PC breadcrumb (ABI v24).
 *
 * This TU is an overlay DLL in miniature: it is built with the same defines
 * compile_overlays.py passes, includes the real dispatch preamble, and stores
 * the way generated code does. The "host" hands overlay_init a pointer to its
 * own breadcrumb, like overlay_loader.c does in every build. Before ABI v24 the
 * preamble defined a private g_debug_last_store_pc, so the host value kept
 * naming an older store: fingerprint pc/mmio/sp columns, write attribution and
 * memory.c's PC-keyed store filters all saw the wrong instruction. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* compile_overlays inserts its preamble AFTER generated headers. The memory
 * helpers must exist on this first DLL include without build-only span flags. */
#include "cpu_state.h"
#include "psx_native_span.h"
#include "overlay_dispatch_preamble.c.inc"

/* Keep the real timed-shim entries, then exercise native span fallbacks in
 * the same DLL namespace too. Neither path may use a DLL-local host identity. */
static uint32_t (*const timed_word)(CPUState*,uint32_t,uint32_t,uint32_t)=psx_cyc_load_word;
static uint16_t (*const timed_half)(CPUState*,uint32_t,uint32_t,uint32_t)=psx_cyc_load_half;
static uint8_t (*const timed_byte)(CPUState*,uint32_t,uint32_t,uint32_t)=psx_cyc_load_byte;

/* Verbatim from code_generator.cpp's emitted extern block. Must still compile
 * once cpu_state.h has mapped the name onto the host pointer. */
extern uint32_t g_debug_last_store_pc;  /* exact PC of the executing SW/SH/SB — wtrace/readtrace producer attribution (debug_server.c) */

static uint32_t host_last_store_pc;
static uint32_t host_ram_word;
static uint64_t host_callback_widths[2][3];
static unsigned custom_reads,custom_writes,forward_bus;

static void host_write_word(uint32_t addr, uint32_t value)
{
    (void)addr;
    /* A store's host-visible side effect runs after the breadcrumb is set;
     * memory.c filters read it here. */
    assert(host_last_store_pc == 0x800657FCu);
    host_ram_word = value;
}
static void host_write_half(uint32_t a,uint16_t v) { host_write_word(a,v); }
static void host_write_byte(uint32_t a,uint8_t v) { host_write_word(a,v); }
static uint32_t host_read_word(uint32_t a) { (void)a;return host_ram_word; }
static uint16_t host_read_half(uint32_t a) { return (uint16_t)host_read_word(a); }
static uint8_t host_read_byte(uint32_t a) { return (uint8_t)host_read_word(a); }
static uint32_t custom_word(uint32_t a) { ++custom_reads;return forward_bus?host_read_word(a):0xFEDCBA98u; }
static uint16_t custom_half(uint32_t a) { ++custom_reads;return forward_bus?host_read_half(a):0xBA98u; }
static uint8_t custom_byte(uint32_t a) { ++custom_reads;return forward_bus?host_read_byte(a):0x98u; }
static void custom_store_word(uint32_t a,uint32_t v) { ++custom_writes;if(forward_bus)host_write_word(a,v); }
static void custom_store_half(uint32_t a,uint16_t v) { ++custom_writes;if(forward_bus)host_write_half(a,v); }
static void custom_store_byte(uint32_t a,uint8_t v) { ++custom_writes;if(forward_bus)host_write_byte(a,v); }

/* The actual preamble, with a host-owned live view. Unlike a zero-pending
 * pointer probe, this runs the emitted charge/load/barrier/store sequence. */
static uint8_t direct_ram[PSX_MAIN_RAM_BACKING_BYTES],direct_scratch[1024];
static uint8_t *direct_ram_ptr=direct_ram,*direct_scratch_ptr=direct_scratch;
static uint32_t direct_mask=PSX_MAIN_RAM_RETAIL_BYTES-1u;
static uint32_t direct_lo,direct_hi,direct_parity,direct_sr;
static const uint32_t *direct_sr_ptr=&direct_sr;
static int direct_guards[7]; /* LS,replay,DMA,host,render,read-watch,last-store */
static volatile int direct_recording,direct_card;
static uint64_t direct_stores;
static uint32_t direct_last_addr,direct_last_value,direct_watch[64],direct_exec[64];
static PSXDirectMemoryStats direct_sites;
static PSXMemoryWidthStats direct_widths;
static void (*direct_flush_owner)(void);
static unsigned direct_reads,direct_writes,direct_charges,direct_published;
static unsigned foreign_pending,foreign_published,foreign_expected_current;
static uint8_t *direct_byte_ptr(uint32_t a) {
    uint32_t p=a&0x1FFFFFFFu;
    if(p<0x00800000u)return direct_ram+(p&direct_mask);
    if(p>=0x1F800000u&&p<0x1F800400u)return direct_scratch+(p&1023u);
    return NULL;
}
static uint32_t direct_read(uint32_t a,unsigned width) {
    ++direct_reads;assert(!s_pending_cycles);
    uint32_t v=0;
    for(unsigned i=0;i<width;++i){uint8_t *p=direct_byte_ptr(a+i);if(p)v|=(uint32_t)*p<<(i*8);}
    return v;
}
static uint32_t direct_word(uint32_t a){return direct_read(a,4);}
static uint16_t direct_half(uint32_t a){return (uint16_t)direct_read(a,2);}
static uint8_t direct_byte(uint32_t a){return (uint8_t)direct_read(a,1);}
static void direct_write(uint32_t a,uint32_t v,unsigned width){
    ++direct_writes;assert(!s_pending_cycles);
    for(unsigned i=0;i<width;++i){uint8_t *p=direct_byte_ptr(a+i);if(p)*p=(uint8_t)(v>>(i*8));}
}
static void direct_store_word(uint32_t a,uint32_t v){direct_write(a,v,4);}
static void direct_store_half(uint32_t a,uint16_t v){direct_write(a,v,2);}
static void direct_store_byte(uint32_t a,uint8_t v){direct_write(a,v,1);}
static uint32_t direct_forward(uint32_t a){++custom_reads;return direct_word(a);}
static void direct_forward_store(uint32_t a,uint32_t v){++custom_writes;direct_store_word(a,v);}
static void direct_charge(uint32_t cycles){++direct_charges;direct_published+=cycles;}
static void foreign_flush(void){foreign_published+=foreign_pending;foreign_pending=0;}
static uint32_t foreign_custom_read(uint32_t a){
    (void)a;assert(!s_pending_cycles && !foreign_pending);
    assert(foreign_published==11 && direct_published==foreign_expected_current);
    ++custom_reads;return 0xFEDCBA98u;
}
static void check_direct_memory(CPUState *cpu,OverlayCallbacks *callbacks){
    PSXDirectMemoryView view={0};view.bytes=sizeof view;
    view.native_spans=view.direct_memory=view.direct_stores=1;
    view.g_psx_ram=&direct_ram_ptr;view.g_psx_scratchpad=&direct_scratch_ptr;view.g_psx_ram_mask=&direct_mask;
    view.g_psx_text_write_lo=&direct_lo;view.g_psx_text_write_hi=&direct_hi;view.g_psx_memory_sr_ptr=&direct_sr_ptr;
    view.g_parity_trace_armed=&direct_parity;view.g_card_data_write_armed=&direct_card;
    view.g_ls_mode=&direct_guards[0];view.g_ls_replay_active=&direct_guards[1];
    view.g_dma_exec_depth=&direct_guards[2];view.g_host_store_depth=&direct_guards[3];
    view.g_psx_render_pass_active=&direct_guards[4];view.g_ram_read_watch_active=&direct_guards[5];
    view.g_psx_track_last_store=&direct_guards[6];view.g_ds_recording=&direct_recording;
    view.g_guest_store_count=&direct_stores;view.g_guest_last_store_addr=&direct_last_addr;view.g_guest_last_store_val=&direct_last_value;
    view.g_psx_direct_memory_stats=&direct_sites;view.width_stats=&direct_widths;
    view.g_psx_overlay_watch_bitmap=direct_watch;view.g_dirty_ram_exec_page_bitmap=direct_exec;
    view.g_overlay_flush_pending_cycles=&direct_flush_owner;
    callbacks->direct_memory=&view;callbacks->cpu_charge=direct_charge;
    callbacks->bus_read_word=direct_word;callbacks->bus_read_half=direct_half;callbacks->bus_read_byte=direct_byte;
    callbacks->bus_write_word=direct_store_word;callbacks->bus_write_half=direct_store_half;callbacks->bus_write_byte=direct_store_byte;
    callbacks->bus_callback_widths=host_callback_widths;
    cpu->read_word=direct_word;cpu->read_half=direct_half;cpu->read_byte=direct_byte;
    cpu->write_word=direct_store_word;cpu->write_half=direct_store_half;cpu->write_byte=direct_store_byte;
    direct_flush_owner=overlay_flush_cycles;overlay_init(callbacks);
    assert(PSX_OVERLAY_ABI_VERSION==37 && g_psx_overlay_memory_view==&view);
    const uint32_t aliases[]={0x00120000u,0x80120000u,0xA0320000u,0x1F800100u,0x9F800100u,0xBF800100u};
    for(unsigned i=0;i<6;++i){
        uint8_t *p=direct_byte_ptr(aliases[i]);p[0]=0x44;p[1]=0x33;p[2]=0x22;p[3]=0x11;
        psx_cpu_charge(7); /* real preamble charge, carried through quiet loads */
        assert(timed_word(cpu,aliases[i],2,0)==0x11223344u);
        assert(timed_half(cpu,aliases[i],2,0)==0x3344u && timed_byte(cpu,aliases[i],2,0)==0x44u);
        assert(s_pending_cycles==7 && direct_charges==i && !direct_reads && !direct_writes);
        psx_store_cycle_barrier();PSX_STORE_WORD(cpu,aliases[i],0x11223344u);
        psx_store_cycle_barrier();PSX_STORE_HALF(cpu,aliases[i],0x3344u);
        psx_store_cycle_barrier();PSX_STORE_BYTE(cpu,aliases[i],0x44u);
        assert(!direct_reads && !direct_writes && direct_charges==i+1);
    }
    assert(direct_published==42 && direct_stores==18);
    assert(direct_sites.load_inline_ram==9 && direct_sites.load_inline_scratch==9);
    assert(direct_sites.store_inline_ram==9 && direct_sites.store_inline_scratch==9);
    for(unsigned a=0;a<2;++a)for(unsigned r=0;r<2;++r)for(unsigned w=0;w<3;++w){
#if defined(PSX_DISABLE_HEADROOM_COUNTERS) && PSX_DISABLE_HEADROOM_COUNTERS
        assert(direct_widths.inline_access[a][r][w]==0);
#else
        assert(direct_widths.inline_access[a][r][w]==3);
#endif
    }
    /* Mutable guards are observed AFTER init. Every rejection still publishes
     * pending cycles before the canonical callback, including foreign owners. */
    for(unsigned i=0;i<6;++i){
        direct_guards[i]=1;psx_cpu_charge(3);unsigned before=direct_reads;
        if(i<3||i==5){(void)timed_word(cpu,aliases[0],2,0);assert(direct_reads==before+1 && !s_pending_cycles);}
        psx_store_cycle_barrier();before=direct_writes;
        PSX_STORE_WORD(cpu,aliases[0],0x11223344u);
        assert(direct_writes==before+(i==5?0u:1u));direct_guards[i]=0;
    }
    direct_recording=1;psx_cpu_charge(2);unsigned before=direct_reads;
    (void)timed_byte(cpu,aliases[0],2,0);assert(direct_reads==before+1&&!s_pending_cycles);direct_recording=0;
    uint32_t *page_guards[]={&direct_watch[9],&direct_exec[9],&direct_parity};
    for(unsigned i=0;i<3;++i){*page_guards[i]=1;before=direct_writes;PSX_STORE_WORD(cpu,aliases[0],0x11223344u);assert(direct_writes==before+1);*page_guards[i]=0;}
    direct_card=1;before=direct_writes;PSX_STORE_BYTE(cpu,aliases[0],0x44);assert(direct_writes==before+1);direct_card=0;
    direct_lo=0x120000;direct_hi=0x121000;before=direct_writes;PSX_STORE_HALF(cpu,aliases[0],0x3344);assert(direct_writes==before+1);direct_lo=direct_hi=0;
    direct_sr=0x10000;before=direct_writes;PSX_STORE_WORD(cpu,aliases[0],0x11223344u);assert(direct_writes==before+1);direct_sr=0;
    assert(!psx_direct_store_view(0x80001000,4) && !psx_direct_store_view(0x80120FFF,2));
    assert(!psx_native_span_ram(0x801FFFFE,4) && !psx_native_span_ram(0xBF8003FF,2));
    assert(!psx_native_span_ram(0xC0120000,4) && !psx_direct_store_view(0x1F801000,4));
    direct_flush_owner=foreign_flush;foreign_pending=11;psx_cpu_charge(5);
    foreign_expected_current=direct_published+5;
    cpu->read_word=foreign_custom_read;before=custom_reads;
    assert(timed_word(cpu,aliases[0],2,0)==0xFEDCBA98u && custom_reads==before+1);
    assert(!s_pending_cycles && !foreign_pending && foreign_published==11);
    cpu->read_word=direct_word;direct_flush_owner=overlay_flush_cycles;
    /* A missing emitted store barrier must never make a pending write inline. */
    psx_cpu_charge(11);before=direct_writes;PSX_STORE_WORD(cpu,aliases[0],0x11223344u);
    assert(direct_writes==before+1 && !s_pending_cycles);
    view.direct_stores=0;before=direct_writes;PSX_STORE_WORD(cpu,aliases[0],0x11223344u);
    assert(direct_writes==before+1);view.direct_stores=1;
    view.direct_memory=0;before=direct_reads;(void)timed_byte(cpu,aliases[3],2,0);
    assert(direct_reads==before+1);view.direct_memory=1;
    direct_mask=PSX_MAIN_RAM_EXPANDED_BYTES-1u;direct_ram[0x320000]=0xAB;
    assert(timed_byte(cpu,0xA0320000u,2,0)==0xAB);direct_mask=PSX_MAIN_RAM_RETAIL_BYTES-1u;
    assert(timed_byte(cpu,0xA0320000u,2,0)==0x44);
    /* Private callbacks retain deliberately different values and effects on
     * quiet main RAM too. Forwarding does not become a canonical identity. */
    cpu->read_word=custom_word;cpu->read_half=custom_half;cpu->read_byte=custom_byte;
    cpu->write_word=custom_store_word;cpu->write_half=custom_store_half;cpu->write_byte=custom_store_byte;forward_bus=0;
    unsigned cr=custom_reads,cw=custom_writes;
    assert(timed_word(cpu,aliases[0],2,0)==0xFEDCBA98u && timed_half(cpu,aliases[0],2,0)==0xBA98u && timed_byte(cpu,aliases[0],2,0)==0x98u);
    PSX_STORE_WORD(cpu,aliases[0],0);PSX_STORE_HALF(cpu,aliases[0],0);PSX_STORE_BYTE(cpu,aliases[0],0);
    assert(custom_reads==cr+3 && custom_writes==cw+3 && direct_ram[0x120000]==0x44);
    cpu->read_word=direct_forward;cpu->write_word=direct_forward_store;before=direct_reads;
    assert(timed_word(cpu,aliases[0],2,0)==0x11223344u && direct_reads==before+1);
    before=direct_writes;PSX_STORE_WORD(cpu,aliases[0],0x11223344u);assert(direct_writes==before+1);
    /* Host-OFF is live table policy even if this DLL's build has counters ON. */
    view.width_stats=NULL;memset(&direct_widths,0,sizeof direct_widths);
    cpu->read_word=direct_word;cpu->write_word=direct_store_word;
    psx_cpu_charge(9);assert(timed_word(cpu,aliases[0],2,0)==0x11223344u);psx_store_cycle_barrier();PSX_STORE_WORD(cpu,aliases[0],0x11223344u);
    const PSXMemoryWidthStats zero={0};assert(!memcmp(&zero,&direct_widths,sizeof zero));
    view.native_spans=0;before=direct_reads;assert(timed_word(cpu,aliases[0],2,0)==0x11223344u && direct_reads==before+1);view.native_spans=1;
    --view.bytes;overlay_init(callbacks);assert(!g_psx_overlay_memory_view);++view.bytes;
    callbacks->direct_memory=NULL;overlay_init(callbacks);
    puts("PASS real DLL preamble:18 charged inline loads+18 stores,zero value helpers;6 original store-charge publications;live guards/custom buses/widths/mirrors/edges/counterOFF");
}

static void check_bus_widths(CPUState *cpu,OverlayCallbacks *callbacks) {
    callbacks->bus_read_word=host_read_word;callbacks->bus_read_half=host_read_half;callbacks->bus_read_byte=host_read_byte;
    callbacks->bus_write_word=host_write_word;callbacks->bus_write_half=host_write_half;callbacks->bus_write_byte=host_write_byte;
    callbacks->bus_callback_widths=host_callback_widths;
    overlay_init(callbacks);
    assert((overlay_abi()&65535)==PSX_OVERLAY_ABI_VERSION&&PSX_OVERLAY_ABI_VERSION>=36);
    host_last_store_pc=0x800657FCu;
    cpu->read_word=host_read_word;cpu->read_half=host_read_half;cpu->read_byte=host_read_byte;
    cpu->write_word=host_write_word;cpu->write_half=host_write_half;cpu->write_byte=host_write_byte;
    PSX_STORE_WORD(cpu,0xBF800100u,0x12345678u);
    PSX_STORE_HALF(cpu,0xBF800100u,0x5678u);PSX_STORE_BYTE(cpu,0xBF800100u,0x78u);
    assert(psx_native_span_word(cpu,0xBF800100u,2,0)==0x78u);
    assert(psx_native_span_half(cpu,0xBF800100u,2,0)==0x78u);
    assert(psx_native_span_byte(cpu,0xBF800100u,2,0)==0x78u);
    assert(timed_word(cpu,0xBF800100u,2,0)==0x78u);
    assert(timed_half(cpu,0xBF800100u,2,0)==0x78u);
    assert(timed_byte(cpu,0xBF800100u,2,0)==0x78u);
    for(unsigned a=0;a<2;++a)for(unsigned w=0;w<3;++w)assert(host_callback_widths[a][w]==0);
    cpu->read_word=custom_word;cpu->read_half=custom_half;cpu->read_byte=custom_byte;
    cpu->write_word=custom_store_word;cpu->write_half=custom_store_half;cpu->write_byte=custom_store_byte;
    assert(psx_native_span_word(cpu,0xBF800100u,2,0)==0xFEDCBA98u);
    assert(psx_native_span_half(cpu,0xBF800100u,2,0)==0xBA98u);
    assert(psx_native_span_byte(cpu,0xBF800100u,2,0)==0x98u);
    assert(timed_word(cpu,0xBF800100u,2,0)==0xFEDCBA98u);
    assert(timed_half(cpu,0xBF800100u,2,0)==0xBA98u);
    assert(timed_byte(cpu,0xBF800100u,2,0)==0x98u);
    PSX_STORE_WORD(cpu,0xBF800100u,1);PSX_STORE_HALF(cpu,0xBF800100u,2);PSX_STORE_BYTE(cpu,0xBF800100u,3);
    assert(custom_reads==6&&custom_writes==3&&host_ram_word==0x78u);
#if defined(PSX_DISABLE_HEADROOM_COUNTERS) && PSX_DISABLE_HEADROOM_COUNTERS
    const unsigned counted=0;
#else
    const unsigned counted=1;
#endif
    for(unsigned w=0;w<3;++w){assert(host_callback_widths[0][w]==2u*counted);assert(host_callback_widths[1][w]==counted);}
    forward_bus=1;PSX_STORE_WORD(cpu,0x80120000u,0x76543210u);
    assert(psx_native_span_word(cpu,0x1F801060u,2,0)==0x76543210u);
    assert(host_ram_word==0x76543210u&&custom_reads==7&&custom_writes==4);
    assert(host_callback_widths[0][2]==3u*counted&&host_callback_widths[1][2]==2u*counted);
    /* Counter-OFF HOST supplies NULL even if the DLL has counters compiled in.
     * The exact same callback values/effects still run, without hidden writes. */
    callbacks->bus_callback_widths=0;overlay_init(callbacks);
    for(unsigned a=0;a<2;++a)for(unsigned w=0;w<3;++w)host_callback_widths[a][w]=0;
    PSX_STORE_WORD(cpu,0x80120000u,0x11223344u);
    assert(timed_word(cpu,0x1F801060u,2,0)==0x11223344u&&host_ram_word==0x11223344u);
    for(unsigned a=0;a<2;++a)for(unsigned w=0;w<3;++w)assert(host_callback_widths[a][w]==0);
}

/* Shape of one emitted overlay store (a generated _full_N.c shard). */
static void shard_store(CPUState *cpu)
{
    g_debug_last_store_pc = 0x800657FCu;
    psx_store_cycle_barrier();
    PSX_STORE_WORD(cpu,cpu->gpr[4] + 304,cpu->gpr[10]);
}

int main(void)
{
    CPUState cpu = {0};
    cpu.write_word = host_write_word;
    cpu.gpr[10] = 0x12345678u;

    /* A store before overlay_init lands in the DLL's private copy. */
    g_debug_last_store_pc = 0x80010000u;
    assert(host_last_store_pc == 0u);

    OverlayCallbacks callbacks = {0};
    callbacks.last_store_pc = &host_last_store_pc;
    overlay_init(&callbacks);
    assert((overlay_abi() & 0xFFFF) >= 24);

    host_last_store_pc = 0xBFC04E90u;  /* last static/BIOS store */
    shard_store(&cpu);
    assert(host_last_store_pc == 0x800657FCu);
    assert(host_ram_word == 0x12345678u);
    assert(g_debug_last_store_pc == 0x800657FCu);
    check_bus_widths(&cpu,&callbacks);
    check_direct_memory(&cpu,&callbacks);

    /* A host that supplies no pointer gets the pre-v24 behaviour back: the
     * DLL keeps its own copy and never touches the host's. */
    callbacks.last_store_pc = 0;
    overlay_init(&callbacks);
    host_last_store_pc = 0xBFC04E90u;
    g_debug_last_store_pc = 0x80065800u;
    assert(host_last_store_pc == 0xBFC04E90u);
    assert(g_debug_last_store_pc == 0x80065800u);
    return 0;
}
