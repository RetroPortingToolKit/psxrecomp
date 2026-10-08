#ifndef PSX_FRAME_GEN_H
#define PSX_FRAME_GEN_H
/*
 * Frame generation from recorded draw lists ([video] frame_generation,
 * docs/FRAME_GENERATION.md). The renderer-independent half: the primitives
 * of two consecutive game frames (one per display flip), their matching,
 * vertex interpolation, how many in-between frames fit, and the breaker.
 * The GL backend (gpu_gl_renderer.c) owns the record lists themselves,
 * drawing and presenting; nothing here touches GL or guest state.
 *
 * A primitive is a triangle of one frame's draw list with a key (the draw
 * op, texture page, CLUT, texture coordinates, the draw area it was clipped
 * to), a view (its draw area: split-screen views have their own cameras), its
 * three screen positions in native pixels relative to that frame's displayed
 * buffer and, per vertex, the GTE projection that produced it when one did
 * (gte_fg_source_lookup): an identity (the projecting function and the
 * model-space vertex), the camera-space position the GTE divided and the
 * projection distance H.
 *
 * In-between frames are the NEWER frame redrawn from an in-between camera;
 * no primitive of the older frame is drawn and none is paired by draw order.
 * Camera motion: per view, vertices of both frames with the same identity
 * are paired in camera space and a rigid motion old -> new is fitted to them
 * (RANSAC, then least squares on the inliers: the static world agrees, moving
 * objects are outliers). A vertex is placed at phase t by
 *   - camera: the fitted motion's fraction t applied to the older camera,
 *     i.e. P_t = D^t D^-1 P_new (the static world, and anything new);
 *   - object: a paired vertex that moved against the world (a car, the
 *     player's own car under a chase camera) lerps its camera-space position;
 *   - neighbours: a vertex without a projection (CPU-built), or one new to
 *     a moving object, in a triangle with placed vertices moves by their
 *     mean screen motion, per position;
 *   - unchanged: everything else (HUD, 2D, sprites).
 * Each is re-projected (H * x / z) and moved by the difference from its own
 * newer projection, so draw offsets and sub-pixel positions stay the game's.
 * What the in-between camera uncovers shows the newer frame: the caller
 * starts from its image and skips the clears before the first draw, and the
 * strips along a view's edges the picture moved away from (fg_cam_place's
 * margins) are the newer frame's.
 * The fit's verdict rejects the whole generated frame (the real frame shows)
 * when a view with projections has too few pairs, too few inliers, or a
 * motion no camera makes between two frames.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgPrim {
    uint32_t rec;        /* index of its record in the owner's list */
    uint32_t key;        /* fg_hash of what it draws */
    uint32_t view;       /* fg_hash of its draw area */
    float    area[4];    /* that draw area (x1, y1, x2, y2), relative like x/y */
    uint32_t vid[3];     /* projection identities, 0 = no projection */
    float    x[3], y[3]; /* native px, relative to the list's display origin */
    float    p[3][3];    /* camera-space positions (GTE units) */
    float    h[3];       /* projection distance */
} FgPrim;

typedef struct FgPrimList {
    FgPrim  *v;
    uint32_t n, cap;
} FgPrimList;

void fg_prims_reset(FgPrimList *l);
void fg_prims_free(FgPrimList *l);
/* Append; returns 0 when out of memory. */
int  fg_prims_add(FgPrimList *l, const FgPrim *p);

/* FNV-1a over 32-bit words, for building keys. */
uint32_t fg_hash(uint32_t h, const int32_t *w, int n);
#define FG_HASH_INIT 2166136261u

typedef struct FgCamParams {
    int      iters;          /* RANSAC samples per view */
    float    tol_px;         /* inlier: |D a - b| within this many screen px at b's depth */
    float    min_inliers;    /* share of pairs the camera must explain */
    uint32_t min_pairs;      /* pairs a view with projections needs */
    float    max_angle;      /* radians the camera may turn between frames */
    float    max_shift;      /* camera travel, as a share of the median depth */
    float    max_obj;        /* object pairing: |b - a| < max_obj * |b.z| */
} FgCamParams;
void fg_cam_defaults(FgCamParams *p);

#define FG_MAX_VIEWS 4
typedef struct FgView {
    uint32_t view, sources, pairs, inliers;
    int      ok;
    float    area[4];        /* the view's draw area */
    double   q[4], t[3];     /* old -> new: rotation quaternion (w,x,y,z), translation */
    const char *why;
} FgView;

typedef struct FgCamFit {
    int      ok;
    const char *why;
    uint32_t nviews, prims, camera, object, neighbour, unchanged;
    uint32_t clamped;        /* fg_cam_place: vertices the in-between camera passes (frame rejected) */
    uint32_t guessed;        /* fg_cam_place: vertices without a projection placed by a triangle
                              * mean over a motion that varies by more than 1 px */
    FgView   v[FG_MAX_VIEWS];
} FgCamFit;

