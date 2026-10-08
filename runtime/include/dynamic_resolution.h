/* dynamic_resolution.h — the controller behind [video] dynamic_resolution
 * (docs/ENHANCEMENTS.md, IR3).
 *
 * Pure C: no SDL, no GL and no clock of its own, so a unit test drives it
 * with synthetic time (tests/test_dynamic_resolution.c). The host feeds it one
 * sample per guest interval (one VBlank) and applies the level it returns;
 * the GL backend changes the internal scale between frames
 * (gl_renderer_step_internal_scale_now).
 *
 * LOAD. A sample's work is the interval's wall time minus the host's idle
 * waits (wall-clock pacer, the frame blend's waits for its next present, the
 * driver's vsync block in the swap), render passes and the frame blend's extra
 * presents: emulation, submission, the frame's own present and any driver
 * blocking. load = work / the guest's nominal interval, averaged over a
 * window (0.5 s). In-between frames only spend idle time and the load leaves
 * them out, so when a scene gets heavier they are shed first; the resolution
 * drops only when the game's own frames would be late.
 *
 * LEVELS are the integer scales floor..ceiling. A level's load is predicted
 * from the current one as load * ((1 - f) + f * (S'/S)^2), f being the share
 * of the load that scales with the pixel count. f starts at a prior and is
 * learned from what each step actually changed.
 *
 * RULES (DynresParams; defaults in dynres_default_params):
 *  - down: a window at >= down_load with >= down_late late intervals, or two
 *    windows above down_load_sustained. Target: the highest level predicted at
 *    <= target_load (at least one lower). No second down step within
 *    cooldown_down_s unless a window has >= burst_late late intervals.
 *  - verify: the second window after a down step must show at least
 *    verify_fraction of the predicted fall. Otherwise the pressure was not the
 *    resolution (a CPU-bound stretch): the step is undone after verify_undo_s
 *    and down steps are blocked for verify_block_s, doubling per failure.
 *  - up: one level, after up_after_s of windows predicted at <= up_load at the
 *    next level with no late interval, the cooldown since the last step over,
 *    the level not blocked, and only at an interval whose idle time covers the
 *    measured step cost.
 *  - relapse: a level reached by an up step and left by a down step within
 *    relapse_s is blocked for up steps for relapse_block_s, doubling up to
 *    relapse_block_max_s; an up step that holds the level for
 *    relapse_forget_s resets that.
 *  - holds: the host's holds (turbo, loads, FMV, compiles, savestate loads,
 *    resizes, the first seconds of the game), and any interval longer than
 *    gap_factor intervals, discard the window in progress; nothing is sampled
 *    or decided until the hold's tail has passed. A single slow interval never
 *    steps: every decision needs a whole window.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define DYNRES_MAX_LEVEL 32

typedef struct DynresParams {
    double window_s;                 /* decision window, guest time */
    double late_factor;              /* interval > period * this is late */
    double gap_factor;               /* interval > period * this: a gap, held */
    double gap_hold_s;
    double down_load;                /* with down_late late intervals */
    int    down_late;
    double down_load_sustained;      /* two windows above it */
    double target_load;
    double cooldown_down_s;
    int    burst_late;
    double verify_fraction;
    double verify_undo_s;
    double verify_block_s, verify_block_max_s, verify_forget_s;
    double up_load;
    double up_after_s;
    double cooldown_up_after_up_s, cooldown_up_after_down_s;
    double relapse_s;
    double relapse_block_s, relapse_block_max_s, relapse_forget_s;
    double prior_scaled;             /* f before any step was measured */
    double learn_rate;
    double step_cost_s;              /* step cost before one was measured */
} DynresParams;

void dynres_default_params(DynresParams *p);

typedef struct DynresSample {
    double period_s;   /* the guest's nominal interval (1 / 59.94 for NTSC) */
    double wall_s;     /* this interval's wall time */
    double work_s;     /* see LOAD above */
    int    held;       /* the host holds: not a sample, the window restarts */
} DynresSample;

