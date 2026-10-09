#ifndef PSXRECOMP_PSX_CYCLES_H
#define PSXRECOMP_PSX_CYCLES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSX guest CPU cycle clock — the single source of truth for guest-
 * visible time. Peripherals derive all schedules from this counter. */
extern uint64_t psx_cycle_count;

/* Deadline-device model bookkeeping (written by psx_cycles.c). Hot path
 * reads these from the inlined psx_advance_cycles below. */
extern uint64_t psx_next_service_cycle; /* absolute; 0 = dirty / recompute */
extern int      psx_in_device_service;  /* re-entrancy guard */
extern int      g_event_step_conservative;

/* Diagnostic replay clock: advance the CPU-visible guest clock without
 * servicing devices, then restore the authoritative live clock afterward.
 * Used only while g_ls_replay_active is set. */
int      psx_cycle_replay_begin(uint64_t start_cycle);
uint64_t psx_cycle_replay_end(void);

/* Transactional I-cache view for the overlay differential harness. The
 * interpreter keeps the authoritative post-call cache; native replay starts
 * from the saved entry tags, mutates a temporary view, then restores live. */
int      psx_icache_shadow_record_begin(void);
int      psx_icache_shadow_replay_begin(void);
void     psx_icache_shadow_replay_end(void);
void     psx_icache_shadow_abort(void);

/* Event-deadline device model: catch every device up to the charged
 * guest-cycle position and force a deadline recompute. memory.c calls
 * this at the top of every device-MMIO read/write. */
void psx_devices_mmio_sync(void);
void psx_devices_service_to_now(void);

/* Rare/slow advance path (COSIM, conservative 1-cycle stepping, lockstep). */
void psx_advance_cycles_slow(uint32_t cycles);

/* Sparse throttle fires (watchdog / PC sample) — not on the per-charge path. */
extern uint32_t psx_watchdog_throttle;
extern uint32_t psx_pc_sample_throttle;
void psx_cycles_watchdog_fire(void);
void psx_cycles_pc_sample_fire(void);

/* Lockstep replay flag (defined in dirty_ram_interp.c). */
extern int g_ls_replay_active;

/* Deferred under-deadline charges (MotK VLC load-charge batching).
 * psx_cyc_charge accumulates here; publish via psx_cyc_batch_flush /
 * psx_advance_cycles before IRQ checks, MMIO, or any cycle read that must
 * match the published counter. Guest totals at those barriers are unchanged. */
extern uint32_t g_psx_cyc_batch;
extern uint32_t g_psx_cyc_batch_limit;

/* GCC/Clang-generated functions can defer deadline probes within a basic
 * block. Interrupt/MMIO edges still publish the accumulated guest cycles. */
extern int g_psx_cyc_bb_defer;

/* Emitter-level VLC load-charge batching: when non-NULL, psx_cyc_charge
 * accumulates into *g_psx_cyc_local_acc instead of g_psx_cyc_batch. Publish
 * via psx_cyc_local_publish / psx_cyc_batch_flush before IRQ/MMIO barriers. */
extern uint32_t *g_psx_cyc_local_acc;

/* [timing] guest_cycle_scale — a title constant, not a player setting.
 * No CPU is emulated: recompiled code charges the guest clock a fixed number
 * of cycles per MIPS instruction it runs, and that clock is what VBlank,
 * timers, CD, SPU and DMA are scheduled against. guest_cycle_scale = N
 * charges each instruction 1/N of its cost; device time is unchanged.
 * Larger N means the game never runs out of frame time; 1 is faithful and
 * skips the scaling entirely. g_psx_gcs_recip_q16 is 65536/N, g_psx_gcs_frac
 * the carried fraction (part of the rollback/savestate snapshot).
 *
 * Two kinds of guest-time charge, and only these two:
 *   psx_cpu_charge(n)      CPU work: instruction base cost, load costs
 *                          (fudge, region wait, completion), i-cache refills,
 *                          BIOS HLE costs. Scaled. Every tier (native
 *                          generated code, overlay DLLs, the dirty-RAM
 *                          interpreter, the BIOS) charges CPU work through
 *                          it; the batched psx_cyc_charge hot path hands its
 *                          charge to it whenever the scale is active.
 *   psx_advance_cycles(n)  device time and stalls already measured in guest
 *                          time (DMA, idle skips, mul/div and GTE deadline
 *                          waits). Never scaled.
 * Mul/div and GTE latencies are CPU work too: psx_cpu_cycles() scales the
 * deadline they set, so the later stall waits the scaled time. */
