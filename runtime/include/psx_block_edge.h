/* Generated block boundaries. Native observation timing keeps ordinary edges
 * in the caller; the original delivery implementation owns every slow edge. */
#ifndef PSX_BLOCK_EDGE_H
#define PSX_BLOCK_EDGE_H
#include "interrupts.h"
#include "psx_observation_view.h"

#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)
#define PSX_BLOCK_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define PSX_BLOCK_INLINE static __forceinline
#else
#define PSX_BLOCK_INLINE static inline
#endif

/* One policy for statically compiled and DLL edges. The DLL supplies its
 * effective clock (including its unpublished raw charge), not another IRQ
 * or device scheduler. */
PSX_BLOCK_INLINE int psx_observation_delivery_pending_values(const CPUState *cpu,
                                                         uint32_t status, uint32_t mask) {
    uint32_t sr = cpu->cop0[12];
    return (sr & 1u) && ((cpu->cop0[13] & sr & 0x0300u) ||
           ((sr & 0x0400u) && (status & mask)));
}
PSX_BLOCK_INLINE int psx_observation_edge_due(const CPUState *cpu, uint64_t now,
        uint64_t deadline, int deferred, int save, int wait, int wait_active,
        uint32_t status, uint32_t mask) {
    return deadline == 0u || now >= deadline || deferred || save || wait ||
           wait_active || psx_observation_delivery_pending_values(cpu, status, mask);
}

#if defined(PSX_OVERLAY_DLL_BUILD)
#ifdef __cplusplus
extern "C" {
#endif
extern const PSXObservationView *g_psx_overlay_observation_view;
extern uint32_t *g_psx_overlay_pending_cycles_p;
#ifdef __cplusplus
}
#endif
static inline void psx_overlay_legacy_edge(CPUState *cpu, uint32_t pc,
                                         const PSXObservationView *v) {
    if (v) {
        if (*v->flush_owner && *v->flush_owner != overlay_flush_cycles) {
            /* Preserve publication order for an unproved foreign owner. */
            overlay_flush_cycles();
            (*v->flush_owner)();
        }
        uint32_t raw = *g_psx_overlay_pending_cycles_p;
        *g_psx_overlay_pending_cycles_p = 0;
        v->fallback(cpu, pc, raw);
        return;
    }
    psx_cyc_bb_defer_flush();
    psx_check_interrupts_at(cpu, pc);
}
PSX_BLOCK_INLINE void psx_overlay_block_edge(CPUState *cpu, uint32_t pc) {
    const PSXObservationView *v = g_psx_overlay_observation_view;
    if (!v || *v->device_service || *v->conservative || *v->frozen || *v->replay ||
        *v->shadow || *v->suppress_irq || *v->cd_only || *v->defer_cd ||
        *v->irq_budget || *v->irq_ratelimit || v->legacy_unit_defer ||
        *v->batch || (*v->local_acc && **v->local_acc) ||
        *v->flush_owner != overlay_flush_cycles) {
        psx_overlay_legacy_edge(cpu, pc, v);
        return;
    }
    uint32_t raw = *g_psx_overlay_pending_cycles_p;
    uint64_t charge = *v->scale_recip == 65536u ? raw :
        psx_gcs_scaled_charge(raw, *v->scale_recip, *v->scale_frac);
    if (charge > UINT64_MAX - *v->clock) {
        psx_overlay_legacy_edge(cpu, pc, v); return;
    }
    uint64_t now = *v->clock + charge;
    int wait = 0;
    for (uint32_t n = 0; n < v->wait_site_count; ++n)
        if (pc == v->wait_sites[n]) { wait = 1; break; }
    ++*v->block_edges;
    if (now >= *v->sample || psx_observation_edge_due(cpu, now, *v->deadline,
            *v->defer_switch, *v->save_pending, wait, *v->wait_active,
            *v->irq_status, *v->irq_mask)) {
        *g_psx_overlay_pending_cycles_p = 0;
        v->edge(cpu, pc, wait, raw);
    } else if (*v->suppress_record > 0) {
        --*v->suppress_record;
    }
}
#define PSX_BLOCK_EDGE(cpu, pc) psx_overlay_block_edge((cpu), (pc))
#else

