#include "headroom_counters.h"
#include "psx_bss.h"
#include "psx_cycles.h"
#include "psx_direct_memory.h"
#include "psx_static_calls.h"
#include "dirty_ram_interp.h"
#include "pgxp.h"
#include "dma.h"
#include <stddef.h>
#include <string.h>

#define PSX_HR_NAME(id, name) #name,
const char *const psx_headroom_field_names[PSX_HR_COUNT]={PSX_HR_FIELDS(PSX_HR_NAME)};
#undef PSX_HR_NAME
_Static_assert(PSX_HR_COUNT<=128,"reset mask covers every counter");
extern uint64_t s_frame_count,g_disp_tail_seq,g_psx_owner_dispatch_count;
extern uint64_t g_spu_render_blocks,g_spu_sample_deadline_queries;
extern uint64_t g_spu_sample_service_checks,g_spu_sample_service_pumps;
extern void psx_interrupt_check_path_diag(uint64_t*,uint64_t*,uint64_t*,uint64_t*,uint64_t*,uint64_t*);
#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
extern uint64_t psx_observation_block_edges,psx_observation_slow_edges;
#endif

void psx_headroom_totals(PsxHeadroomFrame *out) {
    if(!out)return;
    memset(out,0,sizeof *out);
    out->frame=s_frame_count;out->cycles=psx_get_cycle_count();
    uint64_t *v=out->values;
#if defined(PSX_NATIVE_TAIL_CHAINS) && PSX_NATIVE_TAIL_CHAINS
    v[PSX_HR_DISPATCH]=g_psx_owner_dispatch_count;
#else
    v[PSX_HR_DISPATCH]=g_disp_tail_seq;
#endif
    psx_interrupt_check_path_diag(&v[PSX_HR_CHECK_IRQ],&v[PSX_HR_IRQ_FAST_SR],
        &v[PSX_HR_IRQ_FAST_NONE],&v[PSX_HR_IRQ_MID],&v[PSX_HR_IRQ_EVAL],&v[PSX_HR_IRQ_DELIVER]);
    v[PSX_HR_CHECK_IRQ_AT]=psx_hr_interrupt_at_calls;
#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
    v[PSX_HR_BLOCK_EDGES]=psx_observation_block_edges;
    v[PSX_HR_SLOW_EDGES]=psx_observation_slow_edges;
#else
    v[PSX_HR_BLOCK_EDGES]=v[PSX_HR_CHECK_IRQ];
    v[PSX_HR_SLOW_EDGES]=v[PSX_HR_CHECK_IRQ];
#endif
    v[PSX_HR_DEV_SVC]=psx_device_service_calls;
    memcpy(v+PSX_HR_SVC_OTHER,psx_hr_service_requests,sizeof psx_hr_service_requests);
    memcpy(v+PSX_HR_DL_VBLANK,psx_hr_service_deadlines,sizeof psx_hr_service_deadlines);
    PSXDirectMemoryStats memory;psx_direct_memory_get_stats(&memory);
    _Static_assert(sizeof memory==12u*sizeof(uint64_t),"memory getter field contract");
    memcpy(v+PSX_HR_LOAD_INLINE_RAM,&memory,sizeof memory);
    PGXPHookStats pgxp;pgxp_get_hook_stats(&pgxp);
    /* M0 history owns the established 12-field prefix. Later diagnostics
     * stay in pgxp_hook_stats; they must not spill into the DMA lanes. */
#define PSX_HR_PGXP_PREFIX_FIELD(member, id) \
    _Static_assert(offsetof(PGXPHookStats,member)== \
        (PSX_HR_##id-PSX_HR_PGXP_ALU)*sizeof(uint64_t),"precision prefix order"); \
    _Static_assert(sizeof pgxp.member==sizeof(uint64_t),"precision prefix width")
    PSX_HR_PGXP_PREFIX_FIELD(alu,PGXP_ALU);
    PSX_HR_PGXP_PREFIX_FIELD(load,PGXP_LOAD);
    PSX_HR_PGXP_PREFIX_FIELD(store,PGXP_STORE);
    PSX_HR_PGXP_PREFIX_FIELD(cop2,PGXP_COP2);
    PSX_HR_PGXP_PREFIX_FIELD(muldiv,PGXP_MULDIV);
    PSX_HR_PGXP_PREFIX_FIELD(plain_batches,PGXP_BATCHES);
    PSX_HR_PGXP_PREFIX_FIELD(plain_registers,PGXP_REGISTERS);
    PSX_HR_PGXP_PREFIX_FIELD(native_words_captured,PGXP_CAPTURED);
    PSX_HR_PGXP_PREFIX_FIELD(native_words_published,PGXP_PUBLISHED);
    PSX_HR_PGXP_PREFIX_FIELD(native_sxy_imports,PGXP_SXY);
    PSX_HR_PGXP_PREFIX_FIELD(native_clobbers,PGXP_CLOBBER);
    PSX_HR_PGXP_PREFIX_FIELD(native_gpr_imports,PGXP_GPR);
#undef PSX_HR_PGXP_PREFIX_FIELD
    _Static_assert(offsetof(PGXPHookStats,operations)==12u*sizeof(uint64_t),"precision prefix size");
    _Static_assert(PSX_HR_DMA_HOLDS-PSX_HR_PGXP_ALU==12,"precision history lanes contiguous");
    memcpy(v+PSX_HR_PGXP_ALU,&pgxp,offsetof(PGXPHookStats,operations));
    DMAGpuHoldStats hold;dma_debug_get_gpu_hold(&hold,NULL);
    _Static_assert(sizeof hold==8u*sizeof(uint64_t),"hold getter field contract");
    memcpy(v+PSX_HR_DMA_HOLDS,&hold,sizeof hold);
    DMAGpuNativeStats native;dma_debug_get_gpu_native(&native);
    _Static_assert(sizeof native==7u*sizeof(uint64_t),"native list getter field contract");
    memcpy(v+PSX_HR_NATIVE_LISTS,&native,sizeof native);
    PsxStaticCallStats calls;psx_static_call_get_stats(&calls);
    _Static_assert(sizeof calls==5u*sizeof(uint64_t),"static entry getter field contract");
    memcpy(v+PSX_HR_VALID_QUERIES,&calls,sizeof calls);
    v[PSX_HR_MEMCMP]=dirty_ram_text_compare_calls();
    v[PSX_HR_SPU_BLOCKS]=g_spu_render_blocks;
    v[PSX_HR_SPU_QUERIES]=g_spu_sample_deadline_queries;
    v[PSX_HR_SPU_CHECKS]=g_spu_sample_service_checks;
    v[PSX_HR_SPU_PUMPS]=g_spu_sample_service_pumps;
    PSXMemoryWidthStats widths;psx_memory_get_width_stats(&widths);
    _Static_assert(sizeof widths==36u*sizeof(uint64_t),"memory width getter field contract");
    memcpy(v+PSX_HR_MEM_INLINE_LOAD_RAM_U8,&widths,sizeof widths);
    memcpy(v+PSX_HR_MEM_RUNTIME_FAST_LOAD_RAM_U8,psx_hr_runtime_fast_loads,sizeof psx_hr_runtime_fast_loads);
    v[PSX_HR_SVC_GPU_IMPLICIT]=psx_hr_gpu_hold_implicit_services;
    memcpy(v+PSX_HR_MEM_CALLBACK_LOAD_U8,psx_hr_bus_callbacks,sizeof psx_hr_bus_callbacks);
}

#if !defined(PSX_DISABLE_HEADROOM_COUNTERS) || !PSX_DISABLE_HEADROOM_COUNTERS
static PSX_BSS PsxHeadroomFrame s_ring[PSX_HEADROOM_CAP];
static uint64_t s_previous[PSX_HR_COUNT],s_sequence;
void psx_headroom_record_frame(void) {
    PsxHeadroomFrame *out=&s_ring[s_sequence&(PSX_HEADROOM_CAP-1u)];
    psx_headroom_totals(out);out->seq=s_sequence++;
    for(unsigned i=0;i<PSX_HR_COUNT;++i) {
        uint64_t total=out->values[i];
        if(total<s_previous[i])out->resets[i/64]|=UINT64_C(1)<<(i%64);
        else out->values[i]-=s_previous[i];
        s_previous[i]=total;
    }
}
uint64_t psx_headroom_sequence(void) { return s_sequence; }
int psx_headroom_get(uint64_t seq,PsxHeadroomFrame *out) {
    if(!out || seq>=s_sequence || s_sequence-seq>PSX_HEADROOM_CAP)return 0;
    *out=s_ring[seq&(PSX_HEADROOM_CAP-1u)];return 1;
}
int psx_headroom_enabled(void) { return 1; }
#else
void psx_headroom_record_frame(void) {}
uint64_t psx_headroom_sequence(void) { return 0; }
int psx_headroom_get(uint64_t seq,PsxHeadroomFrame *out) { (void)seq;(void)out;return 0; }
int psx_headroom_enabled(void) { return 0; }
#endif
