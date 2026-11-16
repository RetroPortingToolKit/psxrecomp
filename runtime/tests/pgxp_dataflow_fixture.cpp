#include "cpu_state.h"
#include "pgxp.h"
#include "pgxp_hooks.h"
#include "psx_direct_memory.h"
#include "psx_cycles.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" void flow_reference(CPUState*);
extern "C" void flow_candidate(CPUState*);
extern "C" void flow_reference_func_80010300(CPUState*);
extern "C" void flow_candidate_func_80010300(CPUState*);
extern "C" void flow_reference_func_80010400(CPUState*);
extern "C" void flow_candidate_func_80010400(CPUState*);
extern "C" void flow_reference_func_80010500(CPUState*);
extern "C" void flow_candidate_func_80010500(CPUState*);
extern "C" void flow_reference_func_80010600(CPUState*);
extern "C" void flow_candidate_func_80010600(CPUState*);
struct WriteHistory {uint32_t address,old_value,value,width,pc,v0,v1;};
static WriteHistory history[256];static unsigned history_count;
static CPUState *live_cpu;
static uint64_t frame_count;
static unsigned char ram[0x200000],scratch[1024];
extern "C" {
uint8_t *g_psx_ram=ram,*g_psx_scratchpad=scratch;
PSXDirectMemoryStats g_psx_direct_memory_stats;
PSXMemoryWidthStats g_psx_memory_width_stats;
uint64_t psx_hr_bus_callbacks[2][3];
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
void debug_server_trace_write_check(uint32_t a,uint32_t old,uint32_t v,uint8_t width) {
    if(history_count>=256)std::abort();
    uint32_t actual=ram[a&0x1fffffu];
    if(width>=2)actual|=(uint32_t)ram[(a+1)&0x1fffffu]<<8;
    if(width==4)actual|=(uint32_t)ram[(a+2)&0x1fffffu]<<16|(uint32_t)ram[(a+3)&0x1fffffu]<<24;
    if(actual!=old)std::abort(); // observer still sees the pre-mutation bytes
    history[history_count++]={a,old,v,width,g_debug_last_store_pc,
        live_cpu?live_cpu->gpr[2]:0u,live_cpu?live_cpu->gpr[3]:0u};
}
void psx_direct_precision_write(uint32_t a,uint32_t old,uint32_t v,uint8_t width) {debug_server_trace_write_check(a,old,v,width);pgxp_write_owned(a,v,width);}
uint32_t g_psx_cyc_batch,g_psx_cyc_batch_limit,g_psx_gcs_frac,g_psx_gcs_recip_q16=65536;
uint32_t *g_psx_cyc_local_acc;
int g_event_step_conservative,psx_in_device_service,g_psx_cyc_bb_defer,g_psx_load_delay;
int gte_geometry_correction_lookup(uint32_t,int32_t*,int32_t*) {return 0;}
int gte_geometry_correction_lookup_probe(uint32_t,int32_t*,int32_t*) {return 0;}
#if !defined(PSX_TEST_PGXP_REGIONS)
void psx_check_interrupts_at(CPUState*,uint32_t) {}
#endif
#if !defined(PSX_TEST_PGXP_REGIONS)
int psx_mod_try_function_replacement(CPUState*,uint32_t) {return 0;}
#endif
uint32_t psx_read_word(uint32_t a) {uint32_t v;std::memcpy(&v,ram+(a&0x1FFFFC),4);return v;}
uint16_t psx_read_half(uint32_t a) {uint16_t v;std::memcpy(&v,ram+(a&0x1FFFFE),2);return v;}
uint8_t psx_read_byte(uint32_t a) {return ram[a&0x1FFFFF];}
void psx_write_word(uint32_t a,uint32_t v) {debug_server_trace_write_check(a&0x1fffffu,psx_read_word(a),v,4);pgxp_write_owned(a,v,4);std::memcpy(ram+(a&0x1FFFFC),&v,4);}
void psx_write_half(uint32_t a,uint16_t v) {debug_server_trace_write_check(a&0x1fffffu,psx_read_half(a),v,2);pgxp_write_owned(a,v,2);std::memcpy(ram+(a&0x1FFFFE),&v,2);}
void psx_write_byte(uint32_t a,uint8_t v) {debug_server_trace_write_check(a&0x1fffffu,psx_read_byte(a),v,1);pgxp_write_owned(a,v,1);ram[a&0x1FFFFF]=v;}
}
#if defined(PSX_TEST_PGXP_REGIONS)
#include "region_case.h"
extern "C" {
void flow_reference_func_80010700(CPUState*);void flow_candidate_func_80010700(CPUState*);
void flow_reference_func_80010800(CPUState*);void flow_candidate_func_80010800(CPUState*);
void flow_reference_func_80010900(CPUState*);void flow_candidate_func_80010900(CPUState*);
void fixture_regions_owner(CPUState*,uint32_t,uint32_t,int);
unsigned fixture_regions_owner_calls(void);
int psx_game_text_native_ok(uint32_t address) {
    const uint32_t physical=address&0x1fffffffu,base=REGION_CODE_BASE&0x1fffffffu;
    return address<0xc0000000u && !(address&3u) && physical>=base &&
        physical<base+REGION_CODE_BYTES && !std::memcmp(ram+base,region_code,REGION_CODE_BYTES);
}
int g_psx_call_bail,g_psx_precise_slice,g_psx_guest_time_frozen;
uint64_t psx_next_cycle_sample=UINT64_MAX;
uint32_t i_stat,i_mask;
int psx_defer_switch_pending,g_psx_savestate_pending,g_ls_suppress_record;
int psx_observation_wait_active;
uint64_t psx_observation_block_edges,psx_observation_slow_edges;
}
static unsigned region_entry[3],region_replacements,region_instruction,region_sample,region_edges;
static unsigned region_mode,region_sample_receipts;
static CPUState* region_cpu;
extern "C" int psx_mod_function_entry(CPUState* cpu,uint32_t pc) {
    if(pc<0x80010700u||pc>0x80010900u||(pc&255u))std::abort();
    ++region_entry[(pc-0x80010700u)/256u];
    if(cpu->pc)std::abort();
    return 0;
}
extern "C" int psx_mod_try_function_replacement(CPUState*,uint32_t) {++region_replacements;return 0;}
static void region_receipt(CPUState*,unsigned,uint32_t);
extern "C" void psx_mod_instruction(CPUState* cpu,uint32_t pc,uint32_t word) {
    if(pc!=0x80010804u||word!=0x34421234u)std::abort();++region_instruction;
    region_receipt(cpu,2,3); // real mid-body callback sees the preceding LUI receipt
}
static void region_receipt(CPUState* cpu,unsigned reg,uint32_t flags) {
    const uint32_t address=0x80005000u+reg*4u;
    psx_write_word(address,cpu->gpr[reg]);
    psx_pgxp_store(cpu,0xac000000u|(reg<<16),address,cpu->gpr[reg]);
    PGXPWordShadow word{};
    if(!pgxp_capture_word_shadow(address,cpu->gpr[reg],&word)||word.flags!=flags||
       pgxp_load_source(address,cpu->gpr[reg])) {
        std::fprintf(stderr,"FAIL actual observer receipt r%u flags%X/%X\n",reg,word.flags,flags);std::abort();
    }
}
extern "C" void psx_observation_sample(void) {
    ++region_sample;
    region_receipt(region_cpu,8,3);region_receipt(region_cpu,9,0);
    ++region_sample_receipts;psx_next_cycle_sample=UINT64_MAX;
}
extern "C" void psx_observation_edge(CPUState* cpu,uint32_t pc,int) {
    ++region_edges;++psx_observation_slow_edges;
    if(region_mode==2||region_mode==3) {
        if(pc!=0x80010900u||region_entry[2]!=1u)std::abort();
        region_receipt(cpu,8,3);
    }
    psx_next_service_cycle=UINT64_MAX;g_psx_savestate_pending=0;
    if(region_mode==3) {cpu->pc=0x80055500u;cpu->gpr[2]=0x55aau;g_psx_call_bail=1;}
}
extern "C" void psx_check_interrupts_at(CPUState* cpu,uint32_t pc) {psx_observation_edge(cpu,pc,0);}
extern "C" int psx_slice_block_impl(CPUState*,uint32_t,uint32_t,int) {std::abort();}
extern "C" void psx_advance_cycles_slow(uint32_t) {std::abort();}
extern "C" void fixture_regions_dispatch(CPUState* cpu,uint32_t address,int selected) {
    if(address==0x80010700u||address==REGION_CONT_PC) {
        cpu->pc=address==0x80010700u?0u:address;
        if(selected)flow_candidate_func_80010700(cpu);else flow_reference_func_80010700(cpu);
    } else if(address==0x80010800u) {
        cpu->pc=0;
        if(selected)flow_candidate_func_80010800(cpu);else flow_reference_func_80010800(cpu);
    } else if(address==0x80010900u) {
        cpu->pc=0;
        if(selected)flow_candidate_func_80010900(cpu);else flow_reference_func_80010900(cpu);
    } else {std::fprintf(stderr,"FAIL unowned CPS target %08X\n",address);std::abort();}
}
#endif
constexpr uint32_t Input=0x80001000,Plain=0x80002000,Output=0x80003000,Probe=0x80004000;
struct Product {CPUState cpu;unsigned char bytes[sizeof ram];PGXPWordShadow words[74];uint64_t sources[74],alu,cycles,operations,operation_loads,callbacks,callback_writes;
    uint64_t getter_loads,mixed_stores,cold_operations;unsigned history_count,store_count;
    WriteHistory writes[256];PGXPStoreRecord stores[128];uint64_t bus_callbacks[2][3];};
