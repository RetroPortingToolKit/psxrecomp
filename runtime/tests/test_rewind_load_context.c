/* Actual rewind request/poll, actual interrupt continuation gate and actual
 * rbengine ring. The serializer is a counted two-register machine double:
 * checked-load rejection intentionally happens AFTER it changes the CPU. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "interrupts.c"
#include "psx_rewind.c"
#include "pst_wire.h"
#include <assert.h>

static CPUState machine;
static int function_entry_active, provider_ready = 1, capture_fails;
static int capture_calls, load_attempts, quiet_rollbacks, observations;
static int frontend_restores, cycle_restores, gpu_present_resets, cd_restores, resumes;
static jmp_buf resumed;
int g_cosim_dirty_pump_site, g_call_unit_depth, g_psx_dispatch_depth;
uint32_t g_dirty_safe_resume_pc;
int g_ls_suppress_record;

int psx_mod_function_entry_active(void) { return function_entry_active; }
int psx_mod_snapshot_ready(void) { return provider_ready; }
int psx_netplay_active(void) { return 0; }
int psx_hle_scheduler_enabled(void) { return 1; }
int psx_is_dispatchable(uint32_t pc) { return pc == 0x80025070u || pc == 0x80025074u; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
uint32_t sio_get_seq(void) { return 0; }
uint64_t psx_get_cycle_count(void) { return 0; }
void gpu_vblank_clear_deferred_present(void) { ++gpu_present_resets; }
void gpu_get_display_info(GpuDisplayInfo *out) { memset(out,0,sizeof(*out)); out->disabled=1; }
uint32_t gpu_display_pixel_argb(const GpuDisplayInfo *out,uint32_t x,uint32_t y) {
    (void)out; (void)x; (void)y; assert(0); return 0;
}
void host_osd_push(const char *text,int ms) { (void)text; (void)ms; }
void psx_cycles_resync_after_restore(CPUState *cpu) { assert(cpu==&machine); ++cycle_restores; }
void cdrom_accelerate_after_savestate(void) { ++cd_restores; }
void psx_frontend_on_savestate_loaded(void) { ++frontend_restores; }
void psx_scheduler_resume_at(uint32_t pc) {
    assert(pc==machine.pc && psx_is_dispatchable(pc)); ++resumes; longjmp(resumed,1);
}

static uint8_t *machine_image(uint32_t pc,uint32_t value) {
    uint8_t *data=malloc(8); PstW w; assert(data);
    pst_w_init(&w,data,8); assert(pst_w_u32(&w,pc) && pst_w_u32(&w,value));
    return data;
}
int boot_state_save_buffer_raw(const CPUState *cpu,uint32_t bios,uint32_t entry,
                               uint8_t **data,size_t *len) {
    assert(bios==123 && entry==0x801b0000u); ++capture_calls;
    *data=NULL; *len=0;
    if(capture_fails)return 0;
    *data=machine_image(cpu->pc,cpu->gpr[4]); *len=8; return 1;
}
static int restore_image(const uint8_t *data,size_t len,CPUState *cpu) {
    PstR r; uint32_t pc,value;
    if(!data || len!=8)return 0;
    pst_r_init(&r,data,len);
    if(!pst_r_u32(&r,&pc) || !pst_r_u32(&r,&value))return 0;
    cpu->pc=pc; cpu->gpr[4]=value; return 1;
}
int boot_state_load_buffer_checked(const uint8_t *data,size_t len,uint32_t bios,
                                   uint32_t entry,CPUState *cpu,BootStateResumeAccept accept) {
    assert(bios==123 && entry==0x801b0000u); ++load_attempts;
    if(!restore_image(data,len,cpu) || (accept && !accept(cpu->pc)))return 0;
    ++observations; return 1;
}
int boot_state_load_buffer_quiet(const uint8_t *data,size_t len,uint32_t bios,
                                 uint32_t entry,CPUState *cpu) {
    assert(bios==123 && entry==0x801b0000u); ++quiet_rollbacks;
    return restore_image(data,len,cpu);
}

static void capture(uint32_t tick,uint32_t value) {
    machine.pc=0x80025070u; machine.gpr[4]=value;
    s_frame=tick; s_capture_due=1;
    psx_rewind_poll(&machine,machine.pc);
    assert(!s_capture_due && s_last_capture_frame==tick && rbe_snap_ring_has(s_ring,tick));
}
static void choose_first(void) {
    assert(psx_rewind_toggle() && s_open);
    while(s_sel>0)psx_rewind_move(-1);
    assert(psx_rewind_accept() && s_load_pending && s_load_tick==10);
}
static void assert_deferred(void) {
    psx_rewind_poll(&machine,machine.pc);
    assert(s_load_pending && s_load_tick==10 && s_frame==20 && s_count==2);
    assert(rbe_snap_ring_has(s_ring,10) && rbe_snap_ring_has(s_ring,20));
    assert(machine.pc==0x80025074u && machine.gpr[4]==303);
    assert(capture_calls==2 && !load_attempts && !quiet_rollbacks && !observations && !resumes);
}

int main(void) {
    s_enabled=1; /* Ignore the runner's optional rewind environment setting. */
    psx_rewind_configure(123,0x801b0000u);
    assert(psx_rewind_enabled());
    capture(10,101); capture(20,202);
    choose_first(); machine.pc=0x80025074u; machine.gpr[4]=303;
    /* The title is ready, but a registered native callback is still live. */
    function_entry_active=1; assert_deferred(); function_entry_active=0;
    /* Both layers of a nested non-resumable adapter must unwind. */
    psx_snapshot_host_call_begin(); psx_snapshot_host_call_begin(); assert_deferred();
    psx_snapshot_host_call_end(); assert_deferred(); psx_snapshot_host_call_end();
    /* A plausible PC from an unretired interpreter site is not a safe load. */
    g_cosim_dirty_pump_site=5; g_dirty_safe_resume_pc=machine.pc;
    assert_deferred(); g_cosim_dirty_pump_site=0; g_dirty_safe_resume_pc=0;
    /* Title-specific projection readiness remains an independent condition. */
    provider_ready=0; assert_deferred(); provider_ready=1;
    if(setjmp(resumed)==0) {
        psx_rewind_poll(&machine,machine.pc); assert(0);
    }
    assert(!s_load_pending && machine.pc==0x80025070u && machine.gpr[4]==101);
    assert(load_attempts==1 && capture_calls==3 && !quiet_rollbacks && observations==1 && resumes==1);
    assert(frontend_restores==1 && cycle_restores==1 && gpu_present_resets==1 && cd_restores==1);
    assert(s_frame==10 && s_count==1 && !rbe_snap_ring_has(s_ring,20));

    /* Checked load changes the machine before it discovers a bad resume PC.
     * Rewind must quietly restore its rollback and retain the selected history. */
    assert(rbe_snap_ring_store(s_ring,10,machine_image(0xfffffffcu,999),8));
    machine.pc=0x80025074u; machine.gpr[4]=303; choose_first();
    psx_rewind_poll(&machine,machine.pc);
    assert(!s_load_pending && machine.pc==0x80025074u && machine.gpr[4]==303);
    assert(load_attempts==2 && quiet_rollbacks==1 && observations==1 && resumes==1);
    assert(s_count==1 && s_frame==10 && rbe_snap_ring_has(s_ring,10));
    assert(frontend_restores==1 && cycle_restores==1 && gpu_present_resets==1 && cd_restores==1);
    /* Failure to obtain rollback refuses before the target ever applies. */
    machine.gpr[4]=404; choose_first(); capture_fails=1;
    psx_rewind_poll(&machine,machine.pc);
    assert(machine.pc==0x80025074u && machine.gpr[4]==404);
    assert(load_attempts==2 && quiet_rollbacks==1 && observations==1 && resumes==1);
    assert(s_count==1 && s_frame==10 && rbe_snap_ring_has(s_ring,10));
    psx_rewind_shutdown();
    puts("rewind loads defer live host stacks and quietly undo rejected resume PCs");
    return 0;
}
