/* Render-pass planning and presentation-selection math.
 * Build/run: ctest -R render_pass_plan_test */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "render_pass.h"
#include "render_pass_plan.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

/* One 30 Hz game frame (2 VBlanks at 59.94 Hz) in nanosecond ticks. */
static const double kFrame = 2.0 * 1e9 / 59.94;

static void test_counts_per_rate(void) {
    static const struct { double hz; uint32_t lo, hi; } rates[] = {
        {60.0, 1, 2}, {100.0, 3, 4}, {120.0, 3, 4}, {200.0, 6, 7},
        {240.0, 7, 8}, {300.0, 9, 10},
    };
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        double period = 1e9 / rates[r].hz;
        /* Slide the output grid across one output period. */
        for (int k = 0; k < 16; k++) {
            RenderPassPlanInput in = {0};
            uint32_t a[RENDER_PASS_MAX_PHASES], wanted = 0, n;
            char msg[160];
            in.frame_start = 1e12;
            in.frame_length = kFrame;
            in.target_period = period;
            in.next_deadline = in.frame_start - 3.0 * period +
                               period * (double)k / 16.0;
            in.budget = -1.0;
            in.max = RENDER_PASS_MAX_PHASES;
            n = render_pass_plan_phases(&in, a, &wanted);
            snprintf(msg, sizeof msg, "%.0f Hz offset %d/16: %u passes",
                     rates[r].hz, k, (unsigned)n);
            CHECK(n >= rates[r].lo && n <= rates[r].hi, msg);
            CHECK(wanted == n, "unlimited budget sheds nothing");
            for (uint32_t i = 0; i < n; i++) {
                CHECK(a[i] > 0 && a[i] < 65536u, "phase inside (0, 1)");
                if (i) CHECK(a[i] > a[i - 1], "phases ascend");
                /* Each phase is an actual output deadline. */
                double d = in.frame_start + (double)a[i] / 65536.0 * kFrame;
                double m = fmod(d - in.next_deadline, period);
                if (m > period / 2) m -= period;
                snprintf(msg, sizeof msg,
                         "%.0f Hz phase %u lands on a deadline (off %.0f ns)",
                         rates[r].hz, (unsigned)i, m);
                CHECK(fabs(m) < kFrame / 65536.0 + 1.0, msg);
            }
        }
    }
}

static void test_shedding(void) {
    RenderPassPlanInput in = {0};
    uint32_t a[RENDER_PASS_MAX_PHASES], wanted = 0, n;
    in.frame_start = 1e12;
    in.frame_length = kFrame;
    in.target_period = 1e9 / 300.0;
    in.next_deadline = in.frame_start + 1e9 / 600.0;   /* half-period offset */
    in.max = RENDER_PASS_MAX_PHASES;
    in.pass_cost = 2e6;                                 /* 2 ms per pass */
    in.budget = 5e6;                                    /* 5 ms */
    n = render_pass_plan_phases(&in, a, &wanted);
    CHECK(wanted == 10, "300 Hz half-offset wants ten phases");
    CHECK(n == 2, "a 5 ms budget at 2 ms per pass affords two");
    CHECK(n == 2 && a[0] < 32768u && a[1] > 32768u,
          "a shed subset spreads across the frame");

    in.budget = 1e6;
    CHECK(render_pass_plan_phases(&in, a, &wanted) == 0 && wanted == 10,
          "less than one pass of budget renders none");

    /* No pass measured yet (first plans, or a new image size): passes run on
     * the emulation thread, so one measures the cost; a whole plan of
     * unknown cost could stall the guest for frames (46.9 ms each at 4K). */
    in.pass_cost = 0.0;
    in.budget = 26e6;
    n = render_pass_plan_phases(&in, a, &wanted);
    CHECK(n == 1 && wanted == 10, "unknown cost plans one pass");
    CHECK(n == 1 && a[0] > 16384u && a[0] < 49152u,
          "the one pass sits mid-frame, as a shed subset of one does");
    in.budget = 0.0;
    CHECK(render_pass_plan_phases(&in, a, NULL) == 0,
          "no budget: not even the measuring pass");
    in.pass_cost = 2e6;

    in.budget = -1.0;
    in.max = 4;
    CHECK(render_pass_plan_phases(&in, a, NULL) == 4,
          "capacity caps the plan");

    in.max = RENDER_PASS_MAX_PHASES;
    in.target_period = 0.0;
    CHECK(render_pass_plan_phases(&in, a, &wanted) == 0 && wanted == 0,
          "no live output schedule plans nothing");
}