static Product reference,candidate;
static uint64_t callback_reads,callback_writes;
static void substituted_writer(uint32_t address,uint32_t value){++callback_writes;psx_write_word(address,value^(address==0x80020000u?1u:0u));}
static uint32_t substituted_reader(uint32_t address) {++callback_reads;return psx_read_word(address)+(address==Plain?1u:0u);}

static bool equal(const PGXPWordShadow&a,const PGXPWordShadow&b) {
    return a.address==b.address&&a.value==b.value&&a.valid==b.valid&&a.flags==b.flags&&a.x16==b.x16&&
        a.y16==b.y16&&a.z==b.z&&!std::memcmp(&a.projection,&b.projection,sizeof a.projection);
}
static void run(Product& result,bool native,unsigned variant) {
    auto& cpu=result.cpu;live_cpu=&cpu;history_count=0;g_host_store_depth=0;
    std::memset(&cpu,0,sizeof cpu);std::memset(ram,0,sizeof ram);
#if defined(PSX_TEST_PGXP_REGIONS)
    std::memcpy(ram+(REGION_CODE_BASE&0x1fffffffu),region_code,REGION_CODE_BYTES);
#endif
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
    if(variant&256)cpu.write_word=substituted_writer;
    if(variant&512)g_host_store_depth=1;
#endif
    callback_reads=callback_writes=0;
    pgxp_set_trace_sources(&g_debug_last_store_pc,&frame_count);
    if(variant&8)psx_write_half(Input,9); // stale X, matching Y must survive the load
    if(variant&16)cpu.gte_data[14]^=1; // source validation by MFC2 must remain observable
    history_count=0;std::memset(psx_hr_bus_callbacks,0,sizeof psx_hr_bus_callbacks);
    const PGXPStoreRecord *store_ring=nullptr;uint32_t store_capacity=0;
    const uint64_t store_begin=pgxp_store_ring(&store_ring,&store_capacity);
    psx_cycle_count=0;PGXPHookStats before{},after{};pgxp_get_hook_stats(&before);
    if(native)flow_candidate(&cpu);else flow_reference(&cpu);
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS
    PGXPHookStats getter_before{},getter_after{};pgxp_get_hook_stats(&getter_before);
    if(native)flow_candidate_func_80010300(&cpu);else flow_reference_func_80010300(&cpu);
    pgxp_get_hook_stats(&getter_after);result.getter_loads=getter_after.operation_loads-getter_before.operation_loads;
    psx_write_word(Probe+256,cpu.gpr[2]);psx_pgxp_store(&cpu,0xAC020000u,Probe+256,cpu.gpr[2]);
    cpu.gpr[20]=2;pgxp_native_clobber(&cpu,1u<<20,0);
    if(variant&64)pgxp_native_scalar_gpr(&cpu,20,2.25,1);
    if(native)flow_candidate_func_80010400(&cpu);else flow_reference_func_80010400(&cpu);
    PGXPHookStats mixed_before{},mixed_after{};pgxp_get_hook_stats(&mixed_before);
    if(native)flow_candidate_func_80010500(&cpu);else flow_reference_func_80010500(&cpu);
    pgxp_get_hook_stats(&mixed_after);result.mixed_stores=mixed_after.operation_stores-mixed_before.operation_stores;
    // The untouched upper RAM bits are real but not a validated precision word.
    // A SH followed by LW must refuse the whole fast operation and keep the
    // canonical memory-validation effect rather than inventing known bits.
    psx_write_word(0x80020008u,0x76540000u);pgxp_invalidate_word(0x80020008u);
    PGXPHookStats cold_before{},cold_after{};pgxp_get_hook_stats(&cold_before);
    if(native)flow_candidate_func_80010600(&cpu);else flow_reference_func_80010600(&cpu);
    pgxp_get_hook_stats(&cold_after);result.cold_operations=cold_after.operations-cold_before.operations;
#endif
    pgxp_get_hook_stats(&after);result.alu=after.alu-before.alu;result.cycles=psx_cycle_count;result.operations=after.operations-before.operations;result.operation_loads=after.operation_loads-before.operation_loads;result.callbacks=callback_reads;result.callback_writes=callback_writes;
    std::memcpy(result.bus_callbacks,psx_hr_bus_callbacks,sizeof psx_hr_bus_callbacks);
    if(result.bus_callbacks[0][2]!=callback_reads || result.bus_callbacks[1][2]!=callback_writes) {
        std::fputs("FAIL: substituted bus values/effects are not counted exactly once\n",stderr);std::abort();
    }
    result.history_count=history_count;std::memcpy(result.writes,history,history_count*sizeof history[0]);
    const uint64_t store_end=pgxp_store_ring(&store_ring,&store_capacity);
    result.store_count=(unsigned)(store_end-store_begin);if(result.store_count>128)std::abort();
    for(unsigned i=0;i<result.store_count;++i){result.stores[i]=store_ring[(store_begin+i)%store_capacity];result.stores[i].seq=i;}
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
    for(unsigned i=0;i<3;++i)capture(0x80020000u+4*i,psx_read_word(0x80020000u+4*i));
}
#if defined(PSX_TEST_PGXP_REGIONS)
struct RegionProduct {CPUState cpu;unsigned char bytes[sizeof ram];uint64_t cycles,alu;
    uint32_t fraction;unsigned entries[3],replacements,instructions,samples,receipts,edges,owners;
    PGXPWordShadow gprs[32];uint64_t source[32];unsigned writes;WriteHistory history[256];};
