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

#include "overlay_dispatch_preamble.c.inc"

/* Keep the real timed-shim entries, then exercise native span fallbacks in
 * the same DLL namespace too. Neither path may use a DLL-local host identity. */
static uint32_t (*const timed_word)(CPUState*,uint32_t,uint32_t,uint32_t)=psx_cyc_load_word;
static uint16_t (*const timed_half)(CPUState*,uint32_t,uint32_t,uint32_t)=psx_cyc_load_half;
static uint8_t (*const timed_byte)(CPUState*,uint32_t,uint32_t,uint32_t)=psx_cyc_load_byte;
#define PSX_NATIVE_CPU_SPANS 1
#include "psx_native_span.h"

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
