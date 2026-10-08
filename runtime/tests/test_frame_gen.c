/* Frame generation's renderer-independent half (src/frame_gen.c,
 * docs/FRAME_GENERATION.md): matching triangles of two frames, the
 * interpolation endpoints, the plan of how many in-between frames fit, and
 * the breaker, the generated-frame cost estimate and the guest pace. */
#include "frame_gen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); }
}

/* ---- a synthetic 3D scene seen by a moving camera ----
 * World points on a grandstand facade (one repeated texture: every triangle
 * has the same key), a camera at eye E with yaw Y (world -> camera: rotate by
 * -Y about the vertical axis, after subtracting E), projection H / z. */
#define GW 16
#define GH 6
#define HH 300.0
typedef struct { double eye[3], yaw; } Cam;

static void to_cam(const Cam *c, const double w[3], double o[3]) {
    const double d[3] = { w[0] - c->eye[0], w[1] - c->eye[1], w[2] - c->eye[2] };
    const double cs = cos(c->yaw), sn = sin(c->yaw);
    o[0] = cs * d[0] - sn * d[2];
    o[1] = d[1];
    o[2] = sn * d[0] + cs * d[2];
}

static void wpt(int r, int col, double w[3]) {
    w[0] = -400.0 + col * 50.0; w[1] = -100.0 + r * 40.0; w[2] = 2000.0 + col * 30.0;
}

static uint32_t vid_of(int r, int col) { return (uint32_t)(r * (GW + 1) + col) + 1; }

static void vtx(FgPrim *p, int k, const Cam *c, int r, int col, uint32_t id) {
    double w[3], q[3];
    wpt(r, col, w);
    to_cam(c, w, q);
    p->vid[k] = id;
    for (int i = 0; i < 3; i++) p->p[k][i] = (float)q[i];
    p->h[k] = (float)HH;
    p->x[k] = (float)(160.0 + HH * q[0] / q[2]);
    p->y[k] = (float)(120.0 + HH * q[1] / q[2]);
}

static void add(FgPrimList *l, FgPrim p) { p.rec = l->n; check(fg_prims_add(l, &p), "add"); }

/* Rows r0..r1-1, bottom first when `reverse`. */
static void stand(FgPrimList *l, const Cam *c, int r0, int r1, int reverse) {
    for (int ri = r0; ri < r1; ri++) {
        const int r = reverse ? r1 - 1 - (ri - r0) : ri;
        for (int col = 0; col < GW; col++) {
            FgPrim p;
            memset(&p, 0, sizeof p);
            p.key = 7; p.view = 1;
            vtx(&p, 0, c, r, col, vid_of(r, col));
            vtx(&p, 1, c, r, col + 1, vid_of(r, col + 1));
            vtx(&p, 2, c, r + 1, col, vid_of(r + 1, col));
            add(l, p);
            vtx(&p, 0, c, r, col + 1, vid_of(r, col + 1));
            vtx(&p, 1, c, r + 1, col + 1, vid_of(r + 1, col + 1));
            vtx(&p, 2, c, r + 1, col, vid_of(r + 1, col));
            add(l, p);
        }
    }
}

/* Worst screen distance between where a newer vertex is placed at phase t
 * and where camera c shows its world point. */
static float place_error(const FgPrimList *b, const float *x, const float *y, const Cam *c) {
    float worst = 0.0f;
    for (uint32_t j = 0; j < b->n; j++)
        for (int k = 0; k < 3; k++) {
            const uint32_t id = b->v[j].vid[k] - 1;
            double w[3], q[3];
            wpt((int)(id / (GW + 1)), (int)(id % (GW + 1)), w);
            to_cam(c, w, q);
            const float ex = x[3 * j + k] - (float)(160.0 + HH * q[0] / q[2]);
            const float ey = y[3 * j + k] - (float)(120.0 + HH * q[1] / q[2]);
            const float e = sqrtf(ex * ex + ey * ey);
            if (e > worst) worst = e;
        }
    return worst;
}

