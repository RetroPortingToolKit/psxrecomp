/* Render-pass time freeze (psx_cycle_freeze.h psx_cycle_freeze_begin/end).
 * A pass counts guest cycles but never services a device, and afterwards
 * every clock value is exactly as before. Build/run:
 * ctest -R render_pass_freeze_test */

#include "psx_cycles.h"
#include "psx_cycle_freeze.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_ls_replay_active = 0;
int g_ls_mode = 0;
int g_precise_mode = 0;
int g_psx_call_bail = 0;
uint32_t i_mask = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;

/* Mock devices: count every advance and event. The "CD" raises an event
 * every 1000 cycles; the VBlank every 564480. */
static uint64_t s_dev_cycles = 0;
static uint64_t s_dev_calls = 0;
static uint64_t s_cd_events = 0;
static uint32_t s_cd_phase = 0;
static uint64_t s_vblank_phase = 0;

void sio_advance(uint32_t cycles) { (void)cycles; }
void cdrom_advance(uint32_t cycles) {
    s_dev_calls++;
    s_dev_cycles += cycles;
    s_cd_phase += cycles;
    while (s_cd_phase >= 1000u) { s_cd_phase -= 1000u; s_cd_events++; }
}
void dma_advance(uint32_t cycles) { (void)cycles; }
void timers_advance(uint32_t cycles) { (void)cycles; }
void interrupts_advance_cycles(uint32_t cycles) { s_vblank_phase += cycles; }
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) {
    return (uint32_t)(564480u - (s_vblank_phase % 564480u));
}
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) {
    (void)mask;
    return 1000u - s_cd_phase;
}
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return UINT32_MAX; }
void psx_spu_sample_event_service(void) {}
int psx_get_in_exception(void) { return 0; }
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
/* memory.c mod arenas (render_pass_mod_store): nothing journaled here. */
void render_pass_mod_arenas_rollback(void) {}
uint64_t render_pass_mod_arenas_hash(void) { return 0; }
int  psx_netplay_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

static jmp_buf s_jmp;
static int s_overruns;
static void overrun(void) { s_overruns++; longjmp(s_jmp, 1); }