typedef struct DynresController {
    DynresParams p;
    int    floor, ceiling, level, forced;
    double f;                         /* learned share of load ~ S^2 */
    /* the window in progress */
    double win_period, win_work, win_wall;
    int    win_n, win_late;
    /* the last closed window */
    double last_load, last_vblank_hz;
    int    last_late, last_valid;
    double prev_load;                 /* the window before it (-1 = none) */
    double hold_until;
    double up_streak_s;
    int    up_ready;
    double last_step_t, last_down_t;
    int    last_step_up;
    double up_reached_t[DYNRES_MAX_LEVEL + 1];
    double up_block_until[DYNRES_MAX_LEVEL + 1];
    double relapse_dur[DYNRES_MAX_LEVEL + 1];
    double down_block_until, verify_dur, verify_last_fail;
    /* after a step: the window it is judged on */
    int    post_active, post_from, post_to, post_windows, post_down;
    double post_load_before, post_pred;
    int    undo_level;
    double undo_at;
    double step_cost_s;
    /* telemetry */
    unsigned long long downs, ups, undos, relapses, windows, held_windows;
    const char *last_reason;
    double last_decision_t;
} DynresController;

void dynres_init(DynresController *c, const DynresParams *p, int floor_level,
                 int ceiling, int level);
/* Hold sampling and decisions until now_s + tail_s (the latest hold wins). */
void dynres_hold(DynresController *c, double now_s, double tail_s);
/* One guest interval ending at now_s. Returns the level to render at (the
 * current one when nothing changes; the host applies a change). */
int  dynres_sample(DynresController *c, double now_s, const DynresSample *s);
/* The measured wall time of a step the host applied. */
void dynres_note_step_cost(DynresController *c, double seconds);
/* Pin a level (debugging); 0 releases the pin. Returns the level to apply. */
int  dynres_force(DynresController *c, int level);
/* Predicted load at level `to` from `load` measured at `from`. */
double dynres_predict(const DynresController *c, double load, int from, int to);
/* Up steps into `level` blocked for this many seconds from now_s (0 = not). */
double dynres_up_blocked_s(const DynresController *c, int level, double now_s);


/* ---- Render-thread mode ([video] render_thread on) -------------------------
 * With the render thread the guest's frame no longer pays for GL, so the
 * emulation thread's wall time says nothing about whether the resolution
 * fits. This controller is fed what the render thread measured instead
 * (docs/RENDER_THREAD.md, "Dynamic resolution"):
 *
 * COST. Per replayed guest frame, max(GPU time, render-thread CPU time): GPU
 * time from a GL_TIMESTAMP pair (first record of the frame .. after its
 * present), CPU time the render thread spent replaying the frame minus its
 * idle waits for records and its block in the swap (the display's vsync).
 * load = mean cost / the guest's nominal interval (one display refresh at
 * 60 Hz); the budget is 1 - margin (15 %).
 *
 * BOUND. The render thread is the limit when the emulation thread blocked on
 * the queue bound (backpressure) or the cost is over budget. When the guest
 * runs below its nominal rate, never waits on the queue and the render
 * thread had slack in the actual interval, the guest is the limit: no down
 * step (resolution cannot help). Backpressure for bp_strong of a window is a
 * strong signal: one window suffices.
 *
 * RULES (DynrtParams; dynrt_default_params):
 *  - down: ONE level, after down_windows consecutive over-budget windows (one
 *    if strong). Never below the floor.
 *  - judge: the window after a step settles (the cost lags the queue and the
 *    GPU readback); the next one judges it. A down step that removed less
 *    than verify_fraction of the predicted cost (and did not end the
 *    backpressure) is a strike: the window it was decided on may have mixed
 *    lighter frames in. A second such step in a row (within strike_s) means
 *    the cost is not the pixels: both are undone and down steps are blocked
 *    for verify_block_s, doubling per failure up to verify_block_max_s.
 *  - up: one level, after up_after_s of clean windows (no backpressure, the
 *    next level predicted at <= up_load), up_cooldown_s after the last step,
 *    and the level not blocked. The prediction is load * ((1-f) + f*(S'/S)^2)
 *    with f, the share of the cost that scales with pixels, learned from each
 *    judged step. down_load > up_load is the hysteresis.
 *  - fast descent: armed at init (session start, render-thread start, a
 *    resolution change) and by dynrt_arm_descent (savestate load, game
 *    entry, window resize); it runs for descent_s from the first window
 *    judged after arming. While armed, a down step taken on a window whose
 *    load is at or over strong_load, or after down_windows consecutive
 *    over-budget windows, goes straight to the highest level the
 *    pure-area prediction (load * (S'/S)^2) puts at or below descent_load,
 *    several levels at once. A fixed part of the cost only makes the target
 *    dearer than predicted, so a jump never lands below the level that fits;
 *    it is judged like any step, and a judged window still over budget jumps
 *    again from the new measurement.
 *    A window that fits after a down step, an up step or a failed judgement
 *    disarms it (light windows before the heavy part do not), after
 *    which single steps fine-tune. descent_load < budget, and the level
 *    above the target is predicted over descent_load > up_load, so the jump
 *    is not followed by an up step back (no oscillation).
 *  - relapse: a level reached by an up step and left by a down step within
 *    relapse_s is blocked for up steps for relapse_block_s, doubling up to
 *    relapse_block_max_s; holding it relapse_forget_s resets that.
 *  - holds and gaps discard the window in progress, as in the controller
 *    above; so does a window in which fewer than min_coverage of the guest
 *    frames have a measured cost (frames the emulation thread drew itself
 *    at a sync point). */
