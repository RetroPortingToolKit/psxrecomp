#include "freeze_dump_policy.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT_EQ(actual, expected) do {                                      \
    unsigned got_ = (unsigned)(actual);                                       \
    unsigned want_ = (unsigned)(expected);                                    \
    if (got_ != want_) {                                                      \
        fprintf(stderr, "%s:%d: got %u, expected %u\n",                     \
                __FILE__, __LINE__, got_, want_);                             \
        failures++;                                                           \
    }                                                                         \
} while (0)

static int observe(FreezeDumpPolicy *policy, uint32_t kind) {
    return freeze_dump_policy_observe(policy, kind, 0);
}

static void observe_healthy(FreezeDumpPolicy *policy, uint32_t ticks) {
    for (uint32_t i = 0; i < ticks; i++) EXPECT_EQ(observe(policy, 0), 0);
}

#define UNK FREEZE_WEDGE_FN_DIVERSITY_UNKNOWN

static int mdec_idle(uint32_t frames) { (void)frames; return 0; }
static int mdec_busy(uint32_t frames) { (void)frames; return 1; }

/* 20 samples, frames advancing 9 per tick, guest pinned in one kernel loop. */
static void spin_window(FreezeWedgeSample *w, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        w[i].frame_count = 1746u + 9u * i;
        w[i].exc_reentry = 2000u + i;
        w[i].dirty_ram_insns = 908846u;
        w[i].current_func = 0x00000F40u;
        w[i].last_store_pc = 0x8003A230u;
    }
}

static int sample_profile(uint64_t seq, uint32_t *fn, int *exception) {
    if (seq >= 12u) return 0;
    *fn = (uint32_t)seq;
    *exception = seq == 2u;
    return 1;
}

