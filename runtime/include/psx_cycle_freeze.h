#ifndef PSX_CYCLE_FREEZE_H
#define PSX_CYCLE_FREEZE_H

/* Guest-time freeze: render passes (render_pass.c, docs/RENDER_PASSES.md)
 * and uncharged guest calls (mod_plugins.h psx_mod_call_guest_uncharged).
 *
 * Runtime-only: generated and overlay code never sees this header. It is kept
 * out of psx_cycles.h on purpose, because psx_cycles.h is part of the
 * overlay codegen hash (runtime/codegen_hash_sources.cmake) and any change to
 * it invalidates every title's overlay cache and savestates. The freeze is
 * enforced in psx_cycles.c's out-of-line service paths, which generated code
 * already reaches through psx_cycles.h.
 *
 * While g_psx_render_pass_active is set, guest cycles are still counted (GTE
 * and mult/div deadlines keep working) but devices are never serviced, no
 * VBlank or device event fires and no interrupt is delivered; end() puts
 * every clock value back exactly, so the pass consumed no guest time. A pass
 * that runs longer than `watchdog_cycles` calls `overrun` (which must not
 * return into the guest; render_pass.c longjmps out of the pass). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_psx_render_pass_active;
/* Set while guest time is frozen, by a render pass or an uncharged call:
 * devices are not serviced and no interrupt is delivered. Pass-only effects
 * (rolled-back stores, synchronous DMA) key on g_psx_render_pass_active. */
extern int g_psx_guest_time_frozen;
typedef struct PsxCycleFreeze {
    uint64_t cycle_count;
    uint64_t next_service;
    uint64_t fast_limit;
    uint32_t batch;
    uint32_t batch_limit;
    uint32_t *local_acc;
    uint32_t local_acc_value;
    int      in_device_service;
    /* Generated functions bump g_psx_cyc_bb_defer on entry and drop it in a
     * cleanup handler, which a longjmp out of the pass (the watchdog) skips:
     * end() puts the interrupted code's depth back. */
    int      bb_defer;
    uint32_t guest_cycle_scale[3];
} PsxCycleFreeze;
int  psx_cycle_freeze_begin(PsxCycleFreeze *save, uint64_t watchdog_cycles,
                            void (*overrun)(void));
void psx_cycle_freeze_end(const PsxCycleFreeze *save);

/* Uncharged span: the same freeze without the pass's rollback. Code run
 * between begin and end keeps every effect on RAM, registers and devices, as
 * if it executed in an instant at the start cycle: end() puts the clock back,
 * so none of its cycles are charged, no device advanced and no interrupt was
 * taken meanwhile. A span that executes more than `budget_cycles` (0 = no
 * limit) thaws: from then on it runs on the live clock and all its cycles are
 * charged, so a callee that waits on a device still completes. begin()
 * returns 0 while time is already frozen (the caller then simply runs the
 * code). end() returns 1 when the span stayed uncharged. Spans do not nest. */
int  psx_cycle_uncharged_begin(PsxCycleFreeze *save, uint64_t budget_cycles);
int  psx_cycle_uncharged_end(const PsxCycleFreeze *save);

#ifdef __cplusplus
}
#endif

#endif