static RegionProduct region_reference,region_candidate;
static void run_region(RegionProduct& out,bool selected,unsigned mode) {
    std::memset(&out,0,sizeof out);auto& cpu=out.cpu;region_cpu=live_cpu=&cpu;
    std::memset(ram,0,sizeof ram);history_count=0;
    std::memcpy(ram+(REGION_CODE_BASE&0x1fffffffu),region_code,REGION_CODE_BYTES);
    if(!psx_game_text_native_ok(0x80010800u)||psx_game_text_native_ok(0x80009000u))std::abort();
    ram[0x10000]^=1;
    if(psx_game_text_native_ok(0x80010800u))std::abort();
    ram[0x10000]^=1;
    cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
    cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
    cpu.gpr[4]=0x80006000u;cpu.gpr[16]=0;cpu.gpr[20]=2;
    cpu.gpr[29]=0x8001ff00u;cpu.gpr[31]=0x80009000u;
    pgxp_set_enabled(1);pgxp_set_cpu_mode(1);pgxp_set_source_tracking(1);pgxp_invalidate_all();
    cpu.gpr[8]=1;pgxp_native_scalar_gpr(&cpu,8,1.25,1);
    g_psx_call_bail=g_psx_precise_slice=g_psx_guest_time_frozen=0;
    psx_cycle_count=0;psx_next_service_cycle=psx_next_cycle_sample=UINT64_MAX;
    g_psx_cyc_batch=0;g_psx_cyc_local_acc=nullptr;g_psx_cyc_bb_defer=0;
    g_event_step_conservative=psx_in_device_service=g_psx_render_pass_active=0;
    g_ls_replay_active=g_ls_mode=g_ds_recording=0;g_overlay_flush_pending_cycles=nullptr;
    g_psx_gcs_recip_q16=mode==1?21845u:65536u;g_psx_gcs_frac=mode==1?65535u:0u;
    psx_defer_switch_pending=g_psx_savestate_pending=psx_observation_wait_active=0;i_stat=i_mask=0;
    std::memset(region_entry,0,sizeof region_entry);region_replacements=region_instruction=region_sample=region_sample_receipts=region_edges=0;
    region_mode=mode;
    if(mode==1)psx_next_cycle_sample=((uint64_t)REGION_FIRST_CHARGE*21845u+65535u)/65536u+1u;
    if(mode==2||mode==3)g_psx_savestate_pending=1;
    PGXPHookStats before{},after{};pgxp_get_hook_stats(&before);
    if(mode==3)flow_candidate_func_80010900(&cpu); // callback escape is observed before outer dispatch consumes its PC
    else if(mode==4) {
        // A public IRQ/interior entry is canonical, and consumes no entry hook.
        cpu.pc=REGION_INTERIOR_PC;cpu.gpr[29]-=16;psx_write_word(cpu.gpr[29]+12,cpu.gpr[31]);
        if(selected)flow_candidate_func_80010700(&cpu);else flow_reference_func_80010700(&cpu);
    } else fixture_regions_owner(&cpu,mode==2?0x80010900u:0x80010700u,0x80009000u,selected);
    pgxp_get_hook_stats(&after);out.alu=after.alu-before.alu;out.cycles=psx_cycle_count;out.fraction=g_psx_gcs_frac;
    std::memcpy(out.entries,region_entry,sizeof region_entry);out.replacements=region_replacements;
    out.instructions=region_instruction;out.samples=region_sample;out.receipts=region_sample_receipts;out.edges=region_edges;
    out.owners=fixture_regions_owner_calls();std::memcpy(out.bytes,ram,sizeof ram);
    out.writes=history_count;std::memcpy(out.history,history,history_count*sizeof history[0]);
    for(unsigned r=0;r<32;++r) {
        const uint32_t address=0x80007000u+r*4u;
        psx_pgxp_store(&cpu,0xac000000u|(r<<16),address,cpu.gpr[r]);
        if(!pgxp_capture_word_shadow(address,cpu.gpr[r],&out.gprs[r]))std::abort();
        out.source[r]=pgxp_load_source(address,cpu.gpr[r]);
    }
}
static void region_products() {
    for(unsigned mode:{0u,1u,2u,4u}) {
        run_region(region_reference,false,mode);run_region(region_candidate,true,mode);
        auto& a=region_reference;auto& b=region_candidate;
        if(std::memcmp(&a.cpu,&b.cpu,sizeof a.cpu)||std::memcmp(a.bytes,b.bytes,sizeof ram)||
           a.cycles!=b.cycles||a.fraction!=b.fraction||std::memcmp(a.entries,b.entries,sizeof a.entries)||
           a.replacements!=b.replacements||a.instructions!=b.instructions||a.samples!=b.samples||a.receipts!=b.receipts||a.edges!=b.edges||
           a.writes!=b.writes||std::memcmp(a.history,b.history,a.writes*sizeof history[0]))
            {std::fprintf(stderr,"FAIL emitted owner/clock/hook product mode%u\n",mode);std::abort();}
        for(unsigned r=0;r<32;++r)if(!equal(a.gprs[r],b.gprs[r])||a.source[r]!=b.source[r])
            {std::fprintf(stderr,"FAIL emitted receipt mode%u r%u\n",mode,r);std::abort();}
        if(mode==0||mode==1) {
            if(b.entries[0]!=1||b.entries[1]!=1||b.replacements!=2||b.instructions!=1||b.owners!=3||b.alu>=a.alu)std::abort();
        }
        if(!b.cycles)std::abort();
        if(mode==1 && (b.samples!=1||b.receipts!=1||!b.fraction))std::abort();
        if(mode==2 && (b.entries[2]!=1||b.replacements!=1||b.edges!=1||b.alu>=a.alu))std::abort();
        if(mode==4 && (b.entries[0]||b.replacements||b.alu!=a.alu))std::abort();
        std::printf("PASS emitted regions mode%u clock%llu frac%u ALU%llu/%llu\n",mode,
            (unsigned long long)b.cycles,b.fraction,(unsigned long long)a.alu,(unsigned long long)b.alu);
    }
    run_region(region_candidate,true,3);
    if(region_candidate.cpu.pc!=0x80055500u||region_candidate.cpu.gpr[2]!=0x55aau||!g_psx_call_bail||
       region_candidate.entries[2]!=1||region_candidate.edges!=1)std::abort();
    std::puts("PASS real emitted owner/CPS private selection, canonical public entry, first-block hook-once escape, fractional precharge sample, redirect/bail");
}
#endif
int main() {
    for(unsigned test=0;test<38;++test) {
        const unsigned v=test<32?test: test==32?32:test==33?64:test==34?128:test==35?192:test==36?256:512;
        if(!v){std::fputs("RUN first reference PGXP product\n",stderr);std::fflush(stderr);}
        run(reference,false,v);
        if(!v){std::fputs("RUN first native PGXP product\n",stderr);std::fflush(stderr);}
        run(candidate,true,v);
        if(std::memcmp(&reference.cpu,&candidate.cpu,sizeof(CPUState))||
           std::memcmp(reference.bytes,candidate.bytes,sizeof ram)||reference.cycles!=candidate.cycles||reference.callbacks!=candidate.callbacks||reference.callback_writes!=candidate.callback_writes ||
           std::memcmp(reference.bus_callbacks,candidate.bus_callbacks,sizeof reference.bus_callbacks)) {
            std::fprintf(stderr,"FAIL variant%u guest product differs\n",v);return 1;
        }
        if(reference.history_count!=candidate.history_count || reference.store_count!=candidate.store_count ||
           std::memcmp(reference.writes,candidate.writes,reference.history_count*sizeof history[0]) ||
           std::memcmp(reference.stores,candidate.stores,reference.store_count*sizeof reference.stores[0])) {
            std::fprintf(stderr,"FAIL variant%u pre-write/history/typed store-ring ordering differs\n",v);return 1;
        }
        for(unsigned i=0;i<74;++i)if(!equal(reference.words[i],candidate.words[i])||reference.sources[i]!=candidate.sources[i]) {
            std::fprintf(stderr,"FAIL variant%u precision product%u flags%X/%X source%llu/%llu\n",v,i,
                reference.words[i].flags,candidate.words[i].flags,(unsigned long long)reference.sources[i],(unsigned long long)candidate.sources[i]);return 1;
        }
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS
        if(v<32 && (!candidate.operations || !candidate.getter_loads || candidate.mixed_stores!=3)) {
            std::fputs("FAIL: real nongeometry operation did not execute with hooks removed\n",stderr);return 1;
        }
        if((v&(32u|64u|128u|512u)) && (candidate.getter_loads || candidate.mixed_stores)) {
            std::fputs("FAIL: source-only/geometry/custom-reader transport bypassed canonical LOAD\n",stderr);return 1;
        }
        if((v&256u) && (!candidate.callback_writes || candidate.mixed_stores)){
            std::fputs("FAIL: custom writer values/effects bypassed canonical operation fallback\n",stderr);return 1;}
        if(candidate.cold_operations){std::fputs("FAIL: cold partial-word LW validation was bypassed\n",stderr);return 1;}
#endif
        if(candidate.alu>=reference.alu) {std::fputs("FAIL: generated dataflow path did not remove hooks\n",stderr);return 1;}
        if(!v)std::printf("Generated GTE-to-packet ALU hooks reference/native: %llu/%llu\n",
            (unsigned long long)reference.alu,(unsigned long long)candidate.alu);
    }
#if defined(PSX_TEST_PGXP_REGIONS)
    region_products();
#endif
    std::puts("PASS:38 generated GTE/CPU/memory/packet+74precision/lineage products+ordered write/store records (aliases,stale sources,partial/delayed loads)");
}