/* The grandstand regression: the camera pans and travels, the newer frame
 * draws the rows in another order with the top row culled and a new row in.
 * In-between frames are the newer frame from an in-between camera: at phase
 * 0 every vertex lands where the older camera saw it, at 0.5 where the
 * halfway camera does. */
static void test_camera_reordered(void) {
    const Cam co = { { 0, 0, 0 }, 0.00 }, cn = { { 30, 0, 60 }, 0.04 }, cm = { { 15, 0, 30 }, 0.02 };
    FgPrimList a = { 0 }, b = { 0 };
    stand(&a, &co, 0, GH - 1, 0);
    stand(&b, &cn, 1, GH, 1);
    FgCamParams cp;
    fg_cam_defaults(&cp);
    FgVert *v = (FgVert *)malloc(sizeof(FgVert) * b.n * 3);
    float *x = (float *)malloc(sizeof(float) * b.n * 3), *y = (float *)malloc(sizeof(float) * b.n * 3);
    FgCamFit fit;
    check(fg_cam_fit(&a, &b, &cp, &fit, v) == 1 && fit.ok, "the reordered stand gets a camera");
    check(fit.camera == b.n * 3 && fit.v[0].inliers == fit.v[0].pairs, "every vertex placed by the camera");
    fg_cam_place(&b, &fit, v, 0.0, x, y, NULL);
    check(place_error(&b, x, y, &co) < 0.1f, "phase 0: where the older camera saw them (new row too)");
    fg_cam_place(&b, &fit, v, 0.5, x, y, NULL);
    check(place_error(&b, x, y, &cm) < 0.5f, "phase 0.5: where the halfway camera sees them");
    fg_cam_place(&b, &fit, v, 1.0, x, y, NULL);
    check(place_error(&b, x, y, &cn) < 1e-3f, "phase 1: the newer frame exactly");
    free(v); free(x); free(y);
    fg_prims_free(&a); fg_prims_free(&b);
}

/* A car the camera follows (the same camera-space position in both frames)
 * stays where it is; a HUD triangle (no projection) is not moved; a vertex
 * without a projection in a world triangle moves with its neighbours. */
static void test_objects_hud_neighbours(void) {
    const Cam co = { { 0, 0, 0 }, 0.0 }, cn = { { 0, 0, 80 }, 0.03 };
    FgPrimList a = { 0 }, b = { 0 };
    stand(&a, &co, 0, GH, 0);
    stand(&b, &cn, 0, GH, 0);
    FgPrim car;
    memset(&car, 0, sizeof car);
    car.key = 9; car.view = 1;
    const float cpos[3][3] = { { -50, 60, 600 }, { 50, 60, 600 }, { 0, 20, 650 } };
    for (int k = 0; k < 3; k++) {
        car.vid[k] = 900000u + (uint32_t)k;
        memcpy(car.p[k], cpos[k], sizeof car.p[k]);
        car.h[k] = (float)HH;
        car.x[k] = (float)(160.0 + HH * cpos[k][0] / cpos[k][2]);
        car.y[k] = (float)(120.0 + HH * cpos[k][1] / cpos[k][2]);
    }
    add(&a, car); add(&b, car);
    FgPrim hud;
    memset(&hud, 0, sizeof hud);
    hud.key = 3; hud.view = 2;
    hud.x[0] = 10; hud.y[0] = 10; hud.x[1] = 40; hud.y[1] = 10; hud.x[2] = 10; hud.y[2] = 30;
    add(&b, hud);
    FgPrim cpu = b.v[0];   /* a CPU-built vertex in a world triangle */
    cpu.vid[2] = 0;
    add(&b, cpu);
    FgCamParams cp;
    fg_cam_defaults(&cp);
    FgVert *v = (FgVert *)malloc(sizeof(FgVert) * b.n * 3);
    float *x = (float *)malloc(sizeof(float) * b.n * 3), *y = (float *)malloc(sizeof(float) * b.n * 3);
    FgCamFit fit;
    check(fg_cam_fit(&a, &b, &cp, &fit, v) && fit.object == 3, "the followed car is an object");
    fg_cam_place(&b, &fit, v, 0.0, x, y, NULL);
    const uint32_t ic = 3 * (b.n - 3), ih = 3 * (b.n - 2), iu = 3 * (b.n - 1);
    check(fabsf(x[ic] - car.x[0]) < 1e-3f && fabsf(y[ic + 2] - car.y[2]) < 1e-3f, "the followed car stays put");
    check(x[ih] == hud.x[0] && y[ih + 2] == hud.y[2], "the HUD is not moved");
    check(v[iu + 2].mode == FG_PLACE_NEIGHBOUR && fabsf(x[iu + 2] - b.v[b.n - 1].x[2]) > 0.5f,
          "a CPU-built vertex moves with its triangle");
    free(v); free(x); free(y);
    fg_prims_free(&a); fg_prims_free(&b);
}

