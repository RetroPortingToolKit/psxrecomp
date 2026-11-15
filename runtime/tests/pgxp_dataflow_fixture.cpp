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
static unsigned char ram[0x200000],scratch[1024];
extern "C" {
uint8_t *g_psx_ram=ram,*g_psx_scratchpad=scratch;
PSXDirectMemoryStats g_psx_direct_memory_stats;
uint32_t g_debug_last_store_pc,g_debug_current_func_addr;
uint64_t psx_cycle_count,psx_next_service_cycle=UINT64_MAX;
int g_ls_mode,g_ls_replay_active,g_ram_read_watch_active,g_dma_exec_depth;
volatile int g_ds_recording;
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
void psx_write_word(uint32_t a,uint32_t v) {std::memcpy(ram+(a&0x1FFFFC),&v,4);}
void psx_write_half(uint32_t a,uint16_t v) {std::memcpy(ram+(a&0x1FFFFE),&v,2);}
void psx_write_byte(uint32_t a,uint8_t v) {ram[a&0x1FFFFF]=v;}
}
constexpr uint32_t Input=0x80001000,Plain=0x80002000,Output=0x80003000,Probe=0x80004000;
struct Product {CPUState cpu;unsigned char bytes[sizeof ram];PGXPWordShadow words[68];uint64_t sources[68],alu,cycles;};
static Product reference,candidate;
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
    if(variant&8)psx_write_half(Input,9); // stale X, matching Y must survive the load
    psx_cycle_count=0;PGXPHookStats before{},after{};pgxp_get_hook_stats(&before);
    if(native)flow_candidate(&cpu);else flow_reference(&cpu);
    pgxp_get_hook_stats(&after);result.alu=after.alu-before.alu;result.cycles=psx_cycle_count;
    std::memcpy(result.bytes,ram,sizeof ram);
    unsigned n=0;
    auto capture=[&](uint32_t address,uint32_t value) {
        if(!pgxp_capture_word_shadow(address,value,&result.words[n]))std::abort();
        result.sources[n++]=pgxp_load_source(address,value);
    };
    for(unsigned i=0;i<4;++i)capture(out+4*i,psx_read_word(out+4*i));
    for(unsigned r=0;r<32;++r) {
        const uint32_t address=Probe+r*4;psx_write_word(address,cpu.gpr[r]);
        psx_pgxp_store(&cpu,(0x2Bu<<26)|(r<<16),address,cpu.gpr[r]);capture(address,cpu.gpr[r]);
    }
    for(unsigned r=0;r<32;++r) {
        const uint32_t address=Probe+128+r*4;psx_write_word(address,cpu.gte_data[r]);
        psx_pgxp_cop2(&cpu,(0x3Au<<26)|(r<<16),cpu.gte_data[r],address);capture(address,cpu.gte_data[r]);
    }
}
int main() {
    for(unsigned v=0;v<16;++v) {
        run(reference,false,v);run(candidate,true,v);
        if(std::memcmp(&reference.cpu,&candidate.cpu,sizeof(CPUState))||
           std::memcmp(reference.bytes,candidate.bytes,sizeof ram)||reference.cycles!=candidate.cycles) {
            std::fprintf(stderr,"FAIL variant%u guest product differs\n",v);return 1;
        }
        for(unsigned i=0;i<68;++i)if(!equal(reference.words[i],candidate.words[i])||reference.sources[i]!=candidate.sources[i]) {
            std::fprintf(stderr,"FAIL variant%u precision product%u flags%X/%X source%llu/%llu\n",v,i,
                reference.words[i].flags,candidate.words[i].flags,(unsigned long long)reference.sources[i],(unsigned long long)candidate.sources[i]);return 1;
        }
        if(candidate.alu>=reference.alu) {std::fputs("FAIL: generated dataflow path did not remove hooks\n",stderr);return 1;}
        if(!v)std::printf("Generated GTE-to-packet ALU hooks reference/native: %llu/%llu\n",
            (unsigned long long)reference.alu,(unsigned long long)candidate.alu);
    }
    std::puts("PASS:16 generated GTE/CPU/memory/packet+68precision/lineage products (aliases and stale halves)");
}
