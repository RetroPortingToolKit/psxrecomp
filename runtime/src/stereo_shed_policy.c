/* stereo_shed_policy.c — bounded whole-pair shedding. See stereo_shed_policy.h. */

#include "stereo_shed_policy.h"

#include <stdlib.h>
#include <string.h>

#define SHED_EMA_ALPHA 0.1
#define SHED_DEFAULT_PROBE_MS 250.0
#define SHED_DEFAULT_MAX_STREAK 8u
#define SHED_DEFAULT_HYSTERESIS 0.9

static double clamp_double(double v, double lo, double hi) {
    if (!(v == v)) return lo;        /* NaN */
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void stereo_shed_policy_init(StereoShedPolicy *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->enabled = 1;
    p->probe_ms = SHED_DEFAULT_PROBE_MS;
    p->max_streak = SHED_DEFAULT_MAX_STREAK;
    p->hysteresis = SHED_DEFAULT_HYSTERESIS;
}

void stereo_shed_policy_configure_from_env(StereoShedPolicy *p) {
    const char *e;
    if (!p) return;
    e = getenv("PSX_STEREO_SHED");
    if (e) p->enabled = (e[0] && e[0] != '0') ? 1 : 0;
    e = getenv("PSX_STEREO_SHED_PROBE_MS");
    if (e) p->probe_ms = clamp_double(strtod(e, NULL), 10.0, 5000.0);
    e = getenv("PSX_STEREO_SHED_MAX_STREAK");
    if (e) {
        long v = strtol(e, NULL, 0);
        p->max_streak = (uint32_t)(v < 1 ? 1 : (v > 240 ? 240 : v));
    }
    e = getenv("PSX_STEREO_SHED_HYSTERESIS");
    if (e) p->hysteresis = clamp_double(strtod(e, NULL), 0.5, 1.0);
}

void stereo_shed_policy_set_budget(StereoShedPolicy *p, double budget_ms) {
    if (!p) return;
    p->budget_ms = budget_ms > 0.0 ? budget_ms : 0.0;
}

void stereo_shed_policy_reset(StereoShedPolicy *p) {
    if (!p) return;
    p->avg_ms = 0.0;
    p->shedding = 0;
    p->probing = 0;
    p->streak = 0;
    p->streak_start_ms = 0;
    p->pairs_seen = 0;
}

int stereo_shed_policy_should_shed(StereoShedPolicy *p, uint64_t now_ms) {
    if (!p || !p->enabled) return 0;

    if (p->avg_ms > p->budget_ms) {
        if (!p->shedding) {                 /* enter shedding at the first over-budget sample */
            p->shedding = 1;
            p->streak = 0;
            p->streak_start_ms = now_ms;
        }
    } else if (p->avg_ms < p->budget_ms * p->hysteresis) {
        p->shedding = 0;                    /* clearly recovered */
        p->streak = 0;
    } else {
        /* hysteresis band: hold whatever state we are in */
    }

    if (!p->shedding) return 0;

    /* Bounded recovery: neither an elapsed-time nor an attempt-count rule may
     * suppress indefinitely. Whichever bound trips first forces a probe, whose
     * result note_pair() treats as authoritative. The streak's timer restarts
     * here so a persistent overload probes again after the same bound rather
     * than every frame. */
    uint64_t elapsed = now_ms >= p->streak_start_ms ? now_ms - p->streak_start_ms : 0;
    if (p->streak >= p->max_streak || elapsed >= (uint64_t)p->probe_ms) {
        p->probing = 1;
        p->streak = 0;
        p->streak_start_ms = now_ms;
        return 0;                           /* render this one: take a fresh sample */
    }
    p->streak++;
    p->shed_total++;
    return 1;
}

void stereo_shed_policy_note_pair(StereoShedPolicy *p, double pair_ms,
                                  uint64_t now_ms, int ok) {
    if (!p) return;
    if (pair_ms < 0.0) pair_ms = 0.0;
    if (p->probing) {
        /* A forced probe is a deliberate fresh measurement: use it directly
         * instead of blending it away. Otherwise one spike would keep the
         * estimate high for many probes, and the headset would keep showing
         * stale pairs long after the spike ended. */
        p->probing = 0;
        p->avg_ms = pair_ms;
        /* Exit on the deadline itself, not on the hysteresis threshold. A pair
         * time that settles between the two (here 10.0 to 11.111 ms) would
         * otherwise sit in the band forever: the band holds whatever state it
         * is in, so a streak entered during a heavy moment never ends and
         * shedding starves rendering while the pair is in fact meeting its
         * deadline. A latched streak is bounded by the probe interval, which is
         * still the recovery guarantee this policy promises. */
        if (pair_ms <= p->budget_ms) {
            p->shedding = 0;                /* the overload is over */
            p->streak = 0;
        }
    } else {
        p->avg_ms = p->pairs_seen ? p->avg_ms * (1.0 - SHED_EMA_ALPHA) + pair_ms * SHED_EMA_ALPHA
                                  : pair_ms;
    }
    p->pairs_seen++;
    if (ok) {
        p->last_pair_ms = now_ms;
        p->have_pair = 1;
    }
}

uint32_t stereo_shed_policy_streak_sheds(const StereoShedPolicy *p) {
    return p ? p->streak : 0;
}

uint32_t stereo_shed_policy_streak_ms(const StereoShedPolicy *p, uint64_t now_ms) {
    if (!p || !p->shedding || now_ms < p->streak_start_ms) return 0;
    uint64_t d = now_ms - p->streak_start_ms;
    return d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
}

double stereo_shed_policy_avg_ms(const StereoShedPolicy *p) {
    return p ? p->avg_ms : 0.0;
}

uint64_t stereo_shed_policy_pair_age_ms(const StereoShedPolicy *p, uint64_t now_ms) {
    if (!p || !p->have_pair || now_ms < p->last_pair_ms) return 0;
    return now_ms - p->last_pair_ms;
}