#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
#include "psx_observation_config.h"
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t i_stat, i_mask;
extern int psx_defer_switch_pending;
extern int g_psx_savestate_pending, g_ls_suppress_record;
extern int psx_observation_wait_active;
extern uint64_t psx_observation_block_edges, psx_observation_slow_edges;
extern uint64_t psx_observation_wait_edges;
extern void (*g_overlay_flush_pending_cycles)(void);
void psx_observation_edge(struct CPUState *cpu, uint32_t resume_pc, int wait_site);
#ifdef __cplusplus
}
#endif
PSX_BLOCK_INLINE int psx_observation_delivery_pending(const CPUState *cpu) {
    return psx_observation_delivery_pending_values(cpu, i_stat, i_mask);
}
/* A private precision body publishes its pending receipts before ANY callback,
 * then returns to its canonical entry after a non-quiet edge. Do not publish a
 * PC here: delivery and restore callbacks own their CPU/control-flow result. */
PSX_BLOCK_INLINE int psx_block_edge_impl(CPUState *cpu, uint32_t pc,
                                        void (*materialize)(CPUState *)) {
    if (g_psx_guest_time_frozen || g_ls_replay_active) {
        if (materialize) materialize(cpu);
        return 1;
    }
    const int wait_site = PSX_OBSERVATION_WAIT_SITE(pc);
    int observed = 0;
    if (materialize && (g_psx_cyc_batch ||
            (g_psx_cyc_local_acc && *g_psx_cyc_local_acc) ||
            psx_observation_edge_due(cpu, psx_cycle_count, psx_next_service_cycle,
                psx_defer_switch_pending, g_psx_savestate_pending, wait_site,
                psx_observation_wait_active, i_stat, i_mask))) {
        materialize(cpu);
        observed = 1;
    }
    psx_observation_block_edges++;
    /* Native spans charge directly. Legacy/BIOS batches still publish their
     * clock here, without servicing devices. No out-of-line common-edge call. */
    if (g_psx_cyc_batch || (g_psx_cyc_local_acc && *g_psx_cyc_local_acc)) {
        if (!observed && materialize) materialize(cpu);
        observed = 1;
        psx_cyc_batch_flush();
    }
    if (psx_observation_edge_due(cpu, psx_cycle_count, psx_next_service_cycle,
            psx_defer_switch_pending, g_psx_savestate_pending, wait_site,
            psx_observation_wait_active, i_stat, i_mask)) {
        if (!observed && materialize) materialize(cpu);
        observed = 1;
        psx_observation_edge(cpu, pc, wait_site);
    } else if (g_ls_suppress_record > 0) {
        g_ls_suppress_record--;
    }
    return observed;
}
PSX_BLOCK_INLINE int psx_block_edge_materialize(CPUState *cpu, uint32_t pc,
                                               void (*materialize)(CPUState *)) {
    /* These extra private-body exits do not add policy reads/imports to the
     * ordinary public edge, including unoptimized portable fixture builds. */
    if (g_event_step_conservative || psx_in_device_service || g_overlay_flush_pending_cycles) {
        if (materialize) materialize(cpu);
        (void)psx_block_edge_impl(cpu, pc, NULL);
        return 1;
    }
    return psx_block_edge_impl(cpu, pc, materialize);
}
PSX_BLOCK_INLINE void psx_block_edge(CPUState *cpu, uint32_t pc) {
    (void)psx_block_edge_impl(cpu, pc, NULL);
}
#define PSX_BLOCK_EDGE(cpu, pc) psx_block_edge((cpu), (pc))
#else
#ifdef PSX_ENABLE_BLOCK_CYCLES
#define PSX_BLOCK_EDGE(cpu, pc) do { psx_cyc_bb_defer_flush(); \
    psx_check_interrupts_at((cpu), (pc)); } while (0)
#else
#define PSX_BLOCK_EDGE(cpu, pc) psx_check_interrupts_at((cpu), (pc))
#endif
#endif
#endif
#if defined(PSX_OVERLAY_DLL_BUILD) || !defined(PSX_OBSERVATION_TIME) || !PSX_OBSERVATION_TIME
PSX_BLOCK_INLINE int psx_block_edge_materialize(CPUState *cpu, uint32_t pc,
                                               void (*materialize)(CPUState *)) {
    if (materialize) materialize(cpu);
    PSX_BLOCK_EDGE(cpu, pc);
    return 1;
}
#endif
#undef PSX_BLOCK_INLINE
#endif