static void test_verdict(void) {
    const Cam co = { { 0, 0, 0 }, 0.0 }, cn = { { 0, 0, 40 }, 0.02 }, far = { { 0, 0, 0 }, 0.9 };
    FgCamParams cp;
    fg_cam_defaults(&cp);
    FgPrimList a = { 0 }, b = { 0 };
    FgVert v[3 * GW * 2 * GH + 64];
    FgCamFit fit;
    /* No projections (a 2D screen). */
    for (int i = 0; i < 40; i++) {
        FgPrim p; memset(&p, 0, sizeof p); p.key = 1; p.x[1] = (float)i; add(&a, p); add(&b, p);
    }
    check(!fg_cam_fit(&a, &b, &cp, &fit, v) && fit.why && strstr(fit.why, "no projections"), "no projections: rejected");
    fg_prims_reset(&a); fg_prims_reset(&b);
    /* A cut: other geometry entirely. */
    stand(&a, &co, 0, GH, 0);
    stand(&b, &cn, 0, GH, 0);
    for (uint32_t j = 0; j < b.n; j++) for (int k = 0; k < 3; k++) b.v[j].vid[k] += 100000u;
    check(!fg_cam_fit(&a, &b, &cp, &fit, v) && fit.why && strstr(fit.why, "few pairs"), "a cut: rejected");
    fg_prims_reset(&b);
    /* A turn no camera makes between two frames. */
    stand(&b, &far, 0, GH, 0);
    check(!fg_cam_fit(&a, &b, &cp, &fit, v), "an implausible turn: rejected");
    fg_prims_reset(&b);
    /* Half the identities moved independently (wrong lookups): no consistent camera. */
    stand(&b, &cn, 0, GH, 0);
    for (uint32_t j = 0; j < b.n; j += 2)
        for (int k = 0; k < 3; k++) { b.v[j].p[k][0] += 300.0f * (float)((j * 7 + k) % 5); b.v[j].p[k][2] += 150.0f; }
    check(!fg_cam_fit(&a, &b, &cp, &fit, v), "inconsistent motion: rejected");
    fg_prims_free(&a); fg_prims_free(&b);
}

static void test_plan(void) {
    const double f30 = 2.0 / 59.94, f60 = 1.0 / 59.94;
    /* 30 Hz game frames on a 120 Hz panel: 4 slots, 3 in-between. */
    check(fg_plan(f30, 120.0, 0.010, 0.002, 0.85, 7) == 3, "30 Hz on 120 Hz: three in-between frames");
    /* 60 Hz game frames on 120 Hz: one. On 60 Hz: none. */
    check(fg_plan(f60, 120.0, 0.004, 0.002, 0.85, 7) == 1, "60 Hz on 120 Hz: one");
    check(fg_plan(f60, 60.0, 0.004, 0.002, 0.85, 7) == 0, "60 Hz on 60 Hz: none");
    check(fg_plan(f30, 60.0, 0.010, 0.002, 0.85, 7) == 1, "30 Hz on 60 Hz: one");
    /* Only what fits: 33.4 ms * 0.85 = 28.4; real 20 ms leaves 8.4 -> two of 4 ms. */
    check(fg_plan(f30, 120.0, 0.020, 0.004, 0.85, 7) == 2, "only what fits");
    check(fg_plan(f30, 120.0, 0.030, 0.001, 0.85, 7) == 0, "no surplus: none");
    /* Unknown generation cost: one, if half the budget is free. */
    check(fg_plan(f30, 120.0, 0.010, 0.0, 0.85, 7) == 1, "unmeasured: one to measure");
    check(fg_plan(f30, 120.0, 0.020, 0.0, 0.85, 7) == 0, "unmeasured without room: none");
    check(fg_plan(f30, 240.0, 0.001, 0.0001, 0.85, 2) == 2, "max_gens caps");
}