static void test_select(void) {
    const uint32_t ph[] = {0u, 16384u, 32768u, 49152u};
    uint32_t lo = 99, hi = 99;
    float t = -1.0f;
    CHECK(render_pass_select(ph, 4, 0.0, &lo, &hi, &t) && lo == 0 && hi == 0,
          "phase 0 shows the game's image");
    CHECK(render_pass_select(ph, 4, 0.25, &lo, &hi, &t) && lo == 1 && hi == 1,
          "an exact pass phase shows that pass alone");
    CHECK(render_pass_select(ph, 4, 0.375, &lo, &hi, &t) && lo == 1 && hi == 2 &&
          fabsf(t - 0.5f) < 1e-4f,
          "between passes the neighbours blend");
    CHECK(render_pass_select(ph, 4, 0.9, &lo, &hi, &t) && lo == 3 && hi == 3,
          "past the last pass it holds (the next frame is not known yet)");
    CHECK(render_pass_select(ph, 1, 0.6, &lo, &hi, &t) && lo == 0 && hi == 0,
          "with only the game image it holds");
    CHECK(!render_pass_select(ph, 0, 0.5, &lo, &hi, &t), "no items, no pick");
    CHECK(render_pass_select(ph, 4, -0.1, &lo, &hi, &t) && lo == 0 && hi == 0,
          "before the frame starts show its first image");
}

/* A frame on screen longer than planned (a 30 Hz tick that takes three
 * VBlanks) keeps its newest image until the next flip; it must never go back
 * to the game's own image (phase 0), which is older. */
static void test_gen_select_late(void) {
    const uint32_t ph[] = {0u, 16384u, 32768u, 49152u};
    const double late[] = {1.0, 1.25, 1.2501, 1.3, 1.5, 2.0, 3.99,
                           RENDER_PASS_GEN_HOLD_MAX};
    uint32_t lo = 99, hi = 99;
    float t = -1.0f;
    CHECK(render_pass_gen_select(ph, 4, 0.375, &lo, &hi, &t) && lo == 1 &&
          hi == 2 && fabsf(t - 0.5f) < 1e-4f,
          "inside the frame it selects as render_pass_select");
    for (unsigned i = 0; i < sizeof late / sizeof late[0]; i++) {
        char msg[96];
        lo = hi = 99;
        snprintf(msg, sizeof msg, "late flip (p = %.4f) holds the newest image",
                 late[i]);
        CHECK(render_pass_gen_select(ph, 4, late[i], &lo, &hi, &t) &&
              lo == 3 && hi == 3 && t == 0.0f, msg);
    }
    CHECK(!render_pass_gen_select(ph, 4, RENDER_PASS_GEN_HOLD_MAX + 0.01,
                                  &lo, &hi, &t),
          "a game that stops flipping expires the frame after the hold");
    CHECK(!render_pass_gen_select(ph, 4, NAN, &lo, &hi, &t),
          "a phase that is not a number expires it");
    CHECK(!render_pass_gen_select(ph, 0, 0.5, &lo, &hi, &t), "no items, no pick");
}

/* Promotion: a generation for the next flip waits for its own rect; one for
 * a frame already on screen waits for the flip after it, to another rect. */