extern uint32_t g_psx_gcs_recip_q16;
extern uint32_t g_psx_gcs_frac;
void     psx_guest_cycle_scale_set(uint32_t scale);  /* 1..64; 1 = faithful */
/* Live scale: the configured one, or 1 while gated and the gate is shut. */
uint32_t psx_guest_cycle_scale(void);
uint32_t psx_guest_cycle_scale_config(void);
/* Gates (all must be open for the scale to apply; with none it always does):
 *  - RAM gate: [timing] guest_cycle_scale_gate, predicates on guest RAM
 *    words evaluated at every VBlank edge (deterministic, works online).
 *  - Mod gate: guest_cycle_scale_gated = true; a trusted plugin opens it
 *    with psx_mod_set_guest_cycle_scale_gate() (mod_plugins.h). */
void     psx_guest_cycle_scale_set_gated(int gated);
void     psx_guest_cycle_scale_gate_open(int open);
enum { PSX_GCS_RAM_GATE_MAX = 8 };
/* addr: guest main-RAM address (any segment), size 1/2/4, open while
 * (word & mask) == value. Returns 0 for a bad predicate. Clear with
 * psx_guest_cycle_scale_ram_gate_clear(). */
int      psx_guest_cycle_scale_ram_gate_add(uint32_t addr, uint32_t size,
                                            uint32_t mask, uint32_t value);
void     psx_guest_cycle_scale_ram_gate_clear(void);
/* How the RAM gate reads guest RAM (main.cpp installs a main-RAM reader;
 * phys is masked physical, aligned to size). */
void     psx_guest_cycle_scale_set_ram_reader(uint32_t (*read)(uint32_t phys, uint32_t size));
/* Called on each VBlank edge (interrupts.c); no-op without a RAM gate. */
void     psx_guest_cycle_scale_vblank(void);
/* Rollback/savestate: carried fraction + gate state (boot_state BS_SEC_GCS). */
enum { PSX_GCS_SNAPSHOT_BYTES = 12 };
void     psx_guest_cycle_scale_snapshot(uint32_t out[3]);
void     psx_guest_cycle_scale_restore(const uint32_t in[3]);
static inline uint32_t psx_gcs_recip_for(uint32_t scale) {
    if (scale <= 1u) return 65536u;
    if (scale > 64u) scale = 64u;
    return (uint32_t)((65536u + scale / 2u) / scale);
}
static inline uint32_t psx_gcs_scale(uint32_t cpu_cycles) {
    uint64_t t = (uint64_t)cpu_cycles * g_psx_gcs_recip_q16 + g_psx_gcs_frac;
    g_psx_gcs_frac = (uint32_t)(t & 0xFFFFu);
    return (uint32_t)(t >> 16);
}
#if defined(__GNUC__) || defined(__clang__)
#define PSX_GCS_ACTIVE() __builtin_expect(g_psx_gcs_recip_q16 != 65536u, 0)
#else
#define PSX_GCS_ACTIVE() (g_psx_gcs_recip_q16 != 65536u)
#endif
#if !defined(PSX_OVERLAY_DLL_BUILD)
/* CPU cycles -> guest cycles. Identity at scale 1 (no state touched). */
static inline uint32_t psx_cpu_cycles(uint32_t cpu_cycles) {
    return PSX_GCS_ACTIVE() ? psx_gcs_scale(cpu_cycles) : cpu_cycles;
}
#endif

/* Advance guest time. Overlay DLLs forward this through their callback shim;
 * normal runtime/generated code keeps the common production path inlined. */
#if defined(PSX_OVERLAY_DLL_BUILD)
void psx_advance_cycles(uint32_t cycles);
#else
/* The common production path is inlined: bump the
 * counter and only service devices when the next event deadline is due.
 * Guest-visible timing is unchanged (service_to_now replays exact events).
 *
 * Watchdog / PC-sample throttles live in psx_devices_service_to_now (fired
 * on the HARD_CAP / event cadence, ≥ every 16K guest cycles) — not on every
 * per-instruction charge. MotK VLC issues millions of advances/s; two add+
 * branch pairs there were pure host tax. */
static inline void psx_advance_cycles(uint32_t cycles) {
#if !defined(PSX_COSIM)
    if (g_psx_cyc_batch) {
        uint32_t b = g_psx_cyc_batch;
        g_psx_cyc_batch = 0;
        g_psx_cyc_batch_limit = 0;
        if (cycles <= UINT32_MAX - b) cycles += b;
        else {
            /* Extreme: publish b first, then continue with cycles. */
            psx_cycle_count += (uint64_t)b;
            if (!psx_in_device_service &&
                (psx_next_service_cycle == 0u ||
                 psx_cycle_count >= psx_next_service_cycle)) {
                psx_devices_service_to_now();
            }
        }
    }
#endif
    if (cycles == 0u) return;
#if defined(PSX_COSIM)
    psx_advance_cycles_slow(cycles);
    return;
#else
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(g_ls_replay_active | g_event_step_conservative, 0)) {
#else
    if (g_ls_replay_active || g_event_step_conservative) {
#endif
        psx_advance_cycles_slow(cycles);
        return;
    }
    if (psx_in_device_service) {
        psx_cycle_count += (uint64_t)cycles;
        return;
    }
    psx_cycle_count += (uint64_t)cycles;
    if (psx_next_service_cycle == 0u ||
        psx_cycle_count >= psx_next_service_cycle) {
        psx_devices_service_to_now();
    }
#endif
}
#endif

