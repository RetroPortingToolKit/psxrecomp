/*
 * Pin the idle-loop detector's store-counter extension
 * ([runtime] idle_skip_store_counters, psx_cycles.c).
 *
 * The positive case is PsyQ libetc's v_wait as R4 runs it (0x8008B4A8): two
 * interrupt-check edges per pass (0x8008B4D0 and 0x8008B518), one word store
 * per pass to a stack counter that drops by exactly 1, no MMIO, registers
 * identical at the anchor edge. It must skip whole passes, lower the counter
 * by the same number of passes through the host writer, never to 1 or below,
 * and land on the anchor strictly before the next device event.
 *
 * Near misses that must NOT skip: an extra store per pass, an MMIO read per
 * pass, a counter that drops by 2, a store to MMIO space, and the same loop
 * with the option off.
 *
 * Build/run: ctest -R idle_skip_store_counter_test
 */
#include "cpu_state.h"
#include "psx_cycles.h"

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
static uint32_t g_guest_last_store_addr = 0xFFFFFFFFu, g_guest_last_store_val;
static int g_psx_track_last_store;

static uint32_t s_event_at = 0;   /* absolute cycle of the next device event */

void sio_advance(uint32_t c) { (void)c; }
void cdrom_advance(uint32_t c) { (void)c; }
void dma_advance(uint32_t c) { (void)c; }
void timers_advance(uint32_t c) { (void)c; }
void interrupts_advance_cycles(uint32_t c) { (void)c; }
void interrupts_service_scheduled_events(void) {}
uint32_t interrupts_cycles_to_vblank(void) {
    uint64_t now = psx_get_cycle_count();
    return now >= s_event_at ? 1u : (uint32_t)(s_event_at - now);
}
uint32_t timers_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t sio_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return UINT32_MAX; }
void psx_spu_sample_event_service(void) {}
int psx_get_in_exception(void) { return 0; }
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
int  psx_netplay_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}

/* Guest RAM word the loop counts down, and the host writer under test. */
static uint32_t s_counter;
static int s_host_writes;
static void host_write(uint32_t addr, uint32_t val) {
    (void)addr; s_counter = val; s_host_writes++;
}

#define ANCHOR 0x8008B4D0u
#define EDGE2  0x8008B518u
#define COUNTER_ADDR 0x801FFF10u   /* stack word in main RAM */

enum { EXTRA_STORE = 1, MMIO_READ = 2, STEP2 = 4, MMIO_STORE = 8 };

static void guest_store(uint32_t addr, uint32_t val) {
    g_guest_store_count++;
    if (g_psx_track_last_store) { g_guest_last_store_addr = addr; g_guest_last_store_val = val; }
}

static int fails;
static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

static uint64_t s_start, s_after_first_skip;
static uint32_t s_counter_after_first_skip;

static uint64_t run(int ext, int quirks, int passes, uint32_t start_counter) {
    static CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.gpr[2] = 1u;           /* slt result at the edges: still waiting */
    cpu.gpr[29] = 0x801FFF00u;
    psx_cycles_resync_after_restore(NULL);
    s_start = psx_get_cycle_count();
    s_event_at = (uint32_t)s_start + 200000u;
    s_after_first_skip = 0;
    s_counter = start_counter;
    s_host_writes = 0;
    g_idle_skip_enabled = 1;
    g_idle_skip_ext = ext;
    g_idle_skip_count = 0;
    for (int i = 0; i < passes && psx_get_cycle_count() < s_event_at; i++) {
        psx_idle_note_check(&cpu, ANCHOR);
        if (g_idle_skip_count && !s_after_first_skip) {
            s_after_first_skip = psx_get_cycle_count();
            s_counter_after_first_skip = s_counter;
        }
        psx_advance_cycles(9);
        if (s_counter <= 1) break;
        s_counter -= (quirks & STEP2) ? 2u : 1u;
        guest_store((quirks & MMIO_STORE) ? 0x1F801070u : COUNTER_ADDR, s_counter);
        if (quirks & EXTRA_STORE) guest_store(COUNTER_ADDR + 4u, 0);
        if (quirks & MMIO_READ) g_mmio_access_count++;
        psx_advance_cycles(7);
        psx_idle_note_check(&cpu, EDGE2);
    }
    return g_idle_skip_count;
}

int main(void) {
    psx_idle_skip_set_host_writer(host_write);
    psx_idle_skip_set_store_tracker(&g_psx_track_last_store, &g_guest_last_store_addr,
                                    &g_guest_last_store_val);

    uint64_t skips = run(1, 0, 64, 100000u);
    check(skips > 0, "v_wait: passes are skipped");
    check(s_host_writes > 0, "v_wait: the counter is lowered through the host writer");
    check(s_counter > 1u, "v_wait: the counter never reaches the timeout value");
    check(s_after_first_skip < s_event_at,
          "v_wait: the skip lands on the anchor before the device event "
          "(the IRQ is taken by a real pass)");
    check(s_event_at - s_after_first_skip < 200000u / 2u,
          "v_wait: the skip covers most of the wait");
    check((s_after_first_skip - s_start) % 16u == 0,
          "v_wait: cycles advanced in whole 16-cycle passes");
    check((uint64_t)(100000u - s_counter_after_first_skip) * 16u + 16u ==
              s_after_first_skip - s_start + 16u,
          "v_wait: the counter dropped by exactly one per pass, skipped passes included");

    uint64_t short_skips = run(1, 0, 4000, 40u);
    check(s_counter >= 1u && s_counter <= 2u && short_skips > 0,
          "v_wait with a small timeout: skips stop before the timeout test");

    check(run(0, 0, 64, 100000u) == 0, "option off: no skip");
    check(run(1, EXTRA_STORE, 64, 100000u) == 0, "extra store per pass: no skip");
    check(run(1, MMIO_READ, 64, 100000u) == 0, "MMIO read per pass: no skip");
    check(run(1, STEP2, 64, 100000u) == 0, "counter drops by 2: no skip");
    check(run(1, MMIO_STORE, 64, 100000u) == 0, "store to MMIO space: no skip");

    psx_idle_skip_set_host_writer(NULL);
    check(run(1, 0, 64, 100000u) == 0, "no host writer installed: no skip");

    printf("%s (%d failure%s)\n", fails ? "FAILED" : "passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
