/* Uncharged guest-time spans (psx_cycle_freeze.h psx_cycle_uncharged_*),
 * the time layer of mod_plugins.h psx_mod_call_guest_uncharged. Code in a
 * span keeps its effects but consumes no guest time; a span over budget
 * thaws and is charged in full. Build/run: ctest -R uncharged_span_test */

#include "psx_cycles.h"
#include "psx_cycle_freeze.h"

#include <stdint.h>
#include <stdio.h>

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


static uint64_t s_cd_phase_total(void) { return s_dev_cycles; }

int main(void) {
    PsxCycleFreeze save, pass;
    uint64_t cyc0, next0, dev0, cd0;
    uint32_t local = 7;

    psx_cycles_resync_after_restore(NULL);
    psx_advance_cycles(12345);
    g_psx_cyc_batch = 40;
    g_psx_cyc_local_acc = &local;
    cyc0 = psx_cycle_count;
    next0 = psx_next_service_cycle;
    dev0 = s_cd_phase_total();
    cd0 = s_cd_events;

    /* 1. A span within budget: counted inside, nothing charged after. */
    CHECK(psx_cycle_uncharged_begin(&save, 4000000u), "span begins");
    CHECK(g_psx_guest_time_frozen == 1 && g_psx_render_pass_active == 0,
          "time frozen, but this is not a render pass (stores stay live)");
    CHECK(!psx_cycle_uncharged_begin(&pass, 0), "spans do not nest");
    CHECK(!psx_cycle_freeze_begin(&pass, 0, NULL), "no render pass inside a span");
    CHECK(g_psx_cyc_batch == 0 && g_psx_cyc_local_acc == NULL,
          "the caller's pending charges are parked");
    for (int i = 0; i < 1000; i++) psx_advance_cycles(1000);
    psx_advance_cycles_slow(5000);
    psx_devices_service_to_now();
    psx_devices_mmio_sync();
    CHECK(psx_cycle_count - cyc0 == 1005000u, "cycles are counted inside the span");
    g_psx_cyc_batch = 25;
    CHECK(psx_cycle_uncharged_counted() == 1005025u,
          "counted cost includes the span's unpublished batch");
    g_psx_cyc_batch = 0;
    CHECK(s_cd_phase_total() == dev0 && s_cd_events == cd0,
          "no device advanced and no device event fired");
    CHECK(psx_cycle_uncharged_end(&save) == 1, "span stayed uncharged");
    CHECK(psx_cycle_uncharged_counted() == 0, "no span, nothing counted");
    CHECK(g_psx_guest_time_frozen == 0, "time is live again");
    CHECK(psx_cycle_count == cyc0 && psx_next_service_cycle == next0,
          "clock and service deadline restored exactly: zero guest time");
    CHECK(g_psx_cyc_batch == 40 && g_psx_cyc_local_acc == &local && local == 7,
          "the caller's pending charges come back untouched");
    g_psx_cyc_batch = 0;
    g_psx_cyc_local_acc = NULL;

    /* 2. A render pass refuses to start a span inside it. */
    CHECK(psx_cycle_freeze_begin(&pass, 0, NULL), "pass begins");
    CHECK(!psx_cycle_uncharged_begin(&save, 0), "no span inside a pass");
    psx_cycle_freeze_end(&pass);
    CHECK(g_psx_guest_time_frozen == 0 && g_psx_render_pass_active == 0, "pass over");

    /* 3. Over budget: the span thaws and everything it ran is charged.
     * Devices catch up from where they stopped, so the CD raises exactly
     * the events the elapsed time implies (a callee waiting on a device
     * would see it complete). */
    cyc0 = psx_cycle_count;
    dev0 = s_cd_phase_total();
    cd0 = s_cd_events;
    {
        const uint32_t phase0 = s_cd_phase;
        CHECK(psx_cycle_uncharged_begin(&save, 200000u), "budgeted span begins");
        int thawed_at = -1;
        for (int i = 0; i < 1000; i++) {
            psx_advance_cycles(1000);
            if (thawed_at < 0 && !g_psx_guest_time_frozen) thawed_at = i;
        }
        CHECK(thawed_at > 0 && thawed_at < 260, "thawed just past the budget");
        CHECK(psx_cycle_uncharged_end(&save) == 0, "span reports it was charged");
        psx_devices_service_to_now();
        CHECK(psx_cycle_count - cyc0 == 1000000u, "every cycle stays charged");
        CHECK(s_cd_phase_total() - dev0 == 1000000u,
              "devices caught up over the whole span");
        CHECK(s_cd_events - cd0 == (uint64_t)((phase0 + 1000000u) / 1000u),
              "device events fired for the whole elapsed time");
    }

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
