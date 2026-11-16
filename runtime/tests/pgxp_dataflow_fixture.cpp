#include "cpu_state.h"
#include "pgxp.h"
#include "pgxp_hooks.h"
#include "psx_direct_memory.h"
#include "psx_cycles.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void flow_reference(CPUState*);
extern "C" void flow_candidate(CPUState*);
extern "C" void flow_reference_func_80010300(CPUState*);
extern "C" void flow_candidate_func_80010300(CPUState*);
extern "C" void flow_reference_func_80010400(CPUState*);
extern "C" void flow_candidate_func_80010400(CPUState*);
static unsigned char ram[0x200000],scratch[1024];
extern "C" {
uint8_t *g_psx_ram=ram,*g_psx_scratchpad=scratch;
PSXDirectMemoryStats g_psx_direct_memory_stats;
PSXMemoryWidthStats g_psx_memory_width_stats;
uint32_t g_debug_last_store_pc,g_debug_current_func_addr;
volatile uint32_t g_psx_last_fn_entry; // Release's real entry observer writes this slot
uint64_t psx_cycle_count,psx_next_service_cycle=UINT64_MAX;
int g_ls_mode,g_ls_replay_active,g_ram_read_watch_active,g_dma_exec_depth;
volatile int g_ds_recording;
int g_host_store_depth,g_psx_render_pass_active;
uint32_t g_psx_text_write_lo,g_psx_text_write_hi,g_parity_trace_armed;
volatile int g_card_data_write_armed;
const uint32_t *g_psx_memory_sr_ptr;
uint32_t g_psx_overlay_watch_bitmap[64],g_dirty_ram_exec_page_bitmap[64];
void (*g_overlay_flush_pending_cycles)(void);
uint64_t g_guest_store_count;
int g_psx_track_last_store;
uint32_t g_guest_last_store_addr,g_guest_last_store_val;
void debug_server_trace_write_check(uint32_t,uint32_t,uint32_t,uint8_t) {}
void psx_direct_precision_write(uint32_t a,uint32_t,uint32_t v,uint8_t width) {pgxp_write_owned(a,v,width);}
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit,g_psx_gcs_frac,g_psx_gcs_recip_q16=65536;
uint32_t *g_psx_cyc_local_acc;
int g_event_step_conservative,psx_in_device_service,g_psx_cyc_bb_defer,g_psx_load_delay;
int gte_geometry_correction_lookup(uint32_t,int32_t*,int32_t*) {return 0;}
int gte_geometry_correction_lookup_probe(uint32_t,int32_t*,int32_t*) {return 0;}
void psx_check_interrupts_at(CPUState*,uint32_t) {}
int psx_mod_try_function_replacement(CPUState*,uint32_t) {return 0;}
uint32_t psx_read_word(uint32_t a) {uint32_t v;std::memcpy(&v,ram+(a&0x1FFFFC),4);return v;}
uint16_t psx_read_half(uint32_t a) {uint16_t v;std::memcpy(&v,ram+(a&0x1FFFFE),2);return v;}
uint8_t psx_read_byte(uint32_t a) {return ram[a&0x1FFFFF];}
void psx_write_word(uint32_t a,uint32_t v) {pgxp_write_owned(a,v,4);std::memcpy(ram+(a&0x1FFFFC),&v,4);}
void psx_write_half(uint32_t a,uint16_t v) {pgxp_write_owned(a,v,2);std::memcpy(ram+(a&0x1FFFFE),&v,2);}
void psx_write_byte(uint32_t a,uint8_t v) {pgxp_write_owned(a,v,1);ram[a&0x1FFFFF]=v;}
}
constexpr uint32_t Input=0x80001000,Plain=0x80002000,Output=0x80003000,Probe=0x80004000;
struct Product {CPUState cpu;unsigned char bytes[sizeof ram];PGXPWordShadow words[71];uint64_t sources[71],alu,cycles,operations,operation_loads,callbacks;};
static Product reference,candidate;
static uint64_t callback_reads;
static uint32_t substituted_reader(uint32_t address) {++callback_reads;return psx_read_word(address)+(address==Plain?1u:0u);}

