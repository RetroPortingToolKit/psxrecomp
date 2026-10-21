#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "frame_interpolation.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

static int run_interval(FrameInterpolationSchedule *schedule, uint64_t now,
                        double source_hz, double target_hz,
                        float *first_alpha, float *last_alpha) {
    uint64_t deadline;
    float alpha;
    int count = 0;

    CHECK(frame_interpolation_schedule_begin(
              schedule, now, 1000000u, source_hz, target_hz),
          "valid rates should produce an interval");
    while (frame_interpolation_schedule_next(
               schedule, now, &deadline, &alpha)) {
        CHECK(deadline >= now, "presentation deadline must not be in the past");
        CHECK(deadline <= frame_interpolation_schedule_end(schedule),
              "presentation deadline must stay inside the source interval");
        if (count == 0 && first_alpha) *first_alpha = alpha;
        if (last_alpha) *last_alpha = alpha;
        count++;
    }
    return count;
}

/* Drive `ticks` 30 Hz game frames (flip every `period` VBlanks) through the
 * flip tracker + phase-window scheduler at a 59.94 Hz VBlank clock, as the GL
 * presenter does in FLIP mode. Returns the number of presents; checks that
 * alpha is monotonic inside each game frame and ends at 1. */
static int run_flip_ticks(double target_hz, uint32_t period, int ticks,
                          int *min_per_tick, int *max_per_tick) {
    FrameInterpolationSchedule schedule = {0};
    FrameFlipTracker tracker;
    const uint64_t freq = 1000000000u;   /* ns */
    const double vblank_hz = 59.94;
    uint64_t now = 5000000000u;
    int total = 0;
    frame_flip_tracker_reset(&tracker);
    if (min_per_tick) *min_per_tick = 1 << 30;
    if (max_per_tick) *max_per_tick = 0;
    for (int t = 0; t < ticks + 2; t++) {
        float prev_alpha = -1.0f, last_alpha = -1.0f;
        int in_tick = 0;
        for (uint32_t v = 0; v < period; v++) {
            double lo, hi;
            uint64_t deadline;
            float alpha;
            uint32_t k = frame_flip_tracker_vblank(&tracker, v == 0, &lo, &hi);
            CHECK(k == v, "tracker k must count VBlanks since the flip");
            CHECK(frame_interpolation_schedule_begin_phase(
                      &schedule, now, freq, vblank_hz, target_hz, lo, hi),
                  "phase interval should plan");
            while (frame_interpolation_schedule_next(&schedule, now, &deadline,
                                                     &alpha)) {
                if (t >= 2) {   /* after the period estimate settles */
                    CHECK(alpha + 1e-6f >= prev_alpha,
                          "alpha must be monotonic within one game frame");
                    CHECK(alpha >= (float)lo - 1e-6f &&
                          alpha <= (float)hi + 1e-6f,
                          "alpha must stay inside the VBlank's phase window");
                }
                prev_alpha = last_alpha = alpha;
                in_tick++;
                now = deadline;
            }
            now = frame_interpolation_schedule_end(&schedule);
        }
        if (t >= 2) {
            /* Output deadlines are a free-running grid, so the last present
             * of a game frame lands within one output period of its end. */
            double one_output = (vblank_hz / (double)period) / target_hz;
            CHECK(last_alpha >= (float)(1.0 - one_output) - 1e-4f,
                  "the last present of a game frame should be within one "
                  "output period of alpha 1");
            total += in_tick;
            if (min_per_tick && in_tick < *min_per_tick) *min_per_tick = in_tick;
            if (max_per_tick && in_tick > *max_per_tick) *max_per_tick = in_tick;
        }
    }
    return total;
}

static void test_flip_tracker_table(void) {
    /* new_frame sequence -> expected (k, period after the call). */
    static const struct { int new_frame; uint32_t k; uint32_t period; } steps[] = {
        {1, 0, 1},   /* first frame: period unknown, assume 1 */
        {0, 1, 1},   /* late: k >= P holds at alpha 1 */
        {1, 0, 2},   /* second flip after 2 VBlanks: P = 2 */
        {0, 1, 2},
        {1, 0, 2},
        {1, 0, 1},   /* back to 60 Hz */
        {1, 0, 1},
        {0, 1, 1},
        {0, 2, 1},
        {1, 0, 3},   /* 30 -> 20 Hz */
        {0, 1, 3},
        {0, 2, 3},
        {1, 0, 3},
        {0, 1, 3}, {0, 2, 3}, {0, 3, 3}, {0, 4, 3}, {0, 5, 3}, {0, 6, 3},
        {1, 0, FRAME_FLIP_PERIOD_MAX},   /* long hold clamps to the max */
    };
    FrameFlipTracker tracker;
    frame_flip_tracker_reset(&tracker);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        double lo = -1.0, hi = -1.0;
        uint32_t k = frame_flip_tracker_vblank(&tracker, steps[i].new_frame,
                                               &lo, &hi);
        char msg[96];
        snprintf(msg, sizeof msg, "flip tracker step %u: k", (unsigned)i);
        CHECK(k == steps[i].k, msg);
        snprintf(msg, sizeof msg, "flip tracker step %u: period", (unsigned)i);
        CHECK(tracker.period == steps[i].period, msg);
        double elo = (double)k / (double)tracker.period;
        double ehi = (double)(k + 1) / (double)tracker.period;
        if (elo > 1.0) elo = 1.0;
        if (ehi > 1.0) ehi = 1.0;
        snprintf(msg, sizeof msg, "flip tracker step %u: window", (unsigned)i);
        CHECK(fabs(lo - elo) < 1e-9 && fabs(hi - ehi) < 1e-9, msg);
    }
}