static void test_gen_flip_matches(void) {
    /* PsyQ VSync-then-PutDispEnv: built for the rect the flip shows. */
    CHECK(render_pass_gen_flip_matches(0, 0, 240, 1, 320, 240, 0, 240, 1, 320, 240),
          "pending: the flip to the generation's rect promotes it");
    CHECK(!render_pass_gen_flip_matches(0, 0, 240, 1, 320, 240, 0, 0, 1, 320, 240),
          "pending: a flip to the other buffer does not");
    /* Flip-when-drawn (V8:2): built for the rect already on screen. */
    CHECK(render_pass_gen_flip_matches(1, 0, 240, 1, 320, 240, 0, 0, 1, 320, 240),
          "shown: the next flip, to the other buffer, promotes it");
    CHECK(!render_pass_gen_flip_matches(1, 0, 240, 1, 320, 240, 0, 240, 1, 320, 240),
          "shown: a redraw of the rect on screen is not the next flip");
    /* Geometry the images were captured at must still be presented. */
    CHECK(!render_pass_gen_flip_matches(1, 0, 240, 1, 320, 240, 0, 0, 0, 320, 240),
          "a different presented source never promotes");
    CHECK(!render_pass_gen_flip_matches(0, 0, 0, 1, 320, 240, 0, 0, 1, 640, 480),
          "a different presented size never promotes");
}