/* How each newer vertex is placed (fg_cam_fit fills it, newer->n * 3). */
typedef struct FgVert {
    uint8_t  mode;           /* FG_PLACE_* */
    int8_t   view;           /* index into FgCamFit.v, -1 */
    uint8_t  paired;         /* the older frame has this vertex */
    float    a[3];           /* FG_PLACE_OBJECT: the older camera-space position */
} FgVert;
enum { FG_PLACE_UNCHANGED = 0, FG_PLACE_CAMERA, FG_PLACE_OBJECT, FG_PLACE_NEIGHBOUR };

/* Fits the camera motion older -> newer per view and decides each newer
 * vertex's placement; returns fit->ok (the verdict). */
int fg_cam_fit(const FgPrimList *older, const FgPrimList *newer, const FgCamParams *p,
               FgCamFit *fit, FgVert *verts);

/* Screen positions of every newer vertex at phase t (0 = the older frame's
 * camera, 1 = the newer frame exactly): x/y[newer->n * 3]; a vertex the
 * in-between camera is passing is projected at a clamped depth. margin (may be
 * NULL) receives, per view, how far the picture moved in from each edge of
 * its draw area (left, top, right, bottom, px): the newer frame drew nothing
 * beyond its edges, so that strip has no in-between picture and the caller
 * shows the newer frame there. */
void fg_cam_place(const FgPrimList *newer, FgCamFit *fit, const FgVert *verts,
                  double t, float *x, float *y, float margin[][4]);

/* How many in-between frames to draw per game frame.
 *   flip_s       the game frame's interval (time between flips)
 *   refresh_hz   the display's refresh rate
 *   real_cost_s  the render thread's cost of one game frame (all its VBlank
 *                frames), 0 when unknown
 *   gen_cost_s   the cost of one generated frame, 0 when not yet measured
 *   budget       share of the interval the real and generated work may use
 * Returns 0..slots-1, where slots = round(flip_s * refresh_hz); an unknown
 * generation cost allows one frame while the real cost leaves half the
 * interval, so the cost gets measured. */
int fg_plan(double flip_s, double refresh_hz, double real_cost_s,
            double gen_cost_s, double budget, int max_gens);

/* Breaker: after a trip, generation stays off for hold seconds; a trip
 * within `repeat_s` of the end of the last hold doubles the hold, up to max. */
typedef struct FgBreaker {
    double until, hold, last_end;
    double base_hold, max_hold, repeat_s;
    uint32_t trips;
    const char *reason;
} FgBreaker;
void fg_breaker_init(FgBreaker *b, double base_hold, double max_hold, double repeat_s);
void fg_breaker_trip(FgBreaker *b, double now, const char *reason);
int  fg_breaker_open(const FgBreaker *b, double now);   /* 1 = generation allowed */

/* The cost of one generated frame, as the plan sees it.
 *  - Cold samples are discarded: the first frames after the surfaces were
 *    (re)allocated or generation was enabled pay first-touch costs (driver
 *    allocation, page faults) that steady state never sees; fg_cost_cold()
 *    marks the next `n` samples as such.
 *  - A stale estimate is re-probed: when the estimate has kept the plan at
 *    zero for `probe_s`, fg_cost_estimate() reports 0 (unknown) once so one
 *    frame is generated and measured, and that probe's sample replaces the
 *    estimate instead of blending into it. A probe that still does not fit
 *    doubles the wait before the next one (up to max_probe_s); one that fits
 *    resets it. */
typedef struct FgCost {
    double ema;          /* seconds, 0 = unknown */
    uint32_t samples, discarded, probes;
    int    cold;         /* samples still to discard */
    int    probing;      /* the next sample is a probe's */
    double blocked_since;/* when the estimate started keeping the plan at 0 */
    double probe_at;     /* when the probe was granted */
    double probe_s, base_probe_s, max_probe_s;
} FgCost;
void   fg_cost_init(FgCost *c, double probe_s, double max_probe_s);
void   fg_cost_cold(FgCost *c, int n);
/* One measured frame; `fit_s` is what one generated frame may cost to fit. */
void   fg_cost_add(FgCost *c, double cost_s, double fit_s);
/* The estimate to plan with at `now`: 0 when unknown or due for a probe.
 * `fit_s` as above: an estimate above it is what blocks the plan. */
double fg_cost_estimate(FgCost *c, double now, double fit_s);

/* A ceiling on the plan for what the cost estimate cannot see (the real
 * frames' GPU work): an overload trip while n frames were planned lowers
 * it to n - 1; each `recover_s` without one raises it by one, up to max. */
typedef struct FgCeiling { int cap, max; double last; double recover_s; } FgCeiling;
void fg_ceiling_init(FgCeiling *c, int max, double recover_s);
void fg_ceiling_trip(FgCeiling *c, int n_planned, double now);
int  fg_ceiling_get(FgCeiling *c, double now);

/* The guest's own pacing: frame boundaries against a schedule that advances
 * one interval per frame. Late only when the guest slipped more than `slack_s`
 * behind its schedule (jitter, a long frame followed by a short one, does not
 * count); the schedule then restarts from now. Early boundaries pull it in. */
typedef struct FgPace { double next; int primed; } FgPace;
int fg_pace_note(FgPace *p, double now, double period_s, double slack_s);

#ifdef __cplusplus
}
#endif
#endif