typedef struct DynrtParams {
    double window_s;
    double margin;                   /* budget = 1 - margin of the interval */
    int    down_windows;             /* consecutive over-budget windows */
    double strong_load;              /* load at or above: one window suffices */
    double bp_strong;                /* backpressure share of wall: strong */
    double bp_weak;                  /* below: no backpressure */
    double guest_slow;               /* interval > period * this: guest slow */
    double guest_slack;              /* cost < interval * this: render slack */
    double up_load;
    double up_after_s;
    double up_cooldown_s;
    double verify_fraction;
    double verify_block_s, verify_block_max_s, verify_forget_s;
    double strike_s;                 /* a first failed judgement lapses after */
    double relapse_s;
    double relapse_block_s, relapse_block_max_s, relapse_forget_s;
    double prior_scaled;
    double learn_rate;
    double gap_factor, gap_hold_s;
    double min_coverage;
    double descent_s;                /* fast descent lasts this long once judging */
    double descent_load;             /* fast descent target (predicted load) */
} DynrtParams;

void dynrt_default_params(DynrtParams *p);

typedef struct DynrtSample {
    double period_s;   /* the guest's nominal interval */
    double wall_s;     /* this guest interval's wall time */
    int    frames;     /* render-thread frames whose cost arrived since the last sample */
    double cost_s;     /* their summed cost (see COST) */
    double bp_s;       /* emulation thread blocked on the queue bound */
    int    held;       /* the host holds: not a sample, the window restarts */
} DynrtSample;

typedef struct DynrtController {
    DynrtParams p;
    int    floor, ceiling, level, forced;
    double f;
    /* the window in progress */
    double win_period, win_wall, win_cost, win_bp;
    int    win_n, win_frames;
    /* the last closed window */
    double last_load, last_bp_share, last_hz;
    int    last_valid, last_guest_bound, last_over;
    int    over_streak;
    double hold_until;
    double up_streak_s;
    double last_step_t, last_down_t;
    double up_reached_t[DYNRES_MAX_LEVEL + 1];
    double up_block_until[DYNRES_MAX_LEVEL + 1];
    double relapse_dur[DYNRES_MAX_LEVEL + 1];
    double down_block_until, verify_dur, verify_last_fail;
    int    post_active, post_from, post_to, post_windows, post_down;
    double post_load_before, post_pred, post_bp_before;
    int    verify_strikes, strike_from;
    double strike_t;
    /* fast descent: armed; deadline (0 = starts at the next judged window) */
    int    descent_armed, descent_stepped;
    double descent_until;
    /* telemetry */
    unsigned long long downs, ups, undos, relapses, windows, held_windows,
                       guest_bound_windows, thin_windows, fast_downs;
    const char *last_reason;
    double last_decision_t;
} DynrtController;

void dynrt_init(DynrtController *c, const DynrtParams *p, int floor_level,
                int ceiling, int level);
void dynrt_hold(DynrtController *c, double now_s, double tail_s);
/* One guest interval ending at now_s; returns the level to render at. */
int  dynrt_sample(DynrtController *c, double now_s, const DynrtSample *s);
int  dynrt_force(DynrtController *c, int level);
/* Arm fast descent (see RULES): a savestate load, game entry, a resize. */
void dynrt_arm_descent(DynrtController *c);
/* Fast-descent target from `level` at `load`: the highest level in
 * [floor, level - 1] whose pure-area prediction is at or below
 * descent_load (floor if none). */
int  dynrt_descent_target(const DynrtController *c, double load, int level);
double dynrt_predict(const DynrtController *c, double load, int from, int to);
double dynrt_up_blocked_s(const DynrtController *c, int level, double now_s);

#ifdef __cplusplus
}
#endif