static void test_budget_and_ema(void) {
    CHECK(fabs(render_pass_budget(0, 0, 100.0, 0.5) - 50.0) < 1e-9,
          "no history spends the share of the frame");
    CHECK(fabs(render_pass_budget(20.0, 10.0, 100.0, 0.8) - 24.0) < 1e-9,
          "idle plus pass time, scaled");
    CHECK(render_pass_budget(500.0, 0.0, 100.0, 0.8) == 100.0,
          "never more than a frame");
    CHECK(render_pass_budget(10.0, 10.0, 0.0, 0.8) == 0.0, "no frame, no budget");
    CHECK(render_pass_ema(0.0, 4.0) == 4.0, "first sample seeds the average");
    CHECK(fabs(render_pass_ema(4.0, 8.0) - 5.0) < 1e-9, "quarter-weight update");
    CHECK(render_pass_ema(4.0, -1.0) == 4.0, "bad samples are ignored");
    {
        /* First-use allocations (70 ms at 4K) must not set the average a
         * shed-for-time plan then never revisits; steady passes (10 ms) do. */
        RenderPassCost c;
        memset(&c, 0, sizeof c);
        render_pass_cost_add(&c, 70.0, 1);
        CHECK(render_pass_cost_estimate(&c) == 0.0 && c.skips == 1 && c.kept == 0,
              "an allocating pass is not a sample");
        render_pass_cost_add(&c, 10.0, 0);
        render_pass_cost_add(&c, 10.0, 0);
        CHECK(render_pass_cost_estimate(&c) == 0.0 && c.skips == 0,
              "two samples: still warming up (unknown: one pass per plan)");
        render_pass_cost_add(&c, 10.0, 0);
        CHECK(render_pass_cost_estimate(&c) == 10.0, "three samples seed the average");
        render_pass_cost_add(&c, 70.0, 1);
        CHECK(render_pass_cost_estimate(&c) == 10.0,
              "a later allocating pass leaves it alone");
        for (unsigned i = 1; i < RENDER_PASS_ALLOC_SKIPS; i++)
            render_pass_cost_add(&c, 70.0, 1);
        CHECK(render_pass_cost_estimate(&c) == 10.0 &&
              c.skips == RENDER_PASS_ALLOC_SKIPS,
              "up to RENDER_PASS_ALLOC_SKIPS in a row");
        render_pass_cost_add(&c, 70.0, 1);
        CHECK(fabs(render_pass_cost_estimate(&c) - 25.0) < 1e-9 && c.skips == 0,
              "then an allocating pass counts, so the average cannot freeze");
        render_pass_cost_add(&c, -1.0, 0);
        render_pass_cost_add(&c, NAN, 0);
        CHECK(fabs(render_pass_cost_estimate(&c) - 25.0) < 1e-9,
              "bad samples are ignored");
    }
    {
        /* One slow first pass (a busy host: 24 ms against a steady 3 ms)
         * must not price passes out: the median of the warm-up wins. */
        RenderPassCost c;
        memset(&c, 0, sizeof c);
        render_pass_cost_add(&c, 24.0, 0);
        render_pass_cost_add(&c, 3.0, 0);
        render_pass_cost_add(&c, 3.5, 0);
        CHECK(render_pass_cost_estimate(&c) == 3.5,
              "warm-up median ignores one outlier");
        memset(&c, 0, sizeof c);
        render_pass_cost_add(&c, 47.0, 0);
        render_pass_cost_add(&c, 46.0, 0);
        render_pass_cost_add(&c, 48.0, 0);
        CHECK(render_pass_cost_estimate(&c) == 47.0,
              "a truly expensive size (4K) still prices itself out");
    }
    {
        /* The first passes of a race can all run in a transient (14.8 ms
         * against a steady 6.9 ms in a verify run). Priced out, no pass runs
         * to correct it: an estimate no pass was measured against for
         * RENDER_PASS_REWARM_MIN plans is measured again. */
        RenderPassCost c;
        unsigned i, rewarms = 0;
        memset(&c, 0, sizeof c);
        CHECK(!render_pass_cost_note_plan(&c), "unknown cost: nothing to re-measure");
        for (i = 0; i < RENDER_PASS_COST_WARMUP; i++) render_pass_cost_add(&c, 14.8, 0);
        CHECK(render_pass_cost_estimate(&c) == 14.8, "the transient sets the estimate");
        for (i = 1; i < RENDER_PASS_REWARM_MIN; i++)
            rewarms += (unsigned)render_pass_cost_note_plan(&c);
        CHECK(rewarms == 0 && render_pass_cost_estimate(&c) == 14.8,
              "29 plans without a pass: still trusted");
        CHECK(render_pass_cost_note_plan(&c) && render_pass_cost_estimate(&c) == 0.0,
              "the 30th: unknown again, so plans ask for one pass");
        CHECK(!render_pass_cost_note_plan(&c), "no second restart while warming up");
        for (i = 0; i < RENDER_PASS_COST_WARMUP; i++) render_pass_cost_add(&c, 6.9, 0);
        CHECK(render_pass_cost_estimate(&c) == 6.9, "the steady cost replaces it");
        CHECK(c.rewarm_after == 0, "a stale estimate found: the wait stays at the minimum");
        /* Passes run on it: each measured pass restarts the count. */
        for (i = 0; i < 10u * RENDER_PASS_REWARM_MIN; i++) {
            rewarms += (unsigned)render_pass_cost_note_plan(&c);
            render_pass_cost_add(&c, 6.9, 0);
        }
        CHECK(rewarms == 0, "an estimate passes run against is never restarted");
        /* Priced out again (a busier host): re-measured after the minimum. */
        for (i = 0; i < RENDER_PASS_REWARM_MIN; i++)
            rewarms += (unsigned)render_pass_cost_note_plan(&c);
        CHECK(rewarms == 1, "priced out again: re-measured after the minimum wait");
    }
    {
        /* A size that is truly too expensive (47 ms passes), or a machine at
         * its limit (10 ms re-measured as 9): each re-measure confirms the
         * estimate, so the waits double to RENDER_PASS_REWARM_MAX and the
         * one-pass warm-ups become rare. */
        static const double cases[][2] = {{47.0, 47.0}, {10.0, 9.0}};
        for (unsigned k = 0; k < 2; k++) {
            RenderPassCost c;
            unsigned i, plans = 0, waits[8] = {0}, w = 0, first = 1;
            memset(&c, 0, sizeof c);
            while (w < 8u && plans < 4000u) {
                if (render_pass_cost_estimate(&c) == 0.0) {
                    /* the warm-up's passes */
                    render_pass_cost_add(&c, first ? cases[k][0] : cases[k][1], 0);
                } else {
                    first = 0;
                    plans++;
                    if (render_pass_cost_note_plan(&c)) {
                        waits[w++] = plans;
                        plans = 0;
                    }
                }
            }
            CHECK(w == 8u && waits[0] == RENDER_PASS_REWARM_MIN &&
                  waits[1] == 2u * RENDER_PASS_REWARM_MIN &&
                  waits[2] == 4u * RENDER_PASS_REWARM_MIN,
                  "a confirmed estimate: the waits double");
            for (i = 5; i < 8; i++)
                CHECK(waits[i] == RENDER_PASS_REWARM_MAX, "and stop at the maximum");
        }
    }
}