int main(void) {
    PsxCycleFreeze save;
    uint64_t cyc0, next0, dev_calls0, dev_cycles0, cd0;
    uint32_t local = 7;

    psx_cycles_resync_after_restore(NULL);
    psx_advance_cycles(12345);            /* some live history */
    g_psx_cyc_batch = 40;                 /* deferred charges of the guest code */
    g_psx_cyc_local_acc = &local;
    cyc0 = psx_cycle_count;
    next0 = psx_next_service_cycle;
    dev_calls0 = s_dev_calls;
    dev_cycles0 = s_dev_cycles;
    cd0 = s_cd_events;

    CHECK(psx_cycle_freeze_begin(&save, 0, NULL), "freeze begins");
    CHECK(g_psx_render_pass_active == 1, "pass flag set");
    CHECK(!psx_cycle_freeze_begin(&save, 0, NULL), "freezes do not nest");
    CHECK(g_psx_cyc_batch == 0 && g_psx_cyc_local_acc == NULL,
          "the guest's pending charges are parked, not published");
    for (int i = 0; i < 1000; i++) psx_advance_cycles(1000);   /* 10^6 cycles */
    psx_advance_cycles_slow(5000);
    psx_devices_service_to_now();
    psx_devices_mmio_sync();
    CHECK(psx_cycle_count - cyc0 == 1005000u,
          "cycles are still counted inside the pass (GTE/muldiv deadlines)");
    CHECK(s_dev_calls == dev_calls0 && s_dev_cycles == dev_cycles0 &&
          s_cd_events == cd0,
          "no device advanced and no device event fired in a pass");
    psx_cycle_freeze_end(&save);
    CHECK(g_psx_render_pass_active == 0, "pass flag cleared");
    CHECK(psx_cycle_count == cyc0, "cycle count restored exactly");
    CHECK(psx_next_service_cycle == next0, "service deadline restored exactly");
    CHECK(g_psx_cyc_batch == 40 && g_psx_cyc_local_acc == &local && local == 7,
          "the guest's pending charges come back untouched");

    /* The live timeline continues exactly as if the pass never happened. */
    g_psx_cyc_batch = 0;
    g_psx_cyc_local_acc = NULL;
    psx_advance_cycles(3000);
    CHECK(s_cd_events == cd0 + (uint64_t)((12345u % 1000u + 3000u) / 1000u) ||
          s_cd_events >= cd0 + 3u,
          "devices resume on the live clock after the pass");

    uint32_t scale_before[3],scale_after[3];
    psx_guest_cycle_scale_set(2);psx_guest_cycle_scale_set_gated(1);
    psx_guest_cycle_scale_gate_open(1);
    CHECK(psx_cpu_cycles(1)==0, "scaled charge starts with a carried half cycle");
    psx_guest_cycle_scale_snapshot(scale_before);
    CHECK(psx_cycle_freeze_begin(&save,0,NULL), "scaled draw freeze begins");
    psx_cpu_charge(3);
    psx_cycle_freeze_end(&save);
    psx_guest_cycle_scale_snapshot(scale_after);
    CHECK(!memcmp(scale_before,scale_after,sizeof scale_before), "draw freeze restores carried cycle-scale phase");
    CHECK(psx_cpu_cycles(1)==1, "next authoritative instruction retains its original scaled charge");
    psx_guest_cycle_scale_set(1);psx_guest_cycle_scale_set(2);psx_guest_cycle_scale_gate_open(1);
    (void)psx_cpu_cycles(1);psx_guest_cycle_scale_snapshot(scale_before);
    CHECK(psx_cycle_freeze_begin(&save,0,NULL), "scale gate draw freeze begins");
    psx_guest_cycle_scale_gate_open(0);psx_cpu_charge(3);
    psx_cycle_freeze_end(&save);psx_guest_cycle_scale_snapshot(scale_after);
    CHECK(!memcmp(scale_before,scale_after,sizeof scale_before) && psx_guest_cycle_scale()==2,
          "draw freeze restores cycle-scale gates as well as phase");
    psx_guest_cycle_scale_gate_open(1);psx_guest_cycle_scale_set(1);psx_guest_cycle_scale_set(2);
    (void)psx_cpu_cycles(1);psx_guest_cycle_scale_snapshot(scale_before);
    if(setjmp(s_jmp)==0) {
        CHECK(psx_cycle_freeze_begin(&save,100,overrun), "scaled abort freeze begins");
        psx_guest_cycle_scale_gate_open(0);
        for(unsigned iteration=0;iteration<4;++iteration)psx_cpu_charge(101);
        psx_devices_mmio_sync();
        CHECK(0, "scaled freeze watchdog must abort");
    }
    psx_cycle_freeze_end(&save);psx_guest_cycle_scale_snapshot(scale_after);
    CHECK(!memcmp(scale_before,scale_after,sizeof scale_before) && psx_guest_cycle_scale()==2,
          "watchdog abort restores cycle-scale gates and phase");
    psx_guest_cycle_scale_set(1);psx_guest_cycle_scale_set_gated(0);

    /* Watchdog: a pass that runs away is cut off. The longjmp skips the
     * bb_defer exits of the generated frames it leaves (their cleanup
     * handlers never run); end() puts the interrupted depth back. */
    s_overruns = 0;
    cyc0 = psx_cycle_count;
    g_psx_cyc_bb_defer = 2;
    if (setjmp(s_jmp) == 0) {
        CHECK(psx_cycle_freeze_begin(&save, 200000u, overrun), "freeze with watchdog");
        g_psx_cyc_bb_defer += 3;          /* three nested generated frames */
        for (int i = 0; i < 1000; i++) psx_advance_cycles(1000);
        CHECK(0, "watchdog should have fired");
    }
    psx_cycle_freeze_end(&save);
    CHECK(s_overruns == 1, "watchdog fired once");
    CHECK(psx_cycle_count == cyc0 && !g_psx_render_pass_active,
          "state restored after a watchdog abort");
    CHECK(g_psx_cyc_bb_defer == 2,
          "cycle deferral depth restored after a watchdog abort");
    g_psx_cyc_bb_defer = 0;

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
