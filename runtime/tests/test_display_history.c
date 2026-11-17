/* The runner includes the production display-ring adapter unchanged. */
#include "gpu.h"
#include "gpu_timeline.h"
#include "presentation_gpu.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PSX_BSS
static uint64_t s_frame_count;
static uint16_t cpu_vram[1024*512];
static uint16_t gpu_word;
static unsigned peeks,exports,releases;
static int supported=1,queued,fail_capture,gl_available=1;
static GpuDisplayInfo display;
typedef struct { PsxPresentationSnapshotReady ready;void *user;uint64_t id; } Pending;
static Pending pending[128];
static unsigned pending_count;
static struct { uint16_t word;int live; } versions[4096];
static uint64_t next_id;
static void (*preserve)(void);
void gpu_get_display_info(GpuDisplayInfo *out) { *out=display; }
const uint16_t *gpu_get_vram(void) { return cpu_vram; }
void gpu_timeline_note(uint8_t type,uint32_t path,uint32_t arg) {
    (void)type;(void)path;(void)arg;
}
int gl_renderer_fbo_peek(int x,int y,int w,int h,uint16_t *out) {
    (void)x;(void)y;++peeks;
    if(!gl_available)return 0;
    for(int i=0;i<w*h;++i)out[i]=gpu_word;
    return 1;
}
int gl_renderer_fbo_peek_deferred(int x,int y,int w,int h,uint16_t *out) {
    return gl_renderer_fbo_peek(x,y,w,h,out);
}
void psx_presentation_gpu_release(uint64_t id) {
    assert(id && versions[id].live);versions[id].live=0;++releases;
}
int psx_presentation_gpu_history_replace_async(uint64_t old,
    PsxPresentationSnapshotReady ready,void *user) {
    if(!supported)return 0;
    if(old)psx_presentation_gpu_release(old);
    uint64_t id=0;
    if(!fail_capture) {
        id=++next_id;assert(id<4096);
        versions[id].live=1;versions[id].word=gpu_word;
    }
    if(queued)pending[pending_count++]=(Pending){ready,user,id};
    else ready(user,id);
    return 1;
}
int psx_presentation_gpu_snapshot_read_vram(uint64_t id,uint16_t *out) {
    assert(id && versions[id].live);++exports;
    for(unsigned i=0;i<1024*512;++i)out[i]=versions[id].word;
    return 1;
}
void psx_presentation_gpu_history_set_shutdown(void (*fn)(void)) { preserve=fn; }

#include "display_history.inc"

static void capture(uint32_t frame,uint16_t word) {
    s_frame_count=frame;gpu_word=word;debug_server_record_display_frame();
}
int main(void) {
    display.width=320;display.height=240;
    for(unsigned i=0;i<1024*512;++i)cpu_vram[i]=0x1234;
    for(unsigned i=0;i<64;++i)capture(i,(uint16_t)(0x4000u+i));
#if PSX_GPU_DISPLAY_HISTORY
    assert(peeks==0 && exports==0);
#else
    assert(peeks==64 && exports==0);
#endif
    assert(disp_ring_materialize(&s_disp_ring[10]));
    for(unsigned i=0;i<1024*512;++i)assert(s_disp_ring[10].vram[i]==0x400a);
    assert(disp_ring_materialize(&s_disp_ring[10]));
#if PSX_GPU_DISPLAY_HISTORY
    assert(exports==1);
#endif
    s_capture_frozen=1;capture(100,9);s_capture_frozen=0;
    assert(s_disp_ring[36].frame==36);
    capture(64,0x77);assert(s_disp_ring[0].frame==64);
    assert(disp_ring_materialize(&s_disp_ring[0]) && s_disp_ring[0].vram[0]==0x77);
    display.depth24=1;capture(65,0x99);display.depth24=0;
    assert(s_disp_ring[1].vram[0]==0x1234);
    supported=0;capture(66,0x88);supported=1;
    assert(s_disp_ring[2].vram[0]==0x88);
    supported=0;gl_available=0;capture(67,0x66);gl_available=1;supported=1;
    assert(s_disp_ring[3].vram[0]==0x1234);
#if PSX_GPU_DISPLAY_HISTORY
    fail_capture=1;capture(68,0x55);fail_capture=0;
    assert(s_disp_ring[4].valid && s_disp_ring[4].vram[0]==0x55);
    queued=1;
    capture(128,0x18);capture(192,0x19);
    assert(pending_count==2);
    const uint64_t obsolete=pending[0].id;
    pending[0].ready(pending[0].user,pending[0].id);
    assert(!versions[obsolete].live && !s_disp_ring[0].valid);
    pending[1].ready(pending[1].user,pending[1].id);
    queued=0;pending_count=0;
    assert(s_disp_ring[0].valid && s_disp_ring[0].frame==192);
    assert(disp_ring_materialize(&s_disp_ring[0]) && s_disp_ring[0].vram[0]==0x19);
    assert(preserve);preserve();
    for(unsigned i=0;i<64;++i)assert(!s_disp_ring[i].gpu_version);
    memset(versions,0,sizeof versions); /* old GL context is gone */
    capture(193,0x20);
    assert(disp_ring_materialize(&s_disp_ring[1]) && s_disp_ring[1].vram[0]==0x20);
    assert(disp_ring_materialize(&s_disp_ring[10]) && s_disp_ring[10].vram[0]==0x400a);
#endif
    for(unsigned i=0;i<1024*512;++i)assert(cpu_vram[i]==0x1234);
    free(s_disp_ring_px);
    puts("display history capture/export, freeze, eviction, stale and fallback passed");
    return 0;
}
