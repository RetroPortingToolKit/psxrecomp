#ifndef PSXRECOMP_FREEZE_DUMP_POLICY_H
#define PSXRECOMP_FREEZE_DUMP_POLICY_H

#include <stddef.h>
#include <stdint.h>

/* Full freeze dumps contain several large trace rings. Keep automatic
 * watchdog output bounded while leaving deliberate fatal dumps independent. */
#define FREEZE_DUMP_ORDINARY_SLOTS 1u
#define FREEZE_DUMP_HARD_SLOTS 1u
#define FREEZE_DUMP_AUTO_LIMIT \
    (FREEZE_DUMP_ORDINARY_SLOTS + FREEZE_DUMP_HARD_SLOTS)
#define FREEZE_DUMP_MAX_FAILED_ATTEMPTS 3u
/* Policy hysteresis, independent of the detector's sample window. The total
 * time before rearm can therefore be longer than this tick count alone. */
#define FREEZE_DUMP_REARM_HEALTHY_TICKS 20u

typedef struct FreezeDumpPolicy {
    uint32_t automatic_dumps;
    uint32_t failed_dumps;
    uint32_t suppressed_events;
    uint32_t healthy_ticks;
    uint32_t wedge_kind;
    uint32_t pending_kind;
    uint32_t ordinary_failed_attempts;
    uint32_t hard_failed_attempts;
    int ordinary_slot_consumed;
    int hard_slot_consumed;
    int in_wedge;
} FreezeDumpPolicy;

/* Observe one watchdog sample. Returns nonzero when the caller must write an
 * automatic full dump. A new event requires a sustained healthy interval.
 * Fatal state bypasses this automatic policy and does not change its budget. */
int freeze_dump_policy_observe(FreezeDumpPolicy *policy, uint32_t wedge_kind,
                               int fatal_active);

/* Record whether the authorized full dump reached disk. A slot permits a
 * bounded number of failed attempts before the policy consumes that slot. */
void freeze_dump_policy_record_result(FreezeDumpPolicy *policy, int written);

/* Wedge kinds reported by freeze_wedge_classify (0 = healthy). */
#define FREEZE_WEDGE_HARD          1u  /* frame_count did not advance */
#define FREEZE_WEDGE_REENTRY_STORM 2u  /* exception re-entries per frame over threshold */
#define FREEZE_WEDGE_SLOW_FRAMES   3u  /* frames advanced below the slow-frames floor */
#define FREEZE_WEDGE_SPIN          5u  /* guest pinned in one loop while frames advance */

#define FREEZE_WEDGE_EXC_REENTRY_PER_FRAME_THRESHOLD 20000u /* ~10x chronic 2K/frame */
#define FREEZE_WEDGE_SLOW_FRAMES_MAX_DELTA 10u   /* <5 fps avg over the 2s window */

/* One heartbeat sample, as far as wedge classification needs it. */
typedef struct FreezeWedgeSample {
    uint64_t frame_count;
    uint64_t exc_reentry;
    uint64_t dirty_ram_insns;
    uint32_t current_func;
    uint32_t last_store_pc;
} FreezeWedgeSample;

/* A spinning guest enters only its wait loop plus whatever the interrupt
 * handlers last stamped; a running game loop enters dozens of functions per
 * window (LoD title/field/battle >= 13 per 120 frames, measured 2026-10-12). */
#define FREEZE_WEDGE_SPIN_MAX_FNS 8u
#define FREEZE_WEDGE_FN_DIVERSITY_UNKNOWN UINT32_MAX

/* Distinct-value counter that stops counting past its capacity. */
typedef struct FreezeDistinct {
    uint32_t values[FREEZE_WEDGE_SPIN_MAX_FNS + 1u];
    uint32_t count;
} FreezeDistinct;

/* Add one value; returns nonzero once more than FREEZE_WEDGE_SPIN_MAX_FNS
 * distinct values have been seen (the caller can stop scanning). */
int freeze_distinct_add(FreezeDistinct *set, uint32_t value);

/* Distinct functions entered outside exceptions over profile samples
 * [lo, hi), clamped to oldest (the ring's oldest held sample). get() returns
 * 0 for an evicted/unavailable sample. Stops counting past
 * FREEZE_WEDGE_SPIN_MAX_FNS; FREEZE_WEDGE_FN_DIVERSITY_UNKNOWN when no
 * non-exception sample covers the window. */
uint32_t freeze_window_fn_diversity(uint64_t lo, uint64_t hi, uint64_t oldest,
                                    int (*get)(uint64_t seq, uint32_t *fn,
                                               int *in_exception));
/* Classify a window of consecutive samples, oldest first. A spin requires the
 * executing function, last store PC and dirty-RAM instruction count to hold
 * one value at EVERY sample, AND (when known) at most FREEZE_WEDGE_SPIN_MAX_FNS
 * distinct guest functions entered outside exceptions over the window.
 * Statically compiled code stamps neither current_func nor dirty_ram_insns, so
 * the pinned signature alone also matches a healthy static main loop; the
 * function-entry stamp is maintained by all generated code. fn_diversity is
 * FREEZE_WEDGE_FN_DIVERSITY_UNKNOWN when no profile samples cover the window.
 * mdec_recently_active (may be NULL) vetoes a spin while FMV decoding parks
 * the guest in BIOS MDEC code. */
uint32_t freeze_wedge_classify(const FreezeWedgeSample *window, uint32_t count,
                               uint32_t fn_diversity,
                               int (*mdec_recently_active)(uint32_t frames));

/* Format a unique dump path. The sequence prevents same-second truncation. */
int freeze_dump_format_path(char *out, size_t cap, const char *backend,
                            long long wall, uint32_t sequence);

#endif /* PSXRECOMP_FREEZE_DUMP_POLICY_H */