static void test_phase_windows(void) {
    FrameInterpolationSchedule schedule = {0};
    uint64_t deadline;
    float alpha, first = -1.0f, last = -1.0f;
    int n = 0;
    /* P = 2, second VBlank of the frame: 240 Hz gives four presents that
     * cover (0.5, 1]. */
    CHECK(frame_interpolation_schedule_begin_phase(
              &schedule, 1000000u, 1000000u, 60.0, 240.0, 0.5, 1.0),
          "phase window should plan");
    while (frame_interpolation_schedule_next(&schedule, 1000000u, &deadline,
                                             &alpha)) {
        if (n == 0) first = alpha;
        last = alpha;
        n++;
    }
    CHECK(n == 4, "240 Hz over one 60 Hz VBlank should present four times");
    CHECK(fabsf(first - 0.625f) < 1e-3f && fabsf(last - 1.0f) < 1e-3f,
          "second-half window should map to 0.625 .. 1");

    /* P = 3 windows are thirds. */
    frame_interpolation_schedule_reset(&schedule);
    CHECK(frame_interpolation_schedule_begin_phase(
              &schedule, 2000000u, 1000000u, 60.0, 120.0, 1.0 / 3.0, 2.0 / 3.0),
          "third window should plan");
    n = 0;
    while (frame_interpolation_schedule_next(&schedule, 2000000u, &deadline,
                                             &alpha)) {
        if (n == 0) first = alpha;
        last = alpha;
        n++;
    }
    CHECK(n == 2 && fabsf(first - 0.5f) < 1e-3f &&
          fabsf(last - 2.0f / 3.0f) < 1e-3f,
          "middle third at 120 Hz should present 1/2 then 2/3");

    /* Degenerate and reversed windows clamp instead of extrapolating. */
    frame_interpolation_schedule_reset(&schedule);
    CHECK(frame_interpolation_schedule_begin_phase(
              &schedule, 3000000u, 1000000u, 60.0, 120.0, 1.5, 0.25),
          "clamped window should still plan");
    while (frame_interpolation_schedule_next(&schedule, 3000000u, &deadline,
                                             &alpha))
        CHECK(alpha == 1.0f, "a window past the frame must hold at alpha 1");

    /* begin() is the full window: identical alphas to begin_phase(0, 1). */
    {
        FrameInterpolationSchedule a = {0}, b = {0};
        float aa, ab;
        uint64_t da, db;
        CHECK(frame_interpolation_schedule_begin(&a, 4000000u, 1000000u,
                                                 60.0, 165.0) &&
              frame_interpolation_schedule_begin_phase(&b, 4000000u, 1000000u,
                                                       60.0, 165.0, 0.0, 1.0),
              "begin and begin_phase(0,1) should both plan");
        for (;;) {
            int ra = frame_interpolation_schedule_next(&a, 4000000u, &da, &aa);
            int rb = frame_interpolation_schedule_next(&b, 4000000u, &db, &ab);
            CHECK(ra == rb, "begin and begin_phase(0,1) present equally often");
            if (!ra || !rb) break;
            CHECK(aa == ab && da == db,
                  "begin must stay bit-identical to begin_phase(0,1)");
        }
    }
}

/* interp_capture's FLIP branch asks this whether a present is a new frame;
 * a duplicate keeps the history and only advances the tracker's phase. */
static void test_flip_new_frame(void) {
    /* The same origin, nothing redrawn, a frame already held: duplicate. */
    CHECK(!frame_flip_is_new_frame(0, 0, 0, 0, 240, 0, 240),
          "a re-presented image is a duplicate");
    CHECK(frame_flip_is_new_frame(0, 0, 0, 0, 0, 0, 240),
          "the display origin moving in y is a flip");
    CHECK(frame_flip_is_new_frame(0, 0, 0, 320, 240, 0, 240),
          "the display origin moving in x is a flip");
    CHECK(frame_flip_is_new_frame(0, 0, 1, 0, 240, 0, 240),
          "a redraw of the displayed rect is a new frame");
    CHECK(frame_flip_is_new_frame(0, 1, 0, 0, 240, 0, 240),
          "the first frame after a history reset is new");
    CHECK(frame_flip_is_new_frame(1, 0, 0, 0, 240, 0, 240),
          "a presented-geometry change is a new frame");
    CHECK(frame_flip_is_new_frame(0, 0, 0, 0, 0, -1, -1),
          "the first present after a reset (origin unknown) is new");
}

