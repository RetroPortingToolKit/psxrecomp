/* Production history/RPC with the maintained getter interfaces as seams. */
#include "headroom_counters.h"
#include "psx_direct_memory.h"
#include "psx_static_calls.h"
#include "pgxp.h"
#include "dma.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint64_t s_frame_count,g_disp_tail_seq,psx_device_service_calls;
uint64_t psx_hr_service_requests[PSX_HR_REQUEST_COUNT];
uint64_t psx_hr_service_deadlines[PSX_HR_DEADLINE_COUNT];
uint64_t psx_hr_interrupt_at_calls=9;
uint64_t psx_observation_block_edges=1000,psx_observation_slow_edges=20;
uint64_t g_spu_render_blocks=8,g_spu_sample_deadline_queries=9;
uint64_t g_spu_sample_service_checks=10,g_spu_sample_service_pumps=11;
uint64_t psx_get_cycle_count(void) { return s_frame_count*100u; }
static void fill(void *out,unsigned n,uint64_t base) {
    uint64_t values[12];assert(n<=12);
    for(unsigned i=0;i<n;++i)values[i]=base+i;
    memcpy(out,values,n*sizeof *values);
}
void psx_direct_memory_get_stats(PSXDirectMemoryStats *out) { fill(out,12,100); }
void pgxp_get_hook_stats(PGXPHookStats *out) { fill(out,12,200); }
void dma_debug_get_gpu_hold(DMAGpuHoldStats *out,const DMAGpuHoldEntry **ring) { assert(!ring);fill(out,8,300); }
void dma_debug_get_gpu_native(DMAGpuNativeStats *out) { fill(out,7,400); }
void psx_static_call_get_stats(PsxStaticCallStats *out) { fill(out,5,500); }
uint64_t dirty_ram_text_compare_calls(void) { return 600; }
void psx_interrupt_check_path_diag(uint64_t *entry,uint64_t *sr,uint64_t *none,
    uint64_t *mid,uint64_t *eval,uint64_t *delivery) {
    *entry=7;*sr=2;*none=3;*mid=4;*eval=5;*delivery=6;
}
static char response[512*1024];
static int json_get_int(const char *json,const char *key,int fallback) {
    char word[64];snprintf(word,sizeof word,"\"%s\"",key);
    const char *found=strstr(json,word);
    if(!found)return fallback;
    found=strchr(found,':');return found?(int)strtol(found+1,NULL,10):fallback;
}
static void send_fmt(const char *fmt,...) {
    va_list args;va_start(args,fmt);
    int n=vsnprintf(response,sizeof response,fmt,args);va_end(args);
    assert(n>0 && (size_t)n<sizeof response);
}
static void send_err(int id,const char *error) { (void)id;(void)error;abort(); }
#include "headroom_rpc.inc"
int main(void) {
    PsxHeadroomFrame out;g_disp_tail_seq=5;psx_headroom_totals(&out);
    assert(out.values[PSX_HR_DISPATCH]==5 && out.values[PSX_HR_CHECK_IRQ]==7);
#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
    assert(out.values[PSX_HR_BLOCK_EDGES]==1000 && out.values[PSX_HR_SLOW_EDGES]==20);
#else
    assert(out.values[PSX_HR_BLOCK_EDGES]==7 && out.values[PSX_HR_SLOW_EDGES]==7);
#endif
    for(unsigned i=0;i<12;++i)assert(out.values[PSX_HR_LOAD_INLINE_RAM+i]==100+i);
    for(unsigned i=0;i<12;++i)assert(out.values[PSX_HR_PGXP_ALU+i]==200+i);
    for(unsigned i=0;i<8;++i)assert(out.values[PSX_HR_DMA_HOLDS+i]==300+i);
    for(unsigned i=0;i<7;++i)assert(out.values[PSX_HR_NATIVE_LISTS+i]==400+i);
    for(unsigned i=0;i<5;++i)assert(out.values[PSX_HR_VALID_QUERIES+i]==500+i);
    assert(out.values[PSX_HR_MEMCMP]==600 && out.values[PSX_HR_SPU_PUMPS]==11);
    for(unsigned i=0;i<PSX_HEADROOM_CAP+17;++i) {
        s_frame_count=i%100;g_disp_tail_seq+=3;PSX_HEADROOM_RECORD_FRAME();
    }
#if defined(PSX_DISABLE_HEADROOM_COUNTERS) && PSX_DISABLE_HEADROOM_COUNTERS
    assert(!psx_headroom_enabled() && psx_headroom_sequence()==0 && !psx_headroom_get(0,&out));
#else
    assert(psx_headroom_enabled() && psx_headroom_sequence()==PSX_HEADROOM_CAP+17);
    assert(!psx_headroom_get(16,&out));
    assert(psx_headroom_get(17,&out) && out.seq==17 && out.frame==17 && out.values[PSX_HR_DISPATCH]==3);
    g_disp_tail_seq=1;PSX_HEADROOM_RECORD_FRAME();
    assert(psx_headroom_get(psx_headroom_sequence()-1,&out));
    assert(out.values[PSX_HR_DISPATCH]==1 && (out.resets[0]&(UINT64_C(1)<<PSX_HR_DISPATCH)));
#endif
    assert(!psx_headroom_get(UINT64_MAX,&out) && !psx_headroom_get(0,NULL));
    handle_hr_counters(42,"{\"n\":10000,\"from\":0}");puts(response);
    handle_hr_counters(43,"{\"n\":-1,\"from\":999999}");puts(response);
    puts("PASS: getters, per-frame deltas, eviction, reset and RPC bounds");
    return 0;
}