static void test_breaker(void) {
    FgBreaker b;
    fg_breaker_init(&b, 3.0, 24.0, 10.0);
    check(fg_breaker_open(&b, 0.0), "starts allowing generation");
    fg_breaker_trip(&b, 1.0, "late");
    check(!fg_breaker_open(&b, 3.9) && fg_breaker_open(&b, 4.0), "a trip holds 3 s");
    fg_breaker_trip(&b, 5.0, "late");   /* within 10 s of the last hold's end */
    check(!fg_breaker_open(&b, 10.9) && fg_breaker_open(&b, 11.0), "a repeat doubles the hold");
    fg_breaker_trip(&b, 6.0, "late");   /* already held: the hold stands */
    check(fg_breaker_open(&b, 11.0), "a trip while held does not extend it");
    fg_breaker_trip(&b, 40.0, "late");  /* long after: back to the base hold */
    check(!fg_breaker_open(&b, 42.9) && fg_breaker_open(&b, 43.0), "a later trip starts over");
    check(b.trips == 4 && b.reason && strcmp(b.reason, "late") == 0, "trips counted with a reason");
    for (int i = 0; i < 10; i++) fg_breaker_trip(&b, 43.0 + 30.0 * i + (i ? 0 : 0), "x");
    check(b.hold <= 24.0, "the hold is capped");
}

static void test_cost(void) {
    FgCost c;
    fg_cost_init(&c, 2.0, 16.0);
    const double fit = 0.020;
    check(fg_cost_estimate(&c, 0.0, fit) == 0.0, "unknown before any sample");
    /* Cold: the first frames after allocation are discarded. */
    fg_cost_cold(&c, 2);
    fg_cost_add(&c, 0.070, fit);
    fg_cost_add(&c, 0.040, fit);
    check(c.ema == 0.0 && c.discarded == 2, "cold samples discarded");
    fg_cost_add(&c, 0.006, fit);
    check(fabs(fg_cost_estimate(&c, 0.1, fit) - 0.006) < 1e-12, "first warm sample is the estimate");
    fg_cost_add(&c, 0.016, fit);
    check(fabs(c.ema - 0.008) < 1e-12, "warm samples blend");
    /* A spike that pushes the estimate past the fit blocks the plan... */
    for (int i = 0; i < 10; i++) fg_cost_add(&c, 0.060, fit);
    check(fg_cost_estimate(&c, 10.0, fit) > fit, "a high estimate blocks");
    check(fg_cost_estimate(&c, 11.9, fit) > fit, "... until the probe interval");
    /* ...for at most probe_s: then one probe, whose sample replaces it. */
    check(fg_cost_estimate(&c, 12.0, fit) == 0.0 && c.probes == 1, "stale estimate re-probed");
    check(fg_cost_estimate(&c, 12.03, fit) > fit, "one probe at a time");
    fg_cost_add(&c, 0.005, fit);
    check(fabs(fg_cost_estimate(&c, 12.1, fit) - 0.005) < 1e-12, "the probe replaces the estimate");
    /* A probe that still does not fit backs off. */
    for (int i = 0; i < 10; i++) fg_cost_add(&c, 0.060, fit);
    (void)fg_cost_estimate(&c, 20.0, fit);
    check(fg_cost_estimate(&c, 22.0, fit) == 0.0, "second probe");
    fg_cost_add(&c, 0.050, fit);
    check(c.probe_s == 4.0, "a probe that does not fit doubles the wait");
    check(fg_cost_estimate(&c, 25.9, fit) > fit && fg_cost_estimate(&c, 26.0, fit) == 0.0,
          "next probe after the doubled wait");
    fg_cost_add(&c, 0.004, fit);
    check(c.probe_s == 2.0, "a probe that fits resets the wait");
    /* A granted probe that was never drawn is given up, then re-granted. */
    for (int i = 0; i < 10; i++) fg_cost_add(&c, 0.060, fit);
    (void)fg_cost_estimate(&c, 40.0, fit);
    check(fg_cost_estimate(&c, 42.0, fit) == 0.0, "probe granted");
    check(fg_cost_estimate(&c, 44.0, fit) > fit, "an undrawn probe expires");
    check(fg_cost_estimate(&c, 46.0, fit) == 0.0, "and is granted again");
    /* Re-allocation makes the next samples cold again. */
    fg_cost_cold(&c, 2);
    fg_cost_add(&c, 0.5, fit); fg_cost_add(&c, 0.5, fit);
    check(c.cold == 0 && c.ema < 0.5, "cold after re-allocation");
}