static void test_flip_rates(void) {
    static const struct { double hz; int lo; int hi; } rates[] = {
        {60.0, 2, 3}, {100.0, 3, 4}, {120.0, 4, 5}, {200.0, 6, 7},
        {240.0, 8, 9}, {300.0, 10, 11},
    };
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        int mn = 0, mx = 0;
        const int ticks = 60;
        int total = run_flip_ticks(rates[i].hz, 2u, ticks, &mn, &mx);
        double expect = rates[i].hz / (59.94 / 2.0) * ticks;
        char msg[128];
        snprintf(msg, sizeof msg,
                 "%.0f Hz over 30 Hz frames: %d presents (expect ~%.1f), "
                 "%d..%d per frame",
                 rates[i].hz, total, expect, mn, mx);
        CHECK(fabs((double)total - expect) <= 2.0, msg);
        CHECK(mn >= rates[i].lo && mx <= rates[i].hi, msg);
    }
    /* A 60 Hz game (P = 1) keeps today's one-crossfade-per-VBlank shape. */
    {
        int mn = 0, mx = 0;
        int total = run_flip_ticks(120.0, 1u, 60, &mn, &mx);
        CHECK(total == 120 && mn == 2 && mx == 2,
              "120 Hz over a 60 Hz game should present twice per frame");
    }
}

int main(void) {
    test_flip_tracker_table();
    test_flip_new_frame();
    test_phase_windows();
    test_flip_rates();

    FrameInterpolationSchedule schedule = {0};
    float first = 0.0f, last = 0.0f;
    uint64_t end;

    CHECK(run_interval(&schedule, 1000000u, 60.0, 120.0,
                       &first, &last) == 2,
          "120 Hz should schedule two presents per 60 Hz source interval");
    CHECK(fabsf(first - 0.5f) < 0.001f && fabsf(last - 1.0f) < 0.001f,
          "120 Hz blend phases should be one-half and complete");

    frame_interpolation_schedule_reset(&schedule);
    end = 2000000u;
    int total = 0;
    for (int i = 0; i < 5; i++) {
        total += run_interval(&schedule, end, 60.0, 144.0, NULL, NULL);
        end = frame_interpolation_schedule_end(&schedule) + 1000u;
    }
    CHECK(total == 12,
          "144 Hz should retain fractional cadence across five source frames");

    frame_interpolation_schedule_reset(&schedule);
    CHECK(run_interval(&schedule, 3000000u, 60.0, 165.0, NULL, NULL) >= 2,
          "165 Hz should schedule multiple presents");
    end = frame_interpolation_schedule_end(&schedule);
    CHECK(frame_interpolation_schedule_begin(
              &schedule, end + 500u, 1000000u, 60.0, 165.0),
          "normal guest work should keep the existing cadence anchor");
    CHECK(frame_interpolation_schedule_end(&schedule) == end + 16667u ||
          frame_interpolation_schedule_end(&schedule) == end + 16666u,
          "guest work must consume, not extend, the next source interval");

    CHECK(!frame_interpolation_schedule_begin(
               &schedule, 0u, 0u, 60.0, 120.0),
          "zero host frequency should be rejected");
    CHECK(!frame_interpolation_schedule_begin(
               &schedule, 0u, 1000000u, 60.0, 59.0),
          "output below source cadence should be rejected");

    frame_interpolation_schedule_reset(&schedule);
    schedule.queued = 1;
    for (int i = 1; i <= 6; ++i) {
        uint64_t deadline;
        float alpha;
        CHECK(frame_interpolation_schedule_begin(&schedule, 1000000u,
                  1000000u, 60.0, 60.0), "queued interval accepted");
        CHECK(frame_interpolation_schedule_next(&schedule, 1000000u,
                  &deadline, &alpha), "queued interval has one output");
        CHECK(fabs((double)deadline - (1000000.0 + i * 1000000.0 / 60.0)) < 1.0,
              "early producer retains every presentation deadline");
        CHECK(!frame_interpolation_schedule_next(&schedule, 1000000u,
                   &deadline, &alpha), "queued interval has no duplicate output");
    }
    CHECK(frame_interpolation_schedule_begin(&schedule, 2000000u,
              1000000u, 60.0, 60.0), "queued clock recovers after sustained stall");
    CHECK(frame_interpolation_schedule_end(&schedule) == 2016667u,
          "late queued producer reanchors after sustained stall");

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
