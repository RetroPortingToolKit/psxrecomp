/* Generated block boundaries. Native observation timing keeps ordinary edges
 * in the caller; the original delivery implementation owns every slow edge. */
#ifndef PSX_BLOCK_EDGE_H
#define PSX_BLOCK_EDGE_H
#include "interrupts.h"

#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME && !defined(PSX_OVERLAY_DLL_BUILD)
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
void psx_observation_edge(struct CPUState *cpu, uint32_t resume_pc, int wait_site);
#ifdef __cplusplus
}
#endif
#if defined(__GNUC__) || defined(__clang__)
#define PSX_BLOCK_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define PSX_BLOCK_INLINE static __forceinline
#else
#define PSX_BLOCK_INLINE static inline
#endif
PSX_BLOCK_INLINE int psx_observation_delivery_pending(const CPUState *cpu) {
    uint32_t sr = cpu->cop0[12] /* SR */;
    return (sr & 1u) && ((cpu->cop0[13] /* CAUSE */ & sr & 0x0300u) ||
           ((sr & 0x0400u) && (i_stat & i_mask)));
}
PSX_BLOCK_INLINE void psx_block_edge(CPUState *cpu, uint32_t pc) {
    if (g_psx_guest_time_frozen || g_ls_replay_active) return;
    const int wait_site = PSX_OBSERVATION_WAIT_SITE(pc);
    psx_observation_block_edges++;
    /* Native spans charge directly. Legacy/BIOS batches still publish their
     * clock here, without servicing devices. No out-of-line common-edge call. */
    if (g_psx_cyc_batch || (g_psx_cyc_local_acc && *g_psx_cyc_local_acc))
        psx_cyc_batch_flush();
    if (psx_next_service_cycle == 0u || psx_cycle_count >= psx_next_service_cycle ||
        psx_defer_switch_pending || g_psx_savestate_pending || wait_site ||
        psx_observation_wait_active || psx_observation_delivery_pending(cpu)) {
        psx_observation_edge(cpu, pc, wait_site);
    } else if (g_ls_suppress_record > 0) {
        g_ls_suppress_record--;
    }
}
#undef PSX_BLOCK_INLINE
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