static void test_pace(void) {
    FgPace p;
    memset(&p, 0, sizeof p);
    const double T = 1.0 / 59.94, slack = 2.0 * T;
    double t = 0.0;
    int late = 0;
    for (int i = 0; i < 600; i++) {   /* jitter: +-6 ms around the schedule */
        late += fg_pace_note(&p, t + ((i % 3) - 1) * 0.006, T, slack);
        t += T;
    }
    check(late == 0, "jitter is not late");
    late = 0;   /* one 40 ms frame made up by a burst */
    late += fg_pace_note(&p, t + 0.024, T, slack); t += T;
    late += fg_pace_note(&p, t + 0.004, T, slack); t += T;
    late += fg_pace_note(&p, t, T, slack); t += T;
    check(late == 0, "a long frame made up is not late");
    /* A real slip: 60 ms gap. */
    check(fg_pace_note(&p, t + 0.045, T, slack) == 1, "a slip beyond two intervals is late");
    t += 0.045 + T;
    check(fg_pace_note(&p, t, T, slack) == 0, "the schedule restarts after a slip");
    /* Slow but steady (a 50 Hz guest told 59.94): one trip, then resync each time it slips. */
    int trips = 0;
    for (int i = 0; i < 60; i++) { t += 0.020; trips += fg_pace_note(&p, t, T, slack); }
    check(trips > 0 && trips < 20, "a steadily slow guest is late now and then");
}

static void test_ceiling(void) {
    FgCeiling c;
    fg_ceiling_init(&c, 7, 2.0);
    check(fg_ceiling_get(&c, 0.0) == 7, "no ceiling before a trip");
    fg_ceiling_trip(&c, 3, 1.0);
    check(fg_ceiling_get(&c, 1.5) == 2, "an overload at three plans two");
    fg_ceiling_trip(&c, 2, 1.6);
    check(fg_ceiling_get(&c, 1.7) == 1, "another lowers it again");
    fg_ceiling_trip(&c, 1, 1.8);
    check(fg_ceiling_get(&c, 1.9) == 1, "never below one (the breaker stops generation)");
    check(fg_ceiling_get(&c, 3.7) == 1 && fg_ceiling_get(&c, 3.9) == 2, "recovers one per 2 s");
    check(fg_ceiling_get(&c, 7.9) == 4, "and keeps recovering");
    fg_ceiling_trip(&c, 0, 8.0);
    check(fg_ceiling_get(&c, 8.0) == 4, "a trip with nothing planned is not an overload");
    check(fg_ceiling_get(&c, 100.0) == 7, "up to the maximum");
}

int main(void) {
    test_ceiling();
    test_cost();
    test_pace();
    test_camera_reordered();
    test_objects_hud_neighbours();
    test_verdict();
    test_plan();
    test_breaker();
    printf("frame_gen_test: checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
