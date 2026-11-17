#include "cpu_state.h"
#include "pgxp.h"
#include "pgxp_hooks.h"
#include "psx_direct_memory.h"
#include "psx_cycles.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <csetjmp>
#if defined(PSX_TEST_PGXP_REGIONS)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif
#endif

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
#if defined(PSX_TEST_GEOMETRY_CONTROL)
static bool control_active,control_observer,partial_active;
static int control_scratch_mode=-1;static unsigned control_scratch_irq;
static std::jmp_buf control_scratch_escape;
static void control_watch(CPUState*,uint32_t,uint32_t);
#endif
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
#if defined(PSX_TEST_GEOMETRY_CONTROL)
    if(control_active)control_watch(live_cpu,a,old);
    if(partial_active) {
        // The observer receives the old accessed width. Source receipts are
        // aligned whole words, including before an SH into the high half.
        const uint32_t aligned=a&~3u;
        uint32_t word;std::memcpy(&word,ram+(aligned&0x1fffffu),sizeof word);
        if(pgxp_load_source(aligned,word)!=901)std::abort();
    }
#endif
    history[history_count++]={a,old,v,width,g_debug_last_store_pc,
        live_cpu?live_cpu->gpr[2]:0u,live_cpu?live_cpu->gpr[3]:0u};
}
#if !defined(PSX_TEST_GEOMETRY_CONTROL)
void psx_direct_precision_write(uint32_t a,uint32_t old,uint32_t v,uint8_t width) {debug_server_trace_write_check(a,old,v,width);pgxp_write_owned(a,v,width);}
#endif
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
#include "psx_native_span.h"
#include "psx_block_edge.h"
extern "C" {
void flow_reference_func_80010700(CPUState*);void flow_candidate_func_80010700(CPUState*);
void flow_reference_func_80010800(CPUState*);void flow_candidate_func_80010800(CPUState*);
void flow_reference_func_80010810(CPUState*);void flow_candidate_func_80010810(CPUState*);
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
static PGXPWordShadow region_observed_sp;
static uint64_t region_observed_sp_source;
static double region_observed_sp_scalar;
static int region_observed_sp_precise;
static void region_sp_receipt(CPUState* cpu) {
    constexpr uint32_t address=0x80005200u;
    psx_pgxp_store(cpu,0xac1d0000u,address,cpu->gpr[29]);
    if(!pgxp_capture_word_shadow(address,cpu->gpr[29],&region_observed_sp))std::abort();
    region_observed_sp_source=pgxp_load_source(address,cpu->gpr[29]);
    region_observed_sp_scalar=0;
    region_observed_sp_precise=pgxp_native_gpr_scalar(29,cpu->gpr[29],&region_observed_sp_scalar);
}
extern "C" int psx_mod_function_entry(CPUState* cpu,uint32_t pc) {
    const unsigned entry=pc==0x80010700u?0u:pc==0x80010800u?1u:pc==REGION_LOOP_PC?2u:3u;
    if(entry==3u)std::abort();
    ++region_entry[entry];
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
    // mode1 takes the ORI path: changing the low GPU field invalidates X,
    // while the unchanged high half retains Y. The reference arm proves this.
    if(region_mode!=1u||region_cpu->gpr[8]!=0x80007080u)std::abort();
    region_receipt(region_cpu,8,2);region_receipt(region_cpu,9,0);
    region_sp_receipt(region_cpu);
    // This callback keeps the integer word but replaces the live scalar. A
    // later affine receipt must consume this state, not its old entry snapshot.
    if(!pgxp_native_scalar_gpr(region_cpu,29,double((int32_t)region_cpu->gpr[29])+.75,1))std::abort();
    ++region_sample_receipts;psx_next_cycle_sample=UINT64_MAX;
}
extern "C" void psx_observation_edge(CPUState* cpu,uint32_t pc,int) {
    ++region_edges;++psx_observation_slow_edges;
#if defined(PSX_TEST_GEOMETRY_SCRATCH)
    if(control_scratch_mode==6 || control_scratch_mode==7) {
        if(pc!=0x800108a0u || control_scratch_irq++)std::abort();
        // A same-PC context change must leave private facts behind. Preserve
        // the real caller-visible fractional value through the following ADDU.
        cpu->gpr[14]=0x80023800u;
        if(!pgxp_native_scalar_gpr(cpu,14,double((int32_t)cpu->gpr[14])+.625,1))std::abort();
        psx_next_service_cycle=UINT64_MAX;
        if(control_scratch_mode==7) {
            cpu->pc=0x80055500u;cpu->gpr[2]=0x55aau;g_psx_call_bail=1;
            // The maintained nonlocal scheduler/thread transfer never returns
            // to an old compiled frame (interrupts.c uses longjmp too).
            std::longjmp(control_scratch_escape,1);
        }
        return;
    }
#endif
    region_sp_receipt(cpu);
    if(region_mode==2||region_mode==3) {
        if(pc!=REGION_LOOP_PC||region_entry[2]!=1u) {
            std::fprintf(stderr,"FAIL loop observer pc%08X expected%08X entry%u\n",pc,REGION_LOOP_PC,region_entry[2]);std::abort();
        }
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
    } else if(address==REGION_LOOP_PC) {
        cpu->pc=0;
        if(selected)flow_candidate_func_80010810(cpu);else flow_reference_func_80010810(cpu);
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
    PGXPWordShadow gprs[32];uint64_t source[32];unsigned writes;WriteHistory history[256];
    PGXPWordShadow observed_sp;uint64_t observed_sp_source;double observed_sp_scalar,sp_scalar;
    int observed_sp_precise,sp_precise;};
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
    cpu.gpr[4]=0x80006000u;cpu.gpr[16]=mode==1?20u:0u;cpu.gpr[20]=2;
    cpu.gpr[29]=0x8001ff00u;cpu.gpr[31]=0x80009000u;
    pgxp_set_enabled(1);pgxp_set_cpu_mode(1);pgxp_set_source_tracking(1);pgxp_invalidate_all();
    pgxp_gte_push_sxy((int32_t)(int16_t)cpu.gpr[29]*65536+16384,
        (int32_t)(int16_t)(cpu.gpr[29]>>16)*65536+32768,300,cpu.gpr[29]);
    pgxp_store_gte_reg(0x80006200u,15);pgxp_tag_word_source(0x80006200u,cpu.gpr[29],919);
    psx_pgxp_load(&cpu,0x8c1d0000u,0x80006200u,cpu.gpr[29]);
    if(mode==1)pgxp_native_scalar_gpr(&cpu,29,double((int32_t)cpu.gpr[29])+.25,1);
    cpu.gpr[3]=0x80000000u;
    pgxp_gte_push_sxy(16384,(-32768*65536)+32768,300,cpu.gpr[3]);
    pgxp_store_gte_reg(0x80006204u,15);pgxp_tag_word_source(0x80006204u,cpu.gpr[3],920);
    psx_pgxp_load(&cpu,0x8c030000u,0x80006204u,cpu.gpr[3]);
    region_observed_sp={};region_observed_sp_source=0;region_observed_sp_scalar=0;region_observed_sp_precise=0;
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
    if(mode==3)flow_candidate_func_80010810(&cpu); // callback escape is observed before outer dispatch consumes its PC
    else if(mode==4) {
        // A public IRQ/interior entry is canonical, and consumes no entry hook.
        cpu.pc=REGION_INTERIOR_PC;cpu.gpr[29]-=24;psx_write_word(cpu.gpr[29]+16,cpu.gpr[31]);
        if(selected)flow_candidate_func_80010700(&cpu);else flow_reference_func_80010700(&cpu);
    } else fixture_regions_owner(&cpu,mode==2?REGION_LOOP_PC:0x80010700u,0x80009000u,selected);
    pgxp_get_hook_stats(&after);out.alu=after.alu-before.alu;out.cycles=psx_cycle_count;out.fraction=g_psx_gcs_frac;
    std::memcpy(out.entries,region_entry,sizeof region_entry);out.replacements=region_replacements;
    out.instructions=region_instruction;out.samples=region_sample;out.receipts=region_sample_receipts;out.edges=region_edges;
    out.observed_sp=region_observed_sp;out.observed_sp_source=region_observed_sp_source;
    out.observed_sp_scalar=region_observed_sp_scalar;out.observed_sp_precise=region_observed_sp_precise;
    out.sp_precise=pgxp_native_gpr_scalar(29,cpu.gpr[29],&out.sp_scalar);
    out.owners=fixture_regions_owner_calls();std::memcpy(out.bytes,ram,sizeof ram);
    out.writes=history_count;std::memcpy(out.history,history,history_count*sizeof history[0]);
    for(unsigned r=0;r<32;++r) {
        const uint32_t address=0x80007000u+r*4u;
        psx_pgxp_store(&cpu,0xac000000u|(r<<16),address,cpu.gpr[r]);
        if(!pgxp_capture_word_shadow(address,cpu.gpr[r],&out.gprs[r]))std::abort();
        out.source[r]=pgxp_load_source(address,cpu.gpr[r]);
    }
}
#if defined(PSX_TEST_GEOMETRY_CONTROL)
extern "C" void flow_reference_func_8001082C(CPUState*);
extern "C" void flow_candidate_func_8001082C(CPUState*);
extern "C" void flow_reference_func_80010884(CPUState*);
extern "C" void flow_candidate_func_80010884(CPUState*);
extern "C" int fixture_geometry_control_allowed(CPUState* cpu,uint32_t entry) {
    return (entry==0x8001082Cu || entry==0x80010884u) && cpu->gpr[31]==0x80009000u &&
        !control_observer && cpu->read_word==psx_read_word &&
        cpu->write_word==psx_write_word;
}
static void control_watch(CPUState* cpu,uint32_t address,uint32_t old) {
    if(g_debug_last_store_pc==0x80010830u || g_debug_last_store_pc==0x8001088cu) {
        if(pgxp_load_source(address,old)!=777) {
            std::fputs("FAIL write observer lost pre-write geometry/source\n",stderr);std::abort();
        }
        if(control_observer)region_sp_receipt(cpu); // real opaque metadata consumer => canonical
        if(control_scratch_mode==3)cpu->gpr[14]=0x80023800u; // real observer GPR clobber, canonical admission
        if(control_scratch_mode>=6)psx_next_service_cycle=psx_cycle_count; // due before next real block

    }
}
struct ControlProduct {
    CPUState cpu;unsigned char bytes[sizeof ram];PGXPWordShadow packet[3];uint64_t source[3];
    WriteHistory writes[256];unsigned count;uint64_t alu,loads,stores,callbacks;double sp,observed_sp;int sp_precise,observed_precise;
    PGXPWordShadow gte;uint64_t gte_source,cycles;uint32_t frac;double saved_address;int saved_address_precise;
};
static ControlProduct control_reference,control_candidate;
static uint32_t control_reader(uint32_t address) {++callback_reads;return psx_read_word(address)+(address==Input?1u:0u);}
static void run_control(ControlProduct& out,bool selected,unsigned mode,int cpu_mode,bool scratch_case=false) {
    // Nonlocal context transfer must not make an automatic CPU object
    // indeterminate across setjmp/longjmp. Both arms own this same reset storage.
    static CPUState control_cpu;control_cpu=CPUState{};CPUState& cpu=control_cpu;
    live_cpu=region_cpu=&cpu;std::memset(ram,0,sizeof ram);history_count=0;
    cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
    cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
    cpu.gpr[29]=0x8001ff00u;cpu.gpr[31]=0x80009000u;
    cpu.gpr[4]=(mode==1u)?cpu.gpr[29]-8u:Output;
    cpu.gpr[5]=(mode==2u)?(Input^0x20000000u):Input;
    if(mode==4u)cpu.read_word=control_reader; // truthful canonical custom-bus path
    control_observer=mode==3u;control_active=false;
    control_scratch_mode=scratch_case?int(mode):-1;control_scratch_irq=0;
    if(scratch_case){std::memset(scratch,0,sizeof scratch);cpu.gpr[16]=mode==2u?0x80023800u:0x1f800000u;}
    pgxp_set_enabled(1);pgxp_set_cpu_mode(cpu_mode);pgxp_set_source_tracking(1);
    pgxp_set_projection_tracking(1);pgxp_invalidate_all();
    pgxp_native_clobber(&cpu,0xfffffffeu,0);
    if(!pgxp_native_scalar_gpr(&cpu,29,double((int32_t)cpu.gpr[29])+.25,1))std::abort();
    const uint32_t packed=0x000c0008u,stack=cpu.gpr[29]-8u;
    psx_write_word(Input,packed);psx_write_word(stack,cpu.gpr[31]);
    pgxp_gte_push_sxy(8*65536+16384,12*65536+32768,300,packed);
    PGXPProjection projection{8.25f,12.5f,2.f,.5f};pgxp_gte_set_projection(&projection);
    pgxp_store_gte_reg(Input,15);pgxp_tag_word_source(Input,packed,921);
    PGXPNativeWord word{};const uint32_t input=Input;
    if(!pgxp_native_capture_words(&input,&packed,&word,1)||!pgxp_native_import_sxy(14,&word))std::abort();
    cpu.gte_data[14]=cpu.gte_data[15]=packed;
    pgxp_gte_push_sxy(16384,32768,301,cpu.gpr[31]);pgxp_store_gte_reg(stack,15);
    pgxp_tag_word_source(stack,cpu.gpr[31],777); // equal-bit stale geometry at RA slot
    // Restore the real GTE source after poisoning only the stack receipt.
    if(!pgxp_native_import_sxy(14,&word))std::abort();
    if(scratch_case) {
        scratch[0x2b4]=5;std::memcpy(scratch+20,&packed,4);
        pgxp_store_gte_reg(0x1f800014u,14);pgxp_tag_word_source(0x1f800014u,packed,922);
        psx_write_word(0x80023ab4u,5);psx_write_word(0x80023814u,packed);
        pgxp_store_gte_reg(0x80023814u,14);pgxp_tag_word_source(0x80023814u,packed,923);
        if(!pgxp_native_scalar_gpr(&cpu,16,double((int32_t)cpu.gpr[16])+.375,1))std::abort();
    }
    psx_cycle_count=0;psx_next_service_cycle=psx_next_cycle_sample=UINT64_MAX;
    g_psx_cyc_batch=0;g_psx_cyc_local_acc=nullptr;g_psx_cyc_bb_defer=0;
    g_event_step_conservative=psx_in_device_service=g_psx_render_pass_active=0;
    g_ls_replay_active=g_ls_mode=g_ds_recording=0;g_overlay_flush_pending_cycles=nullptr;
    g_psx_gcs_recip_q16=65536u;g_psx_gcs_frac=0;g_psx_precise_slice=g_psx_guest_time_frozen=g_psx_call_bail=0;
    psx_defer_switch_pending=g_psx_savestate_pending=psx_observation_wait_active=0;i_stat=i_mask=0;
    history_count=0;callback_reads=0;region_observed_sp_precise=0;region_observed_sp_scalar=0;
    control_active=true;PGXPHookStats before{},after{};pgxp_get_hook_stats(&before);
    if(!scratch_case || mode!=7u || setjmp(control_scratch_escape)==0) {
        if(scratch_case) {if(selected)flow_candidate_func_80010884(&cpu);else flow_reference_func_80010884(&cpu);}
        else {if(selected)flow_candidate_func_8001082C(&cpu);else flow_reference_func_8001082C(&cpu);}
    }
    pgxp_get_hook_stats(&after);control_active=false;
    out.cpu=cpu;std::memcpy(out.bytes,ram,sizeof ram);out.count=history_count;
    out.cycles=psx_cycle_count;out.frac=g_psx_gcs_frac;out.saved_address=0;
    out.saved_address_precise=pgxp_native_gpr_scalar(17,cpu.gpr[17],&out.saved_address);
    std::memcpy(out.writes,history,history_count*sizeof history[0]);
    out.alu=after.alu-before.alu;out.loads=after.load-before.load;out.stores=after.store-before.store;out.callbacks=callback_reads;
    out.observed_precise=region_observed_sp_precise;out.observed_sp=region_observed_sp_scalar;
    out.sp_precise=pgxp_native_gpr_scalar(29,cpu.gpr[29],&out.sp);
    for(unsigned i=0;i<(scratch_case?3u:2u);++i) {
        const uint32_t address=cpu.gpr[4]+4*i,value=psx_read_word(address);
        if(!pgxp_capture_word_shadow(address,value,&out.packet[i]))std::abort();
        out.source[i]=pgxp_load_source(address,value);
    }
    pgxp_store_gte_reg(Probe,14);
    if(!pgxp_capture_word_shadow(Probe,packed,&out.gte))std::abort();
    out.gte_source=pgxp_load_source(Probe,packed);
    if(mode!=1u && (pgxp_load_source(stack,cpu.gpr[31]) ||
            (pgxp_capture_word_shadow(stack,psx_read_word(stack),&word.word),word.word.flags))) {
        std::fputs("FAIL equal-value RA overwrite retained geometry\n",stderr);std::abort();
    }
}
extern "C" void flow_reference_func_80010860(CPUState*);
extern "C" void flow_candidate_func_80010860(CPUState*);
extern "C" void flow_reference_func_8001086C(CPUState*);
extern "C" void flow_candidate_func_8001086C(CPUState*);
extern "C" void flow_reference_func_80010878(CPUState*);
extern "C" void flow_candidate_func_80010878(CPUState*);
static void retained_partial_store_products() {
    struct Result {uint32_t value;PGXPWordShadow word;uint64_t source;WriteHistory write;};
    constexpr uint32_t destination=0x80023600u,source=0x80023700u,old_word=0x000c0008u;
    for(int cpu_mode:{0,1})for(unsigned kind=0;kind<3;++kind) {
        const unsigned lanes=kind?4u:2u;
        for(unsigned lane=0;lane<lanes;++lane) {
            std::printf("RUN retained typed store CPU%d kind%u lane%u\n",cpu_mode,kind,lane);std::fflush(stdout);
            Result result[2]{};
            for(unsigned selected=0;selected<2;++selected) {
                CPUState cpu{};live_cpu=&cpu;control_active=false;
                cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
                cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
                const uint32_t low=kind?9u:lane?12u:8u,word=(15u<<16)|low;
                // Alternate segment/mirror aliases exercise the same receipt.
                cpu.gpr[4]=(lane&1u?0xa0223600u:destination)+(kind?lane:lane*2u);
                cpu.gpr[8]=word;cpu.gpr[31]=0x80009000u;
                pgxp_set_enabled(1);pgxp_set_cpu_mode(cpu_mode);pgxp_set_source_tracking(1);
                pgxp_set_projection_tracking(1);pgxp_invalidate_all();
                psx_write_word(destination,old_word);psx_write_word(source,word);
                pgxp_gte_push_sxy(8*65536+16384,12*65536+32768,300,old_word);
                PGXPProjection p{8.25f,12.5f,2.f,.5f};pgxp_gte_set_projection(&p);
                pgxp_store_gte_reg(destination,15);pgxp_tag_word_source(destination,old_word,901);
                pgxp_gte_push_sxy(int32_t(low)*65536+49152,15*65536+16384,450,word);
                pgxp_store_gte_reg(source,15);pgxp_tag_word_source(source,word,902);
                psx_pgxp_load(&cpu,0x8c080000u,source,word);
                history_count=0;g_psx_call_bail=0;psx_cycle_count=0;
                psx_next_service_cycle=psx_next_cycle_sample=UINT64_MAX;
                const uint64_t inline_before=g_psx_direct_memory_stats.store_inline_ram;partial_active=true;
                if(!kind) {if(selected)flow_candidate_func_80010860(&cpu);else flow_reference_func_80010860(&cpu);}
                else if(kind==1) {if(selected)flow_candidate_func_8001086C(&cpu);else flow_reference_func_8001086C(&cpu);}
                else {if(selected)flow_candidate_func_80010878(&cpu);else flow_reference_func_80010878(&cpu);}
                partial_active=false;
                if(history_count!=1u || g_psx_direct_memory_stats.store_inline_ram!=inline_before+1u)std::abort();
                auto& out=result[selected];out.value=psx_read_word(destination);out.write=history[0];
                if(!pgxp_capture_word_shadow(destination,out.value,&out.word))std::abort();
                out.source=pgxp_load_source(destination,out.value);
            }
            const auto& a=result[0];const auto& b=result[1];
            const uint32_t observed_address=0x23600u+(!kind?lane*2u:0u);
            const uint32_t observed_old=kind?old_word:(old_word>>(lane*16u))&0xffffu;
            if(b.write.address!=observed_address || b.write.old_value!=observed_old ||
                    b.write.width!=(kind?4u:2u))std::abort();
            if(a.value!=b.value||!equal(a.word,b.word)||a.source!=b.source||
                    std::memcmp(&a.write,&b.write,sizeof a.write))std::abort();
            if(!kind) {
                // Equal integer SH still replaces the touched coordinate's
                // fraction. The untouched coordinate stays exactly canonical.
                if(b.value!=old_word || b.source || (b.word.flags&PGXP_F_VXY)!=PGXP_F_VXY ||
                        b.word.z || (b.word.flags&PGXP_F_PROJECTION))std::abort();
                if(!lane && (b.word.x16!=8*65536+49152 || b.word.y16!=12*65536+32768))std::abort();
                if(lane && (b.word.x16!=8*65536+16384 || b.word.y16!=12*65536+49152))std::abort();
            } else if((kind==1u&&lane==3u)||(kind==2u&&lane==0u)) {
                if(b.source!=902 || (b.word.flags&PGXP_F_VXY)!=PGXP_F_VXY ||
                        b.word.x16!=9*65536+49152 || b.word.y16!=15*65536+16384)std::abort();
            } else if(b.word.valid || b.source)std::abort(); // canonical partial SWL/SWR kills the word
        }
    }
    std::puts("PASS actual emitted SH/SWL/SWR write-boundary +retained canonical hooks, all byte lanes, equal SH fractions and aliases, CPU0/1");
}
#if defined(PSX_TEST_GEOMETRY_SCRATCH)
static void scratch_products() {
    for(int cpu_mode:{0,1})for(unsigned mode:{0u,1u,2u,3u,4u,6u,7u}) {
        std::printf("RUN scratch EA/sink CPU%d mode%u\n",cpu_mode,mode);std::fflush(stdout);
        run_control(control_reference,false,mode,cpu_mode,true);
        const unsigned reference_irq=control_scratch_irq;
        run_control(control_candidate,true,mode,cpu_mode,true);
        const auto& a=control_reference;const auto& b=control_candidate;
        if(std::memcmp(&a.cpu,&b.cpu,sizeof a.cpu)||std::memcmp(a.bytes,b.bytes,sizeof ram)||
                a.count!=b.count||std::memcmp(a.writes,b.writes,a.count*sizeof history[0])||
                a.cycles!=b.cycles||a.frac!=b.frac||a.callbacks!=b.callbacks||reference_irq!=control_scratch_irq)
            {std::fputs("FAIL scratch CPU/RAM/write/clock/IRQ\n",stderr);std::abort();}
        for(unsigned i=0;i<3;++i)if(!equal(a.packet[i],b.packet[i])||a.source[i]!=b.source[i])
            {std::fprintf(stderr,"FAIL scratch actual geometry/source sink%u\n",i);std::abort();}
        if(!equal(a.gte,b.gte)||a.gte_source!=b.gte_source || a.saved_address_precise!=b.saved_address_precise ||
                (a.saved_address_precise&&a.saved_address!=b.saved_address))std::abort();
        if(mode>=6u && control_scratch_irq!=1u)std::abort();
        if(mode==6u && cpu_mode && (!b.saved_address_precise || b.saved_address!=double((int32_t)(0x80023814u))+.625))std::abort();
        if(mode==7u && (b.cpu.pc!=0x80055500u || !g_psx_call_bail))std::abort();
        if(mode<3u && (!(b.alu<a.alu)||!(b.loads<a.loads)||b.stores!=a.stores))std::abort();
        if(mode==3u && (a.alu!=b.alu||a.loads!=b.loads||(!b.observed_precise&&cpu_mode)))std::abort();
        std::printf("PASS scratch CPU%d mode%u ALU%llu/%llu LOAD%llu/%llu IRQ%u\n",cpu_mode,mode,
            (unsigned long long)a.alu,(unsigned long long)b.alu,(unsigned long long)a.loads,(unsigned long long)b.loads,control_scratch_irq);
    }
    control_scratch_mode=-1;
}
#endif
static void control_products() {
    for(int cpu_mode:{0,1})for(unsigned mode=0;mode<5;++mode) {
        std::printf("RUN geometry-only frame CPU%d mode%u\n",cpu_mode,mode);std::fflush(stdout);
        run_control(control_reference,false,mode,cpu_mode);run_control(control_candidate,true,mode,cpu_mode);
        const auto& a=control_reference;const auto& b=control_candidate;
        if(std::memcmp(&a.cpu,&b.cpu,sizeof a.cpu)||std::memcmp(a.bytes,b.bytes,sizeof ram)||
                a.count!=b.count||std::memcmp(a.writes,b.writes,a.count*sizeof history[0]))std::abort();
        for(unsigned i=0;i<2;++i)if(!equal(a.packet[i],b.packet[i])||a.source[i]!=b.source[i])std::abort();
        if(!equal(a.gte,b.gte)||a.gte_source!=b.gte_source||!b.gte.flags)std::abort();
        if(mode<4u && (!b.packet[1].flags || b.source[1]!=921))std::abort();
        if(mode==4u && (b.packet[1].value!=0x000c0009u || !b.callbacks || a.callbacks!=b.callbacks))std::abort();
        if(mode==3u && (a.observed_precise!=b.observed_precise||a.observed_sp!=b.observed_sp||
                (cpu_mode && (!b.observed_precise||b.observed_sp!=double((int32_t)(b.cpu.gpr[29]-24u))+.25))))std::abort();
        if(mode<3u) {
            // Two frame ALUs disappear. The already-qualified incoming ADDIU
            // receipt also replaces the geometry +1 hook: full4 -> selected1.
            if(a.alu!=4u || b.alu!=1u || b.loads+1!=a.loads || b.stores+1!=a.stores || b.sp_precise) {
                std::fprintf(stderr,"FAIL geometry-frame CPU%d mode%u ALU%llu/%llu LOAD%llu/%llu STORE%llu/%llu SPprecise%d\n",
                    cpu_mode,mode,(unsigned long long)a.alu,(unsigned long long)b.alu,
                    (unsigned long long)a.loads,(unsigned long long)b.loads,
                    (unsigned long long)a.stores,(unsigned long long)b.stores,b.sp_precise);
                std::abort();
            }
        } else if(b.alu!=a.alu||b.loads!=a.loads||b.stores!=a.stores||
                b.sp_precise!=a.sp_precise||(b.sp_precise&&b.sp!=a.sp))std::abort();
        std::printf("PASS geometry-only frame CPU%d mode%u packets/source/write ordering ALU%llu/%llu\n",
            cpu_mode,mode,(unsigned long long)a.alu,(unsigned long long)b.alu);
    }
}
#endif
static void region_products() {
#if defined(PSX_TEST_PGXP_REGIONS_REMAINING)
    for(unsigned mode:{2u,4u}) {
#else
    for(unsigned mode:{0u,1u,2u,4u}) {
#endif
        run_region(region_reference,false,mode);run_region(region_candidate,true,mode);
        auto& a=region_reference;auto& b=region_candidate;
        if(std::memcmp(&a.cpu,&b.cpu,sizeof a.cpu)||std::memcmp(a.bytes,b.bytes,sizeof ram)||
           a.cycles!=b.cycles||a.fraction!=b.fraction||std::memcmp(a.entries,b.entries,sizeof a.entries)||
           a.replacements!=b.replacements||a.instructions!=b.instructions||a.samples!=b.samples||a.receipts!=b.receipts||a.edges!=b.edges||
           a.writes!=b.writes||std::memcmp(a.history,b.history,a.writes*sizeof history[0]))
            {std::fprintf(stderr,"FAIL emitted owner/clock/hook product mode%u\n",mode);std::abort();}
        if(!equal(a.observed_sp,b.observed_sp)||a.observed_sp_source!=b.observed_sp_source||
           a.observed_sp_precise!=b.observed_sp_precise||a.observed_sp_scalar!=b.observed_sp_scalar||
           a.sp_precise!=b.sp_precise||a.sp_scalar!=b.sp_scalar) {
            std::fputs("FAIL incoming affine receipt at actual observation/caller boundary\n",stderr);std::abort();
        }
        if(mode==1 && (!b.sp_precise||b.sp_scalar!=double((int32_t)b.cpu.gpr[29])+.75))std::abort();
        for(unsigned r=0;r<32;++r)if(!equal(a.gprs[r],b.gprs[r])||a.source[r]!=b.source[r])
            {std::fprintf(stderr,"FAIL emitted receipt mode%u r%u\n",mode,r);std::abort();}
        if(mode==0||mode==1) {
            if(b.entries[0]!=1||b.entries[1]!=1||b.replacements!=2||b.instructions!=1||b.owners!=3||b.alu>=a.alu)std::abort();
        }
        if(!b.cycles)std::abort();
        if(mode==1 && (b.samples!=1||b.receipts!=1||!b.fraction))std::abort();
        if(mode==2 && (b.entries[2]!=1||b.replacements!=1||b.edges!=1||b.alu>=a.alu)) {
            std::fprintf(stderr,"FAIL loop entry%u replacements%u edges%u ALU%llu/%llu\n",b.entries[2],b.replacements,b.edges,
                (unsigned long long)a.alu,(unsigned long long)b.alu);std::abort();
        }
        if(mode==4 && (b.entries[0]||b.replacements||b.alu!=a.alu)) {
            std::fprintf(stderr,"FAIL public entry hook%u replacements%u ALU%llu/%llu\n",b.entries[0],b.replacements,
                (unsigned long long)a.alu,(unsigned long long)b.alu);std::abort();
        }
        std::printf("PASS emitted regions mode%u clock%llu frac%u ALU%llu/%llu\n",mode,
            (unsigned long long)b.cycles,b.fraction,(unsigned long long)a.alu,(unsigned long long)b.alu);
    }
    run_region(region_candidate,true,3);
    if(region_candidate.cpu.pc!=0x80055500u||region_candidate.cpu.gpr[2]!=0x55aau||!g_psx_call_bail||
       region_candidate.entries[2]!=1||region_candidate.edges!=1)std::abort();
    std::puts("PASS real emitted owner/CPS private selection, canonical public entry, first-block hook-once escape, fractional precharge sample, redirect/bail");
}
/* One finite boundary-cost decision, separate from the correctness entrypoint.
 * These are the real hot receipt masks in normally emitted Ape80044EF4:
 * 45794 LUIat=80000000; 44F54 ADDIUv0=2; 44F54/5C ADDIUv0+LUIv1.
 * The v0 case includes one compatible carrying edge and its real precharge
 * preview; the others end at the real incompatible-join receipt shape. */
static void cost_receipt_2(CPUState* cpu) {psx_pgxp_region_materialize(cpu,2,2,2,2,2);}
static void cost_receipt_4(CPUState* cpu) {psx_pgxp_region_materialize(cpu,4,0,0,0,0);}
static void cost_receipt_c(CPUState* cpu) {psx_pgxp_region_materialize(cpu,12,8,8,8,8);}
static uint64_t thread_cpu_ns() {
#if defined(_WIN32)
    FILETIME created,exited,kernel,user;
    if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))std::abort();
    return ((((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime)+
            (((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime))*100u;
#else
    timespec time{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&time))std::abort();
    return (uint64_t)time.tv_sec*1000000000u+(uint64_t)time.tv_nsec;
#endif
}
template<unsigned Kind,bool Native> static void cost_body(CPUState* cpu,uint32_t count) {
    for(uint32_t i=0;i<count;++i) {
        psx_native_span_charge(2u);
        if constexpr(Kind==0) {
            cpu->gpr[1]=0x80000000u;
            if constexpr(!Native)psx_pgxp_alu(cpu,0x3c018000u,cpu->gpr[1],0,0x8000);
        } else {
            cpu->gpr[2]=2u;
            if constexpr(!Native)psx_pgxp_alu(cpu,0x24020002u,2,0,2);
            if constexpr(Kind==2) {
                cpu->gpr[3]=0x800f0000u;
                if constexpr(!Native)psx_pgxp_alu(cpu,0x3c03800fu,cpu->gpr[3],0,0x800f);
            }
        }
        auto receipt=Kind==0?cost_receipt_2:Kind==1?cost_receipt_4:cost_receipt_c;
        if constexpr(Kind==1) {
            if constexpr(Native) {
                if(psx_block_edge_materialize(cpu,0x80045040u,receipt)||
                   psx_native_span_charge_observable(2u))std::abort();
            } else PSX_BLOCK_EDGE(cpu,0x80045040u);
            psx_native_span_charge(2u);
        }
        if constexpr(Native) {
            if(psx_block_edge_materialize(cpu,0x800457a4u,receipt))std::abort();
            receipt(cpu);
        } else PSX_BLOCK_EDGE(cpu,0x800457a4u);
    }
}
struct BoundaryCost {uint64_t cpu_ns,cycles,edges,alu;unsigned writes;};
template<unsigned Kind,bool Native> static BoundaryCost cost_arm(uint32_t count) {
    CPUState cpu{};region_cpu=live_cpu=&cpu;history_count=0;std::memset(ram,0,sizeof ram);
    cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;cpu.read_byte=psx_read_byte;
    cpu.write_word=psx_write_word;cpu.write_half=psx_write_half;cpu.write_byte=psx_write_byte;
    pgxp_set_enabled(1);pgxp_set_cpu_mode(1);pgxp_set_source_tracking(1);pgxp_invalidate_all();
    cpu.gpr[1]=0x80000000u;cpu.gpr[2]=2u;cpu.gpr[3]=0x800f0000u;
    for(unsigned r=1;r<=3;++r)if(!pgxp_native_scalar_gpr(&cpu,r,(int32_t)cpu.gpr[r]+.25,1))std::abort();
    g_psx_call_bail=g_psx_precise_slice=g_psx_guest_time_frozen=0;
    g_event_step_conservative=psx_in_device_service=g_psx_render_pass_active=0;
    g_ls_replay_active=g_ls_mode=g_ds_recording=g_host_store_depth=g_ram_read_watch_active=g_dma_exec_depth=0;
    g_psx_cyc_batch=0;g_psx_cyc_local_acc=nullptr;g_psx_cyc_bb_defer=0;g_overlay_flush_pending_cycles=nullptr;
    g_psx_gcs_recip_q16=32768u;g_psx_gcs_frac=0;psx_cycle_count=0;
    psx_next_service_cycle=psx_next_cycle_sample=UINT64_MAX;
    psx_defer_switch_pending=g_psx_savestate_pending=psx_observation_wait_active=g_ls_suppress_record=0;
    i_stat=i_mask=0;region_mode=0;region_sample=region_edges=0;
    g_debug_last_store_pc=Kind==0?0x80045794u:0x80044f54u;frame_count=1;
    pgxp_set_trace_sources(&g_debug_last_store_pc,&frame_count);
    PGXPHookStats before{},after{};pgxp_get_hook_stats(&before);
    const uint64_t first_edge=psx_observation_block_edges,begin=thread_cpu_ns();
    cost_body<Kind,Native>(&cpu,count);
    const uint64_t end=thread_cpu_ns();pgxp_get_hook_stats(&after);
    BoundaryCost result{end-begin,psx_cycle_count,psx_observation_block_edges-first_edge,after.alu-before.alu,0};
    const uint64_t expected_edges=(Kind==1?2u:1u)*(uint64_t)count;
    if(result.edges!=expected_edges||result.cycles!=expected_edges||region_edges||region_sample||
       result.alu!=(Native?0u:(Kind==2?2u:1u)*(uint64_t)count))std::abort();
    if constexpr(Kind==0)region_receipt(&cpu,1,3);
    else {region_receipt(&cpu,2,0);if constexpr(Kind==2)region_receipt(&cpu,3,3);}
    for(unsigned r=Kind==0?1:2;r<=(Kind==2?3:Kind==0?1:2);++r) {
        double scalar;if(pgxp_native_gpr_scalar(r,cpu.gpr[r],&scalar))std::abort();
    }
    result.writes=history_count;return result;
}
static int boundary_cost() {
    constexpr uint32_t count=1u<<22;constexpr unsigned rounds=3;
    uint64_t total[3][2]{};
    std::puts("SCOPE component only: real CPU-mode PGXP receipts plus quiet edges; Ape2x guest scaling; masks2/4/C; no guest-memory/device workload in timed interval; full providers/history remain enabled");
    for(unsigned round=0;round<rounds;++round)for(unsigned kind=0;kind<3;++kind) {
        BoundaryCost result[2]{};const bool reverse=(round+kind)&1u;
        for(unsigned arm=0;arm<2;++arm) {
            const bool native=bool(arm)^reverse;
            if(kind==0)result[native]=native?cost_arm<0,true>(count):cost_arm<0,false>(count);
            if(kind==1)result[native]=native?cost_arm<1,true>(count):cost_arm<1,false>(count);
            if(kind==2)result[native]=native?cost_arm<2,true>(count):cost_arm<2,false>(count);
        }
        if(result[0].cycles!=result[1].cycles||result[0].edges!=result[1].edges||result[0].writes!=result[1].writes)std::abort();
        for(unsigned arm=0;arm<2;++arm)total[kind][arm]+=result[arm].cpu_ns;
        std::printf("COST round%u mask0x%X iterations%u order%s canonical_cpu_ns%llu native_cpu_ns%llu cycles%llu edges%llu ALU%llu/%llu retained_probe_writes%u\n",
            round,kind==0?2:kind==1?4:12,count,reverse?"NC":"CN",(unsigned long long)result[0].cpu_ns,
            (unsigned long long)result[1].cpu_ns,(unsigned long long)result[0].cycles,(unsigned long long)result[0].edges,
            (unsigned long long)result[0].alu,(unsigned long long)result[1].alu,result[0].writes);std::fflush(stdout);
    }
    for(unsigned kind=0;kind<3;++kind) {
        if(!total[kind][0]||!total[kind][1])std::abort();
        std::printf("COST_TOTAL mask0x%X logical_operations%llu canonical_cpu_ns%llu native_cpu_ns%llu delta_percent%.6f\n",
            kind==0?2:kind==1?4:12,(unsigned long long)count*rounds,(unsigned long long)total[kind][0],
            (unsigned long long)total[kind][1],100.*((double)total[kind][1]/total[kind][0]-1.));
    }
    std::puts("PASS finite interleaved real receipt/edge cost; correctness/clock/count/history checks retained; no game-performance or global zero-hook claim");return 0;
}

#endif
int main(int argc,char** argv) {
#if defined(PSX_TEST_PGXP_REGIONS)
    if(argc==2 && !std::strcmp(argv[1],"--cost"))return boundary_cost();
#else
    (void)argc;(void)argv;
#endif
#if !defined(PSX_TEST_PGXP_REGIONS_REMAINING)
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
#endif
#if defined(PSX_TEST_PGXP_REGIONS)
    region_products();
#if defined(PSX_TEST_GEOMETRY_CONTROL)
    control_products();
    retained_partial_store_products();
#if defined(PSX_TEST_GEOMETRY_SCRATCH)
    scratch_products();
#endif
#endif
#endif
#if !defined(PSX_TEST_PGXP_REGIONS_REMAINING)
    std::puts("PASS:38 generated GTE/CPU/memory/packet+74precision/lineage products+ordered write/store records (aliases,stale sources,partial/delayed loads)");
#endif
}
