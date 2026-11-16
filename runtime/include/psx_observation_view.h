#ifndef PSX_OBSERVATION_VIEW_H
#define PSX_OBSERVATION_VIEW_H
#include <stdint.h>

struct CPUState;
/* ABI-owned references to the CPU owner's maintained clock and edge state.
 * Only the build selection, audited PC list and process-fixed legacy option
 * are values. Mutable clock/guard state is never cached in an overlay. */
typedef struct PSXObservationView {
    uint32_t bytes, model;
    int legacy_unit_defer;
    uint64_t *clock, *deadline, *sample;
    uint32_t *scale_recip, *scale_frac, *batch;
    uint32_t **local_acc;
    int *device_service, *conservative, *frozen, *replay;
    int *defer_switch, *save_pending, *suppress_record, *wait_active;
    uint32_t *irq_status, *irq_mask;
    int *shadow, *suppress_irq, *cd_only, *defer_cd;
    uint32_t *irq_budget, *irq_ratelimit;
    void (**flush_owner)(void);
    const uint32_t *wait_sites;
    uint32_t wait_site_count;
    uint64_t *block_edges;
    void (*edge)(struct CPUState *, uint32_t pc, int wait_site, uint32_t charge);
    void (*fallback)(struct CPUState *, uint32_t pc, uint32_t charge);
} PSXObservationView;
#endif