/* The CPU charge (see above): every tier's CPU work is scaled in this one
 * function (psx_cpu_cycles does the same for mul/div and GTE deadlines,
 * which are set, not charged). At scale 1 it is exactly psx_advance_cycles(cycles). Overlay DLLs
 * accumulate it locally (overlay_dispatch_preamble.c.inc) and publish the
 * raw total through the cpu_charge callback, which lands in this function. */
#if defined(PSX_OVERLAY_DLL_BUILD)
void psx_cpu_charge(uint32_t cycles);
#else
static inline void psx_cpu_charge(uint32_t cycles) {
    if (PSX_GCS_ACTIVE()) {
        cycles = psx_gcs_scale(cycles);
        if (cycles == 0u) return;
    }
    psx_advance_cycles(cycles);
}
#endif

/* Publish deferred charges (IRQ edge / MMIO / savestate). Overlay DLLs keep
 * their pending total in the callback shim rather than these host globals. */
#if defined(PSX_OVERLAY_DLL_BUILD)
PSX_OVERLAY_EXPORT void overlay_flush_cycles(void);
static inline void psx_cyc_local_publish(void) { }
static inline void psx_cyc_batch_flush(void) { overlay_flush_cycles(); }
#else
/* Publish function-local charges into the normal batch/advance path while
 * keeping the local pointer installed (nested charges resume locally). */
static inline void psx_cyc_local_publish(void) {
#if !defined(PSX_COSIM)
    uint32_t *acc = g_psx_cyc_local_acc;
    if (!acc) return;
    uint32_t v = *acc;
    if (!v) return;
    *acc = 0;
    g_psx_cyc_local_acc = NULL;
    psx_advance_cycles(v);
    g_psx_cyc_local_acc = acc;
#endif
}

static inline void psx_cyc_batch_flush(void) {
#if !defined(PSX_COSIM)
    psx_cyc_local_publish();
    uint32_t b = g_psx_cyc_batch;
    if (!b) return;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    psx_advance_cycles(b);
#endif
}
#endif

/* Read accessor for telemetry (includes deferred batch). */
uint64_t psx_get_cycle_count(void);

/* Idle-loop cycle skip (see psx_cycles.c "Idle-loop cycle skip"). */
struct CPUState;
void psx_idle_note_check(struct CPUState *cpu, uint32_t check_pc);
int  psx_idle_skip_is_enabled(void);
/* Cycles until the nearest IRQ-observable device event (mask-aware); the bound an idle skip may not cross. */
uint32_t psx_idle_cycles_to_next_observable_event(void);
extern int      g_idle_skip_enabled;
/* [runtime] idle_skip_store_counters (psx_cycles.c): 1 on, 0 off, -1 env. */
extern int      g_idle_skip_ext;
/* The host write that lowers a skipped loop counter (psx_host_write_word);
 * the store-counter extension stays off until one is installed. */
void psx_idle_skip_set_host_writer(void (*w)(uint32_t addr, uint32_t val));
/* The store path's last-word-store record (memory.c): the gate the detector
 * sets while the extension is on, and the address/value it reads. */
void psx_idle_skip_set_store_tracker(int *gate, const uint32_t *addr, const uint32_t *val);
extern uint64_t g_idle_skip_count;
extern uint64_t g_idle_skip_cycles;
extern uint32_t g_idle_skip_last_pc;
extern uint32_t g_idle_skip_last_quantum;

/* Post-load probe cycle diagnostics (optional; main.cpp soft-load tooling). */
extern int      g_plp_cycle_diag;
extern uint64_t g_plp_adv_calls;
extern uint32_t g_plp_adv_max_chunk;
extern uint64_t g_plp_adv_sum;
extern uint64_t g_plp_svc_calls;

/* Save-state restore: re-anchor the deadline device model after psx_cycle_count
 * is overwritten from a snapshot. Pass the live CPU so GTE/muldiv completion
 * stamps and load-absorb give-back are rewound with the guest clock. */
void psx_cycles_resync_after_restore(struct CPUState *cpu);

/* Soft rematch / session_reboot: zero the guest clock and deadline bookkeeping.
 * Soft-exit longjmps out of vblank (inside psx_devices_service_to_now) leave
 * psx_in_device_service stuck at 1 and a huge leftover cycle count — without
 * this reset the next match never services devices or fires vblanks. */
void psx_cycles_reset_for_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_CYCLES_H */
