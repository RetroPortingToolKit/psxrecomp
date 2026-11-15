/* The two load resyncs keep what a checkpoint state restored, once.
 * Production interrupts.c, dirty_ram_interp.c and psx_cycles.c; every other
 * seam aborts (fixture_link.py). A load without the sections behaves as it
 * always has: the pending load is dropped and the IRQ delivery state reset. */
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include <stdio.h>
#include <string.h>

int g_boot_state_cpu_timing_restored;
static uint32_t s_sio_seq = 77;
uint32_t sio_get_seq(void) { return s_sio_seq; }

void gpu_vblank_clear_deferred_present(void) {}
static int failures;
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static int timing_equal(const PsxIrqResumeTiming *a, const PsxIrqResumeTiming *b) {
    return a->cooldown_until == b->cooldown_until && a->sio_seq_seen == b->sio_seq_seen &&
           a->sio_progress_cycle == b->sio_progress_cycle && a->defer_from == b->defer_from &&
           a->defer_target == b->defer_target && a->defer_pending == b->defer_pending;
}

int main(void) {
    static CPUState cpu;
    PsxIrqResumeTiming in, out;
    DirtyRamLoadDelay ld = {9, 0x12345678u, 0, 1}, got;

    psx_cycle_count = 5000;
    memset(&in, 0, sizeof in);
    in.cooldown_until = 6000;
    in.sio_seq_seen = 70;
    in.sio_progress_cycle = 4000;
    in.defer_from = 0x800F0000u;
    in.defer_target = 0x800F00C0u;
    in.defer_pending = 1;

    /* A plain load: the resync resets what it always reset. */
    interrupts_resume_timing_set(&in);
    interrupts_resync_after_restore();   /* consumes the set above */
    interrupts_resync_after_restore();
    interrupts_resume_timing_get(&out);
    check(out.cooldown_until == 0 && out.defer_pending == 0 && out.defer_from == 0 &&
          out.defer_target == 0, "a plain load resets the cooldown and deferred switch");
    check(out.sio_seq_seen == s_sio_seq && out.sio_progress_cycle == psx_cycle_count,
          "a plain load re-anchors SIO progress at the live sequence and clock");

    /* A checkpoint load: the next resync keeps every field, once. */
    interrupts_resume_timing_set(&in);
    interrupts_resync_after_restore();
    interrupts_resume_timing_get(&out);
    check(timing_equal(&in, &out), "the resync after a checkpoint load keeps IRQ timing");
    interrupts_resync_after_restore();
    interrupts_resume_timing_get(&out);
    check(out.cooldown_until == 0 && out.defer_pending == 0,
          "the keep is consumed by one resync");

    /* Pending load: dropped after a plain load ... */
    dirty_ram_ld_delay_restore(&ld);
    psx_cycles_resync_after_restore(&cpu);
    dirty_ram_ld_delay_save(&got);
    check(!got.armed && got.rt == 0 && got.val == 0, "a plain load drops the pending load");

    /* ... kept after a checkpoint load, once. */
    dirty_ram_ld_delay_restore_from_state(&ld);
    psx_cycles_resync_after_restore(&cpu);
    dirty_ram_ld_delay_save(&got);
    check(got.armed == 1 && got.rt == 9 && got.val == 0x12345678u && got.age == 0,
          "the resync after a checkpoint load keeps the pending load");
    psx_cycles_resync_after_restore(&cpu);
    dirty_ram_ld_delay_save(&got);
    check(!got.armed, "the keep is consumed by one resync");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: load resyncs keep checkpoint state once");
    return 0;
}
