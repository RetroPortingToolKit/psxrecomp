#ifndef PSX_STEREO_SHED_POLICY_H
#define PSX_STEREO_SHED_POLICY_H

/* Whole-pair stereo shedding policy, extracted so it can be reasoned about and
 * tested without a GPU, a headset or a guest.
 *
 * WHY THIS EXISTS
 * ---------------
 * The original rule was `avg_pair_ms > budget_ms && ++retry < 30` — an
 * attempt counter with no notion of time. One short overload latched the
 * average above budget and then silently suppressed the next 29 pair requests:
 * a single heavy frame produced a long run of stale frames. There was also no
 * way to say "stop shedding and just measure", and no bound on how long
 * suppression could last.
 *
 * THIS POLICY
 * -----------
 *   * Enters shedding when the pair-time EMA is over budget, and leaves it only
 *     when a fresh measurement is clearly under `hysteresis * budget` (so noise
 *     at the boundary does not flap).
 *   * While shedding, suppression lasts at most `probe_ms` of wall clock and at
 *     most `max_streak` consecutive sheds, whichever comes first. Whichever way
 *     it ends, the next frame is rendered, which takes a fresh measurement.
 *     A stale average can therefore never suppress indefinitely: recovery is
 *     bounded in TIME, not in attempt count.
 *   * A forced probe is authoritative: it REPLACES the running estimate rather
 *     than blending into it. That is what makes a one-off spike cheap — one
 *     good probe ends the streak, instead of the EMA having to decay over
 *     several probes while the headset keeps showing stale pairs.
 *   * `enabled = 0` disables shedding entirely (baseline diagnostic: measure
 *     what happens with no suppression at all).
 *
 * The policy decides only whether to skip a pair. It never decides which eyes
 * to publish; the caller keeps whole-pair publish (a stale frame retains the
 * previous complete pair, never an unmatched eye).
 *
 * All time is passed in by the caller (host monotonic ms), so there is no clock
 * dependency here and the interesting cases are unit-testable.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StereoShedPolicy {
    /* configuration */
    int      enabled;        /* 0 = never shed (diagnostic) */
    double   budget_ms;      /* per-pair budget; set each frame from the cadence */
    double   probe_ms;       /* bound on one shedding streak (wall clock) */
    uint32_t max_streak;     /* bound on one shedding streak (count) */
    double   hysteresis;     /* resume threshold as a fraction of budget (0..1) */
    /* state */
    double   avg_ms;         /* EMA of measured pair time */
    int      shedding;       /* currently suppressing */
    int      probing;        /* the last decision forced a probe; the next
                              * measurement is authoritative */
    uint32_t streak;         /* consecutive sheds in this streak */
    uint64_t streak_start_ms;/* when this streak began */
    uint64_t last_pair_ms;   /* when the last pair was published */
    int      have_pair;
    uint64_t pairs_seen;     /* measurements folded into the average */
    uint64_t shed_total;     /* pairs skipped since reset */
} StereoShedPolicy;

void stereo_shed_policy_init(StereoShedPolicy *p);
/* Reads PSX_STEREO_SHED (0 disables), PSX_STEREO_SHED_PROBE_MS,
 * PSX_STEREO_SHED_MAX_STREAK and PSX_STEREO_SHED_HYSTERESIS, clamping each to a
 * safe range. Called once per session. */
void stereo_shed_policy_configure_from_env(StereoShedPolicy *p);
void stereo_shed_policy_set_budget(StereoShedPolicy *p, double budget_ms);
/* Clears the timing estimate and streak but keeps configuration. Call on a
 * scene/mode transition: a new scene must not inherit the previous one's
 * estimate, and a cadence change invalidates the budget. */
void stereo_shed_policy_reset(StereoShedPolicy *p);

/* 1 = skip this frame's pair, 0 = render it. */
int stereo_shed_policy_should_shed(StereoShedPolicy *p, uint64_t now_ms);
/* Fold a measured pair attempt into the estimate. `ok` = a complete pair was
 * published (so the retained pair is now fresh). */
void stereo_shed_policy_note_pair(StereoShedPolicy *p, double pair_ms,
                                  uint64_t now_ms, int ok);

uint32_t stereo_shed_policy_streak_sheds(const StereoShedPolicy *p);
uint32_t stereo_shed_policy_streak_ms(const StereoShedPolicy *p, uint64_t now_ms);
double   stereo_shed_policy_avg_ms(const StereoShedPolicy *p);
/* Age of the retained pair, or 0 when no pair has been published yet. */
uint64_t stereo_shed_policy_pair_age_ms(const StereoShedPolicy *p, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PSX_STEREO_SHED_POLICY_H */
