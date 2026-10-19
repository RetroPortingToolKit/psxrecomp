#include "freeze_dump_policy.h"

#include <stdio.h>

static unsigned wedge_priority(uint32_t wedge_kind) {
    switch (wedge_kind) {
    case 1: return 4; /* hard freeze */
    case 2: return 3; /* exception re-entry storm */
    case 3: return 2; /* slow frames */
    case 5: return 1; /* logical spin */
    default: return 0;
    }
}

int freeze_dump_policy_observe(FreezeDumpPolicy *policy, uint32_t wedge_kind,
                               int fatal_active) {
    if (!policy) return 0;
    if (fatal_active) return 0;

    unsigned priority = wedge_priority(wedge_kind);
    if (wedge_kind != 0 && priority == 0) return 0;

    if (wedge_kind != 0) {
        policy->healthy_ticks = 0;
        if (policy->in_wedge) {
            /* Only a hard freeze can reserve the second slot during an
             * existing event. Lesser severity changes stay in one incident. */
            if (wedge_kind != 1 || policy->wedge_kind == 1) return 0;
        }

        policy->in_wedge = 1;
        policy->wedge_kind = wedge_kind;

        if (wedge_kind == 1) {
            if (!policy->hard_slot_consumed) {
                policy->pending_kind = wedge_kind;
                return 1;
            }
        } else if (!policy->ordinary_slot_consumed) {
            policy->pending_kind = wedge_kind;
            return 1;
        }

        policy->suppressed_events++;
        return 0;
    }

    if (!policy->in_wedge) return 0;

    if (++policy->healthy_ticks >= FREEZE_DUMP_REARM_HEALTHY_TICKS) {
        policy->healthy_ticks = 0;
        policy->wedge_kind = 0;
        policy->in_wedge = 0;
    }
    return 0;
}

void freeze_dump_policy_record_result(FreezeDumpPolicy *policy, int written) {
    if (!policy || policy->pending_kind == 0) return;

    int hard = policy->pending_kind == 1;
    if (written) {
        if (hard)
            policy->hard_slot_consumed = 1;
        else
            policy->ordinary_slot_consumed = 1;
        policy->automatic_dumps++;
    } else {
        uint32_t *attempts = hard ? &policy->hard_failed_attempts
                                  : &policy->ordinary_failed_attempts;
        int *consumed = hard ? &policy->hard_slot_consumed
                             : &policy->ordinary_slot_consumed;
        policy->failed_dumps++;
        if (++*attempts >= FREEZE_DUMP_MAX_FAILED_ATTEMPTS) *consumed = 1;

        /* A failed attempt ends this event. The next sample can retry only
         * while the slot remains within its fixed attempt limit. */
        policy->healthy_ticks = 0;
        policy->wedge_kind = 0;
        policy->in_wedge = 0;
    }
    policy->pending_kind = 0;
}

int freeze_dump_format_path(char *out, size_t cap, const char *backend,
                            long long wall, uint32_t sequence) {
    if (!out || cap == 0 || !backend) return 0;
    int n = snprintf(out, cap, "psx_freeze_dump_%s_%lld_%u.json",
                     backend, wall, sequence);
    return n > 0 && (size_t)n < cap;
}

int freeze_distinct_add(FreezeDistinct *set, uint32_t value) {
    if (set->count > FREEZE_WEDGE_SPIN_MAX_FNS) return 1;
    for (uint32_t i = 0; i < set->count; i++)
        if (set->values[i] == value) return 0;
    set->values[set->count++] = value;
    return set->count > FREEZE_WEDGE_SPIN_MAX_FNS;
}

uint32_t freeze_window_fn_diversity(uint64_t lo, uint64_t hi, uint64_t oldest,
                                    int (*get)(uint64_t seq, uint32_t *fn,
                                               int *in_exception)) {
    if (lo < oldest) lo = oldest;
    FreezeDistinct set = {{0}, 0};
    int any = 0;
    for (uint64_t seq = lo; seq < hi; seq++) {
        uint32_t fn = 0;
        int in_exception = 0;
        if (!get(seq, &fn, &in_exception) || in_exception) continue;
        any = 1;
        if (freeze_distinct_add(&set, fn)) break;
    }
    return any ? set.count : FREEZE_WEDGE_FN_DIVERSITY_UNKNOWN;
}
uint32_t freeze_wedge_classify(const FreezeWedgeSample *window, uint32_t count,
                               uint32_t fn_diversity,
                               int (*mdec_recently_active)(uint32_t frames)) {
    if (!window || count < 2u) return 0;
    const FreezeWedgeSample *oldest = &window[0];
    const FreezeWedgeSample *newest = &window[count - 1u];
    uint64_t frame_delta = (newest->frame_count >= oldest->frame_count)
                           ? (newest->frame_count - oldest->frame_count) : 0;
    uint64_t excre_delta = (newest->exc_reentry >= oldest->exc_reentry)
                           ? (newest->exc_reentry - oldest->exc_reentry) : 0;

    if (frame_delta == 0) return FREEZE_WEDGE_HARD;
    if (excre_delta / frame_delta > FREEZE_WEDGE_EXC_REENTRY_PER_FRAME_THRESHOLD)
        return FREEZE_WEDGE_REENTRY_STORM;
    if (frame_delta < FREEZE_WEDGE_SLOW_FRAMES_MAX_DELTA) return FREEZE_WEDGE_SLOW_FRAMES;

    for (uint32_t i = 1; i < count; i++) {
        if (window[i].current_func    != oldest->current_func ||
            window[i].last_store_pc   != oldest->last_store_pc ||
            window[i].dirty_ram_insns != oldest->dirty_ram_insns)
            return 0;
    }
    if (fn_diversity != FREEZE_WEDGE_FN_DIVERSITY_UNKNOWN &&
        fn_diversity > FREEZE_WEDGE_SPIN_MAX_FNS)
        return 0;
    /* During an FMV the guest parks in BIOS MDEC code with every pinned
     * signal legitimately still while frames advance. */
    if (mdec_recently_active &&
        mdec_recently_active((uint32_t)(frame_delta > 0xFFFFFFFEu ? 0xFFFFFFFEu : frame_delta) + 1u))
        return 0;
    return FREEZE_WEDGE_SPIN;
}