static void test_store_policy(void) {
    CHECK(render_pass_mmio_class(0x1F801810u, 0x28000000u, 4) == -1, "GP0 reaches the GPU");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x04000002u, 4) == -1, "GP1 DMA mode allowed");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x10000007u, 4) == -1, "GP1 info query allowed");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x05000000u, 4) == RENDER_PASS_DROP_GPU,
          "GP1 display start (a flip) is dropped");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x00000000u, 4) == RENDER_PASS_DROP_GPU,
          "GP1 reset is dropped");
    CHECK(render_pass_mmio_class(0x1F8010A8u, 0x01000401u, 4) == -1, "GPU DMA CHCR allowed");
    CHECK(render_pass_mmio_class(0x1F8010E8u, 0x11000002u, 4) == -1, "OTC DMA allowed");
    CHECK(render_pass_mmio_class(0x1F8010F4u, 0, 4) == -1, "DICR allowed (restored)");
    CHECK(render_pass_mmio_class(0x1F8010C8u, 0x01000201u, 4) == RENDER_PASS_DROP_DMA,
          "SPU DMA is dropped");
    CHECK(render_pass_mmio_class(0x1F801074u, 0, 4) == -1, "I_MASK allowed (restored)");
    CHECK(render_pass_mmio_class(0x1F801D88u, 0x00FFu, 2) == RENDER_PASS_DROP_SPU,
          "SPU key-on is dropped (cannot be undone)");
    CHECK(render_pass_mmio_class(0x1F801C00u, 0, 2) == RENDER_PASS_DROP_SPU, "SPU voice regs dropped");
    CHECK(render_pass_mmio_class(0x1F801801u, 0x1Bu, 1) == RENDER_PASS_DROP_CD, "CD command dropped");
    CHECK(render_pass_mmio_class(0x1F801104u, 0, 4) == RENDER_PASS_DROP_TIMER, "timer mode dropped");
    CHECK(render_pass_mmio_class(0x1F801040u, 0, 1) == RENDER_PASS_DROP_OTHER, "SIO dropped");
    CHECK(render_pass_mmio_class(0x1F801820u, 0, 4) == RENDER_PASS_DROP_OTHER, "MDEC dropped");
}

static void test_stereo_pair_fresh(void) {
    const uint32_t vb = 564480u;
    const uint64_t c = 100000000ull;
    CHECK(render_pass_stereo_pair_fresh(c, c, vb), "pair fresh on its own cycle");
    CHECK(render_pass_stereo_pair_fresh(c, c + 8ull * vb, vb), "pair fresh across the slowest cadence");
    CHECK(render_pass_stereo_pair_fresh(c, c + 24ull * vb, vb), "pair fresh at the age limit");
    CHECK(!render_pass_stereo_pair_fresh(c, c + 24ull * vb + 1, vb),
          "pair stale once the plugin stops submitting");
    CHECK(!render_pass_stereo_pair_fresh(c, c - 1, vb), "clock behind pair (state load) is stale");
}

int main(void) {
    {
        const uint32_t phases[] = {0, 16384, 32768, 65536};
        uint32_t lo, hi; float blend;
        CHECK(render_pass_gen_select_mode(phases, 4, 0.375, 1, &lo, &hi, &blend) &&
              lo == 1 && hi == 1 && blend == 0.0f, "HOLD keeps a native sample between phases");
        CHECK(render_pass_gen_select_mode(phases, 4, 0.5, 1, &lo, &hi, &blend) &&
              lo == 2 && hi == 2 && blend == 0.0f, "HOLD advances at the next native phase");
        CHECK(render_pass_gen_select_mode(phases, 4, 0.375, 0, &lo, &hi, &blend) &&
              lo == 1 && hi == 2 && blend == 0.5f, "explicit blending remains supported");
        CHECK(render_pass_gen_select_mode(phases, 4, 2.0, 1, &lo, &hi, &blend) &&
              lo == 3 && hi == 3, "late HOLD uses newest native image");
        CHECK(!render_pass_gen_select_mode(phases, 4, 5.0, 1, &lo, &hi, &blend),
              "HOLD cannot revive an expired generation");
    }
    test_stereo_pair_fresh();
    test_store_policy();
    test_counts_per_rate();
    test_shedding();
    test_select();
    test_gen_select_late();
    test_gen_flip_matches();
    test_budget_and_ema();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