static bool equal(const PGXPWordShadow&a,const PGXPWordShadow&b) {
    return a.address==b.address&&a.value==b.value&&a.valid==b.valid&&a.flags==b.flags&&a.x16==b.x16&&
        a.y16==b.y16&&a.z==b.z&&!std::memcmp(&a.projection,&b.projection,sizeof a.projection);
}
static void run(Product& result,bool native,unsigned variant) {
    auto& cpu=result.cpu;std::memset(&cpu,0,sizeof cpu);std::memset(ram,0,sizeof ram);
    cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
    cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
    for(unsigned r=1;r<32;++r)cpu.gpr[r]=0x53100000u+0x10203u*r;
    const uint32_t out=(variant&1)?Input:Output,packed=0x000C0008u;
    cpu.gpr[4]=out;cpu.gpr[5]=(variant&2)?(Input^0x20000000u):Input;
    cpu.gpr[6]=(variant&4)?1:2;cpu.gpr[7]=Plain;cpu.gpr[31]=0x80009000;
    pgxp_set_enabled(1);pgxp_set_cpu_mode(1);pgxp_set_source_tracking(1);pgxp_set_projection_tracking(1);
    pgxp_set_position_fallback(0);pgxp_invalidate_all();
    psx_write_word(Input,packed);psx_write_word(Plain,packed);
    pgxp_gte_push_sxy((8<<16)+16384,(12<<16)+32768,300,packed);
    PGXPProjection projection{8.25f,12.5f,2.f,.5f};pgxp_gte_set_projection(&projection);
    pgxp_store_gte_reg(Input,14);pgxp_tag_word_source(Input,packed,101);
    PGXPNativeWord receipt{};uint32_t addr=Input;
    if(!pgxp_native_capture_words(&addr,&packed,&receipt,1)||!pgxp_native_import_sxy(14,&receipt))std::abort();
    cpu.gte_data[14]=packed;cpu.gte_data[15]=packed;
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS
    if(variant&64){pgxp_store_gte_reg(Plain,14);pgxp_tag_word_source(Plain,packed,202);}
    else if(variant&32)pgxp_tag_word_source(Plain,packed,202);
    if(variant&128)cpu.read_word=substituted_reader;
#endif
    callback_reads=0;
    if(variant&8)psx_write_half(Input,9); // stale X, matching Y must survive the load
    if(variant&16)cpu.gte_data[14]^=1; // source validation by MFC2 must remain observable
    psx_cycle_count=0;PGXPHookStats before{},after{};pgxp_get_hook_stats(&before);
    if(native)flow_candidate(&cpu);else flow_reference(&cpu);
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS
    if(native)flow_candidate_func_80010300(&cpu);else flow_reference_func_80010300(&cpu);
    psx_write_word(Probe+256,cpu.gpr[2]);psx_pgxp_store(&cpu,0xAC020000u,Probe+256,cpu.gpr[2]);
    cpu.gpr[20]=2;pgxp_native_clobber(&cpu,1u<<20,0);
    if(variant&64)pgxp_native_scalar_gpr(&cpu,20,2.25,1);
    if(native)flow_candidate_func_80010400(&cpu);else flow_reference_func_80010400(&cpu);
#endif
    pgxp_get_hook_stats(&after);result.alu=after.alu-before.alu;result.cycles=psx_cycle_count;result.operations=after.operations-before.operations;result.operation_loads=after.operation_loads-before.operation_loads;result.callbacks=callback_reads;
    std::memcpy(result.bytes,ram,sizeof ram);
    unsigned n=0;
    auto capture=[&](uint32_t address,uint32_t value) {
        if(!pgxp_capture_word_shadow(address,value,&result.words[n]))std::abort();
        result.sources[n++]=pgxp_load_source(address,value);
    };
    for(unsigned i=0;i<6;++i)capture(out+4*i,psx_read_word(out+4*i));
    for(unsigned r=0;r<32;++r) {
        const uint32_t address=Probe+r*4;psx_write_word(address,cpu.gpr[r]);
        psx_pgxp_store(&cpu,(0x2Bu<<26)|(r<<16),address,cpu.gpr[r]);capture(address,cpu.gpr[r]);
    }
    for(unsigned r=0;r<32;++r) {
        const uint32_t address=Probe+128+r*4;psx_write_word(address,cpu.gte_data[r]);
        psx_pgxp_cop2(&cpu,(0x3Au<<26)|(r<<16),cpu.gte_data[r],address);capture(address,cpu.gte_data[r]);
    }
    capture(Probe+256,psx_read_word(Probe+256));
}
int main() {
    for(unsigned test=0;test<36;++test) {
        const unsigned v=test<32?test: test==32?32:test==33?64:test==34?128:192;
        if(!v){std::fputs("RUN first reference PGXP product\n",stderr);std::fflush(stderr);}
        run(reference,false,v);
        if(!v){std::fputs("RUN first native PGXP product\n",stderr);std::fflush(stderr);}
        run(candidate,true,v);
        if(std::memcmp(&reference.cpu,&candidate.cpu,sizeof(CPUState))||
           std::memcmp(reference.bytes,candidate.bytes,sizeof ram)||reference.cycles!=candidate.cycles||reference.callbacks!=candidate.callbacks) {
            std::fprintf(stderr,"FAIL variant%u guest product differs\n",v);return 1;
        }
        for(unsigned i=0;i<71;++i)if(!equal(reference.words[i],candidate.words[i])||reference.sources[i]!=candidate.sources[i]) {
            std::fprintf(stderr,"FAIL variant%u precision product%u flags%X/%X source%llu/%llu\n",v,i,
                reference.words[i].flags,candidate.words[i].flags,(unsigned long long)reference.sources[i],(unsigned long long)candidate.sources[i]);return 1;
        }
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS
        if(v<32 && (!candidate.operations || !candidate.operation_loads)) {
            std::fputs("FAIL: real nongeometry operation did not execute with hooks removed\n",stderr);return 1;
        }
        if(v>=32 && candidate.operation_loads) {
            std::fputs("FAIL: source-only/geometry/custom-reader transport bypassed canonical LOAD\n",stderr);return 1;
        }
#endif
        if(candidate.alu>=reference.alu) {std::fputs("FAIL: generated dataflow path did not remove hooks\n",stderr);return 1;}
        if(!v)std::printf("Generated GTE-to-packet ALU hooks reference/native: %llu/%llu\n",
            (unsigned long long)reference.alu,(unsigned long long)candidate.alu);
    }
    std::puts("PASS:36 generated GTE/CPU/memory/packet+71precision/lineage products (aliases,stale sources,partial/delayed loads)");
}
