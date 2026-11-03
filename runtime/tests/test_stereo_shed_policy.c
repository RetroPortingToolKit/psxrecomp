/* test_stereo_shed_policy.c — PR2 shedding/recovery policy.
 *
 * Covers the plan's PR2 test list at the policy level: short spike, persistent
 * overload, alternating load (hysteresis), stale estimate, no unbounded streak,
 * transient measurement gaps, and the disable switch. Both-eye integrity and
 * guest-cycle accounting are structural (the policy has no eye or guest state);
 * the render-pass side already asserts eye-cycle equality.
 */

#include "stereo_shed_policy.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail = 1; } \
} while (0)

/* Drive `budget`/`probe`/`streak` config the way a session would, without env. */
static void configure(StereoShedPolicy *p, double avg_ms, uint64_t probe_ms,
                      uint32_t max_streak, double hysteresis) {
    stereo_shed_policy_init(p);
    p->budget_ms = 16.0;
    p->probe_ms = (double)probe_ms;
    p->max_streak = max_streak;
    p->hysteresis = hysteresis;
    if (avg_ms > 0.0) { p->avg_ms = avg_ms; p->pairs_seen = 1; }
}

/* 1: disabled means never shed, whatever the timing. */
static void test_disabled(void) {
    StereoShedPolicy p;
    configure(&p, 40.0, 250, 8, 0.9);
    p.enabled = 0;
    for (uint64_t t = 0; t < 2000; t += 8)
        CHECK(stereo_shed_policy_should_shed(&p, t) == 0);
}

/* 2: a single spike sheds, but the streak is bounded and one good probe ends it
 * (the probe replaces the estimate instead of decaying into it). */
static void test_short_spike_bounded(void) {
    StereoShedPolicy p;
    uint32_t sheds = 0;
    configure(&p, 0.0, 250, 8, 0.9);
    uint64_t t = 0;
    for (int i = 0; i < 20; i++) {          /* establish a fast baseline (~12 ms) */
        stereo_shed_policy_note_pair(&p, 12.0, t, 1);
        t += 16;
        CHECK(stereo_shed_policy_should_shed(&p, t) == 0);
    }
    stereo_shed_policy_note_pair(&p, 80.0, t, 1); /* one heavy frame */
    for (int i = 0; i < 200; i++) {
        if (stereo_shed_policy_should_shed(&p, t)) { sheds++; t += 16; continue; }
        stereo_shed_policy_note_pair(&p, 12.0, t, 1); /* the probe measures fast again */
        t += 16;
    }
    CHECK(sheds > 0);           /* the spike did suppress something */
    CHECK(sheds <= 8u);         /* ...but never more than one bounded streak */
    CHECK(stereo_shed_policy_avg_ms(&p) < 16.0); /* and recovery was immediate */
}

/* 3: sustained overload keeps shedding but must still probe periodically (so a
 * stale average can never suppress forever), and each streak stays bounded. */
static void test_persistent_overload_probes(void) {
    StereoShedPolicy p;
    uint32_t sheds = 0, attempts = 0;
    uint32_t run = 0, max_run = 0;
    configure(&p, 30.0, 250, 8, 0.9);
    uint64_t t = 0;
    for (int i = 0; i < 500; i++) { /* 500 * 8 ms = 4 s of sustained overload */
        if (stereo_shed_policy_should_shed(&p, t)) { sheds++; run++; }
        else { attempts++; if (run > max_run) max_run = run; run = 0;
               stereo_shed_policy_note_pair(&p, 30.0, t, 1); } /* still slow */
        t += 8;
    }
    CHECK(sheds > 0);
    CHECK(attempts > 0);          /* overload does not suppress everything */
    CHECK(max_run <= 8u);         /* every streak bounded by max_streak */
    CHECK(sheds <= 500u - attempts);
}

/* 4: alternating load must not flap: a value inside the hysteresis band holds
 * the current state instead of toggling every frame. */
static void test_alternating_load_hysteresis(void) {
    StereoShedPolicy p;
    int transitions = 0, prev = 0;
    configure(&p, 20.0, 100000, 240, 0.9); /* wide bounds: isolate hysteresis */
    uint64_t t = 0;
    for (int i = 0; i < 200; i++) {
        int s = stereo_shed_policy_should_shed(&p, t);
        if (i && s != prev) transitions++;
        prev = s;
        /* alternate just under budget and inside the band (14.4..16) */
        stereo_shed_policy_note_pair(&p, (i & 1) ? 15.8 : 15.0, t, 1);
        t += 8;
    }
    CHECK(transitions <= 2); /* entered once, at most one exit: no flapping */
}

/* 5: stale estimate — a long gap with no measurement must still be interrupted
 * by a probe within probe_ms, and must not suppress indefinitely. */
static void test_stale_estimate_recovers(void) {
    StereoShedPolicy p;
    configure(&p, 40.0, 250, 1000, 0.9); /* high avg, count bound far away */
    CHECK(stereo_shed_policy_should_shed(&p, 0) == 1);      /* enter */
    CHECK(stereo_shed_policy_should_shed(&p, 8) == 1);
    CHECK(stereo_shed_policy_should_shed(&p, 260) == 0);    /* >= probe_ms: probe */
    CHECK(stereo_shed_policy_should_shed(&p, 260) == 1);    /* fresh streak begins */
    CHECK(stereo_shed_policy_should_shed(&p, 520) == 0);    /* bounded again */
}