int main(void) {
    FreezeDumpPolicy policy = {0};

    /* Authorization does not consume a slot until the file reaches disk. */
    EXPECT_EQ(observe(&policy, 5), 1);
    EXPECT_EQ(policy.automatic_dumps, 0);
    freeze_dump_policy_record_result(&policy, 1);
    EXPECT_EQ(policy.automatic_dumps, 1);
    EXPECT_EQ(policy.ordinary_slot_consumed, 1);
    EXPECT_EQ(observe(&policy, 5), 0);

    /* A short healthy gap does not turn one oscillating wedge into two. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS - 1u);
    EXPECT_EQ(observe(&policy, 5), 0);

    /* Lesser severity changes share the ordinary slot. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&policy, 3), 0);
    EXPECT_EQ(policy.suppressed_events, 1);

    /* A hard freeze owns the reserved second slot during the same event. */
    EXPECT_EQ(observe(&policy, 1), 1);
    freeze_dump_policy_record_result(&policy, 1);
    EXPECT_EQ(policy.automatic_dumps, FREEZE_DUMP_AUTO_LIMIT);
    EXPECT_EQ(policy.hard_slot_consumed, 1);
    EXPECT_EQ(observe(&policy, 1), 0);

    /* Re-entry storms are known events and are counted after the limit. */
    observe_healthy(&policy, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&policy, 2), 0);
    EXPECT_EQ(policy.suppressed_events, 2);

    /* Fatal state bypasses the automatic policy without consuming state. */
    FreezeDumpPolicy fatal = {0};
    EXPECT_EQ(freeze_dump_policy_observe(&fatal, 5, 1), 0);
    EXPECT_EQ(fatal.in_wedge, 0);
    EXPECT_EQ(fatal.automatic_dumps, 0);
    EXPECT_EQ(observe(&fatal, 5), 1);

    /* A failed write keeps its slot for a bounded retry. */
    FreezeDumpPolicy retry = {0};
    EXPECT_EQ(observe(&retry, 5), 1);
    freeze_dump_policy_record_result(&retry, 0);
    EXPECT_EQ(retry.automatic_dumps, 0);
    EXPECT_EQ(retry.failed_dumps, 1);
    EXPECT_EQ(observe(&retry, 5), 1);
    freeze_dump_policy_record_result(&retry, 1);
    EXPECT_EQ(retry.automatic_dumps, 1);
    EXPECT_EQ(retry.failed_dumps, 1);

    /* Three failed attempts consume the ordinary slot and stop retries. */
    FreezeDumpPolicy exhausted = {0};
    for (uint32_t i = 0; i < FREEZE_DUMP_MAX_FAILED_ATTEMPTS; i++) {
        EXPECT_EQ(observe(&exhausted, 5), 1);
        freeze_dump_policy_record_result(&exhausted, 0);
    }
    EXPECT_EQ(exhausted.failed_dumps, FREEZE_DUMP_MAX_FAILED_ATTEMPTS);
    EXPECT_EQ(exhausted.ordinary_slot_consumed, 1);
    EXPECT_EQ(observe(&exhausted, 5), 0);
    EXPECT_EQ(exhausted.suppressed_events, 1);

    /* An exhausted ordinary slot does not consume the hard-freeze slot. */
    EXPECT_EQ(observe(&exhausted, 1), 1);
    freeze_dump_policy_record_result(&exhausted, 1);
    EXPECT_EQ(exhausted.hard_slot_consumed, 1);
    EXPECT_EQ(exhausted.automatic_dumps, 1);

    /* Unknown future kinds do not consume or suppress an existing slot. */
    FreezeDumpPolicy unknown = {0};
    EXPECT_EQ(observe(&unknown, 7), 0);
    EXPECT_EQ(unknown.in_wedge, 0);
    EXPECT_EQ(unknown.suppressed_events, 0);

    /* A hard-first event leaves the ordinary evidence slot available. */
    FreezeDumpPolicy hard_first = {0};
    EXPECT_EQ(observe(&hard_first, 1), 1);
    freeze_dump_policy_record_result(&hard_first, 1);
    observe_healthy(&hard_first, FREEZE_DUMP_REARM_HEALTHY_TICKS);
    EXPECT_EQ(observe(&hard_first, 5), 1);
    freeze_dump_policy_record_result(&hard_first, 1);
    EXPECT_EQ(hard_first.automatic_dumps, FREEZE_DUMP_AUTO_LIMIT);

    /* Equal wall seconds still produce distinct file names. */
    char path0[128], path1[128];
    EXPECT_EQ(freeze_dump_format_path(path0, sizeof(path0),
                                      "psx-runtime", 1234, 0), 1);
    EXPECT_EQ(freeze_dump_format_path(path1, sizeof(path1),
                                      "psx-runtime", 1234, 1), 1);
    EXPECT_EQ(strcmp(path0, path1) != 0, 1);
    EXPECT_EQ(freeze_dump_format_path(path0, 4, "psx-runtime", 1234, 0), 0);

    /* Wedge classification over the whole window. */
    FreezeWedgeSample w[20];
    spin_window(w, 20);
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_idle), FREEZE_WEDGE_SPIN);
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, NULL), FREEZE_WEDGE_SPIN);
    /* A busy native window is healthy, while a small wait-loop remains detectable. */
    EXPECT_EQ(freeze_wedge_classify(w, 20, 9u, mdec_idle), 0);
    EXPECT_EQ(freeze_wedge_classify(w, 20, 8u, mdec_idle), FREEZE_WEDGE_SPIN);
    EXPECT_EQ(freeze_window_fn_diversity(0, 12, 0, sample_profile), 9u);
    EXPECT_EQ(freeze_window_fn_diversity(0, 8, 5, sample_profile), 3u);
    EXPECT_EQ(freeze_window_fn_diversity(2, 3, 0, sample_profile), UNK);
    EXPECT_EQ(freeze_window_fn_diversity(20, 30, 0, sample_profile), UNK);
    /* FMV decode parks the guest the same way; MDEC activity vetoes it. */
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_busy), 0);
    /* Static-code main loop (LoD 2026-10-12 dump): the window ends agree but
     * intermediate ticks store elsewhere. Not a spin. */
    w[3].last_store_pc = 0x800461F0u;
    w[11].last_store_pc = 0x800FBD44u;
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_idle), 0);
    spin_window(w, 20);
    w[7].current_func = 0x1FC02918u;
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_idle), 0);
    spin_window(w, 20);
    w[19].dirty_ram_insns += 4u;
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_idle), 0);
    /* Frame-progress kinds take precedence over the spin test. */
    spin_window(w, 20);
    for (uint32_t i = 0; i < 20; i++) w[i].frame_count = 500u;
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_busy), FREEZE_WEDGE_HARD);
    for (uint32_t i = 0; i < 20; i++) w[i].frame_count = 500u + (i == 19 ? 5u : 0u);
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_idle), FREEZE_WEDGE_SLOW_FRAMES);
    spin_window(w, 20);
    w[19].exc_reentry = w[0].exc_reentry +
        (uint64_t)(FREEZE_WEDGE_EXC_REENTRY_PER_FRAME_THRESHOLD + 1u) * 171u;
    EXPECT_EQ(freeze_wedge_classify(w, 20, UNK, mdec_idle), FREEZE_WEDGE_REENTRY_STORM);
    EXPECT_EQ(freeze_wedge_classify(w, 1, UNK, mdec_idle), 0);
    EXPECT_EQ(freeze_wedge_classify(NULL, 20, UNK, mdec_idle), 0);

    if (failures != 0) {        fprintf(stderr, "freeze dump policy: %d failure(s)\n", failures);
        return 1;
    }
    puts("freeze dump policy: all checks passed");
    return 0;
}