/* 6: a measurement gap (e.g. OpenXR failing so render_stereo never runs) leaves
 * the average untouched, but the time bound still forces a probe. */
static void test_transient_measurement_gap(void) {
    StereoShedPolicy p;
    uint64_t first_probe = 0;
    configure(&p, 30.0, 250, 8, 0.9);
    uint64_t t = 0;
    for (int i = 0; i < 200; i++) {
        if (stereo_shed_policy_should_shed(&p, t) == 0) { first_probe = t; break; }
        t += 8; /* no note_pair: the average is never refreshed */
    }
    CHECK(first_probe > 0 && first_probe <= 256u);
}

/* 7: a failure that aborts the attempt early is still a measurement, so the
 * average falls and shedding ends (no unbounded failure loop). */
static void test_failure_ends_streak(void) {
    StereoShedPolicy p;
    configure(&p, 30.0, 100000, 240, 0.9);
    CHECK(stereo_shed_policy_should_shed(&p, 0) == 1);
    stereo_shed_policy_note_pair(&p, 3.0, 8, 0); /* aborted early, not published */
    for (int i = 0; i < 40; i++)
        stereo_shed_policy_note_pair(&p, 3.0, (uint64_t)(16 + i * 8), 0);
    CHECK(stereo_shed_policy_should_shed(&p, 1000) == 0);
}

/* 8: pair age tracks the retained pair independently of the compute estimate. */
static void test_pair_age_separate(void) {
    StereoShedPolicy p;
    configure(&p, 0.0, 250, 8, 0.9);
    CHECK(stereo_shed_policy_pair_age_ms(&p, 100) == 0); /* nothing published yet */
    stereo_shed_policy_note_pair(&p, 12.0, 1000, 1);
    CHECK(stereo_shed_policy_pair_age_ms(&p, 1000) == 0);
    CHECK(stereo_shed_policy_pair_age_ms(&p, 1050) == 50);
    /* a later failed attempt must not refresh the age */
    stereo_shed_policy_note_pair(&p, 12.0, 1100, 0);
    CHECK(stereo_shed_policy_pair_age_ms(&p, 1100) == 100);
}

/* 9: reset clears the estimate so a new scene does not inherit it. */
static void test_reset_on_transition(void) {
    StereoShedPolicy p;
    configure(&p, 40.0, 250, 8, 0.9);
    CHECK(stereo_shed_policy_should_shed(&p, 0) == 1);
    stereo_shed_policy_reset(&p);
    CHECK(stereo_shed_policy_avg_ms(&p) == 0.0);
    CHECK(stereo_shed_policy_should_shed(&p, 10) == 0); /* fresh, under budget */
}

/* 11: a pair time that settles *inside* the hysteresis band must not latch the
 * shedding state. Measured case (SteamVR null HMD at 90 Hz): deadline
 * 11.111 ms, pair 10.4 ms, so the band is 10.0 to 11.111. Shedding entered
 * during a heavy moment and then never ended, because the band holds whatever
 * state it is in and the old probe exit test was the band's lower edge. That
 * cost 26 sheds/s against 3.7 published pairs/s. The probe is authoritative and
 * must exit on the deadline itself. */
static void test_band_latch_recovers(void) {
    StereoShedPolicy p;
    uint32_t sheds = 0;
    configure(&p, 0.0, 250, 8, 0.9);
    p.budget_ms = 11.111;
    uint64_t t = 0;
    stereo_shed_policy_note_pair(&p, 15.0, t, 1);   /* heavy moment: enter shedding */
    for (int i = 0; i < 60; i++) {                  /* 2 s at a 33 ms frame */
        if (stereo_shed_policy_should_shed(&p, t)) { sheds++; t += 33; continue; }
        stereo_shed_policy_note_pair(&p, 10.4, t, 1); /* steady state inside the band */
        t += 33;
    }
    CHECK(sheds <= 12u);                    /* one bounded streak, then rendering resumes */
    CHECK(stereo_shed_policy_avg_ms(&p) <= 11.111);
}

/* 10: env parsing clamps to safe ranges (and PSX_STEREO_SHED=0 disables). */
static void test_env(void) {
    StereoShedPolicy p;
    stereo_shed_policy_init(&p);
    CHECK(p.enabled == 1 && p.max_streak >= 1u && p.probe_ms > 0.0);
    stereo_shed_policy_configure_from_env(&p); /* no env set in CI: defaults hold */
    CHECK(p.max_streak >= 1u && p.max_streak <= 240u);
    CHECK(p.hysteresis >= 0.5 && p.hysteresis <= 1.0);
}

int main(void) {
    test_disabled();
    test_short_spike_bounded();
    test_persistent_overload_probes();
    test_alternating_load_hysteresis();
    test_stale_estimate_recovers();
    test_transient_measurement_gap();
    test_failure_ends_streak();
    test_pair_age_separate();
    test_reset_on_transition();
    test_band_latch_recovers();
    test_env();
    if (g_fail) { printf("stereo_shed_policy: FAILED\n"); return 1; }
    printf("stereo_shed_policy: all checks passed\n");
    return 0;
}
