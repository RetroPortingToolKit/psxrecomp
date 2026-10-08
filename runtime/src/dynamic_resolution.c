/* dynamic_resolution.c — see dynamic_resolution.h. */
#include "dynamic_resolution.h"

#include <string.h>

void dynres_default_params(DynresParams *p) {
    memset(p, 0, sizeof *p);
    p->window_s = 0.5;
    p->late_factor = 1.10;
    p->gap_factor = 4.0;
    p->gap_hold_s = 1.0;
    p->down_load = 0.90;
    p->down_late = 2;
    p->down_load_sustained = 0.92;
    p->target_load = 0.80;
    p->cooldown_down_s = 2.0;
    p->burst_late = 6;
    p->verify_fraction = 0.30;
    p->verify_undo_s = 1.0;
    p->verify_block_s = 30.0;
    p->verify_block_max_s = 480.0;
    p->verify_forget_s = 300.0;
    p->up_load = 0.75;
    p->up_after_s = 3.0;
    p->cooldown_up_after_up_s = 3.0;
    p->cooldown_up_after_down_s = 5.0;
    p->relapse_s = 10.0;
    p->relapse_block_s = 10.0;
    p->relapse_block_max_s = 160.0;
    p->relapse_forget_s = 60.0;
    p->prior_scaled = 0.6;
    p->learn_rate = 0.3;
    p->step_cost_s = 0.004;
}

static int clamp_level(const DynresController *c, int l) {
    if (l < c->floor) l = c->floor;
    if (l > c->ceiling) l = c->ceiling;
    return l;
}

void dynres_init(DynresController *c, const DynresParams *p, int floor_level,
                 int ceiling, int level) {
    memset(c, 0, sizeof *c);
    if (p) c->p = *p; else dynres_default_params(&c->p);
    if (ceiling < 1) ceiling = 1;
    if (ceiling > DYNRES_MAX_LEVEL) ceiling = DYNRES_MAX_LEVEL;
    if (floor_level < 1) floor_level = 1;
    if (floor_level > ceiling) floor_level = ceiling;
    c->floor = floor_level;
    c->ceiling = ceiling;
    c->level = clamp_level(c, level);
    c->f = c->p.prior_scaled;
    c->prev_load = -1.0;
    c->last_step_t = -1e9;
    c->last_down_t = -1e9;
    c->verify_last_fail = -1e9;
    c->verify_dur = c->p.verify_block_s;
    c->step_cost_s = c->p.step_cost_s;
    for (int i = 0; i <= DYNRES_MAX_LEVEL; i++) {
        c->up_reached_t[i] = -1e9;
        c->relapse_dur[i] = c->p.relapse_block_s;
    }
    c->last_reason = "start";
}

double dynres_predict(const DynresController *c, double load, int from, int to) {
    if (from < 1 || to < 1) return load;
    double q = ((double)to * (double)to) / ((double)from * (double)from);
    return load * ((1.0 - c->f) + c->f * q);
}

double dynres_up_blocked_s(const DynresController *c, int level, double now_s) {
    if (level < 0 || level > DYNRES_MAX_LEVEL) return 0.0;
    double r = c->up_block_until[level] - now_s;
    return r > 0.0 ? r : 0.0;
}

static void window_reset(DynresController *c) {
    c->win_period = c->win_work = c->win_wall = 0.0;
    c->win_n = c->win_late = 0;
}

/* Everything that needs consecutive clean windows starts over. */
static void discard(DynresController *c) {
    if (c->win_n) c->held_windows++;
    window_reset(c);
    c->prev_load = -1.0;
    c->up_streak_s = 0.0;
    c->up_ready = 0;
    /* A step's effect is judged on clean windows only: drop the judgement
     * (no learning, no undo) rather than blame a hold on the resolution. */
    c->post_active = 0;
}

void dynres_hold(DynresController *c, double now_s, double tail_s) {
    double until = now_s + (tail_s > 0.0 ? tail_s : 0.0);
    if (until > c->hold_until) c->hold_until = until;
    discard(c);
}

void dynres_note_step_cost(DynresController *c, double seconds) {
    if (seconds <= 0.0) return;
    c->step_cost_s = c->step_cost_s + 0.3 * (seconds - c->step_cost_s);
}

int dynres_force(DynresController *c, int level) {
    if (level <= 0) {
        c->forced = 0;
        c->last_reason = "force released";
        discard(c);
        return c->level;
    }
    c->forced = clamp_level(c, level);
    c->level = c->forced;
    c->post_active = 0;
    c->undo_level = 0;
    c->last_reason = "forced";
    discard(c);
    return c->level;
}

static void learn(DynresController *c, double before, double after, int from, int to) {
    if (before <= 0.05 || from == to) return;
    double q = ((double)to * (double)to) / ((double)from * (double)from);
    double fo = (after / before - 1.0) / (q - 1.0);
    if (fo < 0.1) fo = 0.1;
    if (fo > 0.95) fo = 0.95;
    c->f += c->p.learn_rate * (fo - c->f);
}

static void begin_post(DynresController *c, int from, int to, double load_before,
                       int down) {
    c->post_active = 1;
    c->post_from = from;
    c->post_to = to;
    c->post_windows = 0;
    c->post_down = down;
    c->post_load_before = load_before;
    c->post_pred = dynres_predict(c, load_before, from, to);
}

static int step_down(DynresController *c, double now, double load, const char *why) {
    int from = c->level, to = c->floor;
    for (int l = from - 1; l >= c->floor; l--)
        if (dynres_predict(c, load, from, l) <= c->p.target_load) { to = l; break; }
    /* Relapse: an up step into this level failed within relapse_s. */
    if (now - c->up_reached_t[from] < c->p.relapse_s) {
        c->up_block_until[from] = now + c->relapse_dur[from];
        c->relapse_dur[from] *= 2.0;
        if (c->relapse_dur[from] > c->p.relapse_block_max_s)
            c->relapse_dur[from] = c->p.relapse_block_max_s;
        c->relapses++;
    }
    c->up_reached_t[from] = -1e9;
    begin_post(c, from, to, load, 1);
    c->level = to;
    c->last_step_t = c->last_down_t = now;
    c->last_step_up = 0;
    c->up_streak_s = 0.0;
    c->up_ready = 0;
    c->prev_load = -1.0;
    c->downs++;
    c->last_reason = why;
    c->last_decision_t = now;
    return c->level;
}

static int step_up(DynresController *c, double now, double load, int to, const char *why) {
    int from = c->level;
    begin_post(c, from, to, load, 0);
    c->level = to;
    c->up_reached_t[to] = now;
    c->last_step_t = now;
    c->last_step_up = 1;
    c->up_streak_s = 0.0;
    c->up_ready = 0;
    c->prev_load = -1.0;
    c->ups++;
    c->last_reason = why;
    c->last_decision_t = now;
    return c->level;
}

/* A window closed: judge a recent step, then the rules. */
static int close_window(DynresController *c, double now) {
    const double load = c->win_work / c->win_period;
    const int late = c->win_late;
    const double dur = c->win_period;
    c->last_load = load;
    c->last_late = late;
    c->last_vblank_hz = c->win_wall > 0.0 ? (double)c->win_n / c->win_wall : 0.0;
    c->last_valid = 1;
    c->windows++;
    window_reset(c);

    if (c->post_active && ++c->post_windows >= 2) {
        /* The first window after a step settles; the second judges it. */
        c->post_active = 0;
        double fell = c->post_load_before - load;
        double want = c->post_load_before - c->post_pred;
        if (c->post_down && want > 0.0 && fell < c->p.verify_fraction * want) {
            /* Not resolution-bound: undo, and block down steps a while. */
            if (now - c->verify_last_fail > c->p.verify_forget_s)
                c->verify_dur = c->p.verify_block_s;
            c->down_block_until = now + c->verify_dur;
            c->verify_dur *= 2.0;
            if (c->verify_dur > c->p.verify_block_max_s)
                c->verify_dur = c->p.verify_block_max_s;
            c->verify_last_fail = now;
            c->undo_level = c->post_from;
            c->undo_at = now + c->p.verify_undo_s;
            c->last_reason = "down step did not lower the load (not resolution-bound)";
            c->last_decision_t = now;
        } else {
            learn(c, c->post_load_before, load, c->post_from, c->post_to);
        }
    }
    if (c->undo_level && now >= c->undo_at) {
        int to = clamp_level(c, c->undo_level);
        c->undo_level = 0;
        if (to != c->level) {
            c->undos++;
            c->level = to;
            c->last_step_t = now;
            c->last_step_up = 1;
            c->prev_load = -1.0;
            c->up_streak_s = 0.0;
            c->up_ready = 0;
            c->last_reason = "undo";
            c->last_decision_t = now;
            return c->level;
        }
    }

    /* A level an up step reached and kept for relapse_forget_s is stable:
     * its next relapse starts the back-off over. */
    if (now - c->up_reached_t[c->level] >= c->p.relapse_forget_s) {
        c->relapse_dur[c->level] = c->p.relapse_block_s;
        c->up_reached_t[c->level] = -1e9;
    }
    const double prev = c->prev_load;
    c->prev_load = load;
    if (c->post_active || c->undo_level) return c->level;   /* judging a step */

    int over = (load >= c->p.down_load && late >= c->p.down_late) ||
               (load > c->p.down_load_sustained && prev > c->p.down_load_sustained);
    if (over) {
        c->up_streak_s = 0.0;
        c->up_ready = 0;
        if (c->level > c->floor && now >= c->down_block_until &&
            (now - c->last_down_t >= c->p.cooldown_down_s || late >= c->p.burst_late))
            return step_down(c, now, load,
                             late >= c->p.down_late ? "busy with late frames"
                                                    : "busy for two windows");
        return c->level;
    }
    if (c->level >= c->ceiling) { c->up_streak_s = 0.0; c->up_ready = 0; return c->level; }
    const int next = c->level + 1;
    if (late == 0 && dynres_predict(c, load, c->level, next) <= c->p.up_load)
        c->up_streak_s += dur;
    else {
        c->up_streak_s = 0.0;
        c->up_ready = 0;
    }
    double cd = c->last_step_up ? c->p.cooldown_up_after_up_s : c->p.cooldown_up_after_down_s;
    c->up_ready = c->up_streak_s >= c->p.up_after_s - 1e-9 &&
                  now - c->last_step_t >= cd &&
                  now >= c->up_block_until[next];
    return c->level;
}

int dynres_sample(DynresController *c, double now_s, const DynresSample *s) {
    if (c->forced) return c->level;
    if (!s || s->period_s <= 0.0) return c->level;
    if (s->held || now_s < c->hold_until) {
        if (s->held) discard(c);
        else if (c->win_n) discard(c);
        return c->level;
    }
    if (s->wall_s > s->period_s * c->p.gap_factor) {
        /* A pause, a stall in the host, a window drag: not the scene. */
        dynres_hold(c, now_s, c->p.gap_hold_s);
        c->last_reason = "gap";
        return c->level;
    }
    double work = s->work_s < 0.0 ? 0.0 : s->work_s;
    if (c->up_ready) {
        /* Up steps land in an interval whose idle time covers the step. */
        double idle = s->period_s - work;
        if (idle >= c->step_cost_s && c->level < c->ceiling)
            return step_up(c, now_s, c->last_load, c->level + 1, "headroom");
    }
    c->win_period += s->period_s;
    c->win_work += work;
    c->win_wall += s->wall_s;
    c->win_n++;
    if (s->wall_s > s->period_s * c->p.late_factor) c->win_late++;
    if (c->win_period >= c->p.window_s - 1e-9) return close_window(c, now_s);
    return c->level;
}

/* ---- Render-thread mode (see dynamic_resolution.h) ----------------------- */

void dynrt_default_params(DynrtParams *p) {
    memset(p, 0, sizeof *p);
    p->window_s = 0.25;
    p->margin = 0.15;
    p->down_windows = 2;
    p->strong_load = 1.0;
    p->bp_strong = 0.05;
    p->bp_weak = 0.01;
    p->guest_slow = 1.03;
    p->guest_slack = 0.90;
    p->up_load = 0.70;
    p->up_after_s = 3.0;
    p->up_cooldown_s = 2.0;
    p->verify_fraction = 0.30;
    p->verify_block_s = 20.0;
    p->verify_block_max_s = 320.0;
    p->verify_forget_s = 300.0;
    p->strike_s = 2.0;
    p->relapse_s = 10.0;
    p->relapse_block_s = 10.0;
    p->relapse_block_max_s = 160.0;
    p->relapse_forget_s = 60.0;
    p->prior_scaled = 0.8;
    p->learn_rate = 0.3;
    p->gap_factor = 4.0;
    p->gap_hold_s = 1.0;
    p->min_coverage = 0.5;
    p->descent_s = 6.0;
    p->descent_load = 0.78;
}

static int rt_clamp(const DynrtController *c, int l) {
    if (l < c->floor) l = c->floor;
    if (l > c->ceiling) l = c->ceiling;
    return l;
}

void dynrt_init(DynrtController *c, const DynrtParams *p, int floor_level,
                int ceiling, int level) {
    memset(c, 0, sizeof *c);
    if (p) c->p = *p; else dynrt_default_params(&c->p);
    if (ceiling < 1) ceiling = 1;
    if (ceiling > DYNRES_MAX_LEVEL) ceiling = DYNRES_MAX_LEVEL;
    if (floor_level < 1) floor_level = 1;
    if (floor_level > ceiling) floor_level = ceiling;
    c->floor = floor_level;
    c->ceiling = ceiling;
    c->level = rt_clamp(c, level);
    c->f = c->p.prior_scaled;
    c->last_step_t = c->last_down_t = -1e9;
    c->verify_last_fail = -1e9;
    c->verify_dur = c->p.verify_block_s;
    for (int i = 0; i <= DYNRES_MAX_LEVEL; i++) {
        c->up_reached_t[i] = -1e9;
        c->relapse_dur[i] = c->p.relapse_block_s;
    }
    c->descent_armed = c->p.descent_s > 0.0;
    c->last_reason = "start";
}

void dynrt_arm_descent(DynrtController *c) {
    if (c->p.descent_s <= 0.0) return;
    c->descent_armed = 1;
    c->descent_stepped = 0;
    c->descent_until = 0.0;
}

/* Pure area (f = 1): the cost's fixed part only makes a level dearer than
 * this predicts, so a jump never lands below the level that fits; a jump
 * that lands too high is still over budget and jumps again from the new
 * measurement. */
int dynrt_descent_target(const DynrtController *c, double load, int level) {
    for (int l = level - 1; l > c->floor; l--)
        if (load * ((double)l * l) / ((double)level * level) <= c->p.descent_load) return l;
    return c->floor < level ? c->floor : level;
}

double dynrt_predict(const DynrtController *c, double load, int from, int to) {
    if (from < 1 || to < 1) return load;
    double q = ((double)to * (double)to) / ((double)from * (double)from);
    return load * ((1.0 - c->f) + c->f * q);
}

double dynrt_up_blocked_s(const DynrtController *c, int level, double now_s) {
    if (level < 0 || level > DYNRES_MAX_LEVEL) return 0.0;
    double r = c->up_block_until[level] - now_s;
    return r > 0.0 ? r : 0.0;
}

static void rt_window_reset(DynrtController *c) {
    c->win_period = c->win_wall = c->win_cost = c->win_bp = 0.0;
    c->win_n = c->win_frames = 0;
}

static void rt_discard(DynrtController *c) {
    if (c->win_n) c->held_windows++;
    rt_window_reset(c);
    c->over_streak = 0;
    c->up_streak_s = 0.0;
    c->post_active = 0;
}

void dynrt_hold(DynrtController *c, double now_s, double tail_s) {
    double until = now_s + (tail_s > 0.0 ? tail_s : 0.0);
    if (until > c->hold_until) c->hold_until = until;
    rt_discard(c);
}

int dynrt_force(DynrtController *c, int level) {
    if (level <= 0) {
        c->forced = 0;
        c->last_reason = "force released";
        rt_discard(c);
        return c->level;
    }
    c->forced = rt_clamp(c, level);
    c->level = c->forced;
    c->last_reason = "forced";
    rt_discard(c);
    return c->level;
}

static void rt_step(DynrtController *c, double now, int to, int down, double load,
                    double bp_share, const char *why) {
    const int from = c->level;
    c->post_active = 1;
    c->post_from = from;
    c->post_to = to;
    c->post_windows = 0;
    c->post_down = down;
    c->post_load_before = load;
    c->post_bp_before = bp_share;
    c->post_pred = dynrt_predict(c, load, from, to);
    if (down) {
        if (now - c->up_reached_t[from] < c->p.relapse_s) {
            c->up_block_until[from] = now + c->relapse_dur[from];
            c->relapse_dur[from] *= 2.0;
            if (c->relapse_dur[from] > c->p.relapse_block_max_s)
                c->relapse_dur[from] = c->p.relapse_block_max_s;
            c->relapses++;
        }
        c->up_reached_t[from] = -1e9;
        c->last_down_t = now;
        c->downs++;
    } else {
        c->up_reached_t[to] = now;
        c->ups++;
    }
    c->level = to;
    c->last_step_t = now;
    c->over_streak = 0;
    c->up_streak_s = 0.0;
    c->last_reason = why;
    c->last_decision_t = now;
}

static void rt_learn(DynrtController *c, double before, double after, int from, int to) {
    if (before <= 0.05 || from == to) return;
    double q = ((double)to * (double)to) / ((double)from * (double)from);
    double fo = (after / before - 1.0) / (q - 1.0);
    if (fo < 0.1) fo = 0.1;
    if (fo > 0.95) fo = 0.95;
    c->f += c->p.learn_rate * (fo - c->f);
}

static int rt_close_window(DynrtController *c, double now) {
    const double dur = c->win_period;
    const int n = c->win_n, frames = c->win_frames;
    const double wall = c->win_wall, cost = c->win_cost, bp = c->win_bp;
    rt_window_reset(c);
    c->windows++;
    if (frames < 1 || (double)frames < c->p.min_coverage * (double)n) {
        /* Mostly frames the emulation thread drew at a sync point, or the
         * costs have not arrived yet: nothing to judge on. */
        c->thin_windows++;
        c->over_streak = 0;
        c->up_streak_s = 0.0;
        c->post_active = 0;
        return c->level;
    }
    const double period = dur / (double)n;
    const double load = (cost / (double)frames) / period;
    const double bp_share = wall > 0.0 ? bp / wall : 0.0;
    const double interval = wall / (double)n;
    const double budget = 1.0 - c->p.margin;
    c->last_load = load;
    c->last_bp_share = bp_share;
    c->last_hz = wall > 0.0 ? (double)n / wall : 0.0;
    c->last_valid = 1;
    if (c->descent_armed) {
        if (c->descent_until <= 0.0) c->descent_until = now + c->p.descent_s;
        else if (now > c->descent_until) c->descent_armed = 0;
    }
    const int strong_bp = bp_share >= c->p.bp_strong;
    const int no_bp = bp_share < c->p.bp_weak;
    /* The guest is the limit: slow, never held by the queue, and the render
     * thread had room in the interval it was actually given. */
    const int guest_bound = no_bp && interval > period * c->p.guest_slow &&
                            (cost / (double)frames) < interval * c->p.guest_slack;
    c->last_guest_bound = guest_bound;
    if (guest_bound) c->guest_bound_windows++;

    /* Judge a recent step: the first window settles, the second judges. */
    if (c->post_active && ++c->post_windows >= 2) {
        c->post_active = 0;
        const double want = c->post_load_before - c->post_pred;
        const double fell = c->post_load_before - load;
        const int bp_cleared = c->post_bp_before >= c->p.bp_strong && no_bp;
        const int failed = c->post_down && want > 0.0 &&
                           fell < c->p.verify_fraction * want && !bp_cleared;
        if (failed) c->descent_armed = 0;   /* the model is off: single steps */
        if (failed && !c->verify_strikes) {
            /* One strike: the window before the step may have mixed lighter
             * frames in (a scene getting heavier, the cost's lag), so it
             * says little. Keep going; a second step that also removes
             * nothing is the evidence. */
            c->verify_strikes = 1;
            c->strike_from = c->post_from;
            c->strike_t = now;
            c->last_reason = "down step removed less than predicted (one strike)";
            c->last_decision_t = now;
        } else if (failed) {
            c->verify_strikes = 0;
            if (now - c->verify_last_fail > c->p.verify_forget_s)
                c->verify_dur = c->p.verify_block_s;
            c->down_block_until = now + c->verify_dur;
            c->verify_dur *= 2.0;
            if (c->verify_dur > c->p.verify_block_max_s)
                c->verify_dur = c->p.verify_block_max_s;
            c->verify_last_fail = now;
            const int to = rt_clamp(c, c->strike_from > c->post_from ? c->strike_from
                                                                         : c->post_from);
            if (to != c->level) {
                c->undos++;
                c->level = to;
                c->last_step_t = now;
            }
            c->over_streak = 0;
            c->up_streak_s = 0.0;
            c->last_reason = "two down steps did not lower the cost: undone, down steps blocked";
            c->last_decision_t = now;
            return c->level;
        } else {
            if (c->post_down) c->verify_strikes = 0;
            rt_learn(c, c->post_load_before, load, c->post_from, c->post_to);
        }
    }
    /* A strike is about the steps right after it. */
    if (c->verify_strikes && !c->post_active && now - c->strike_t > c->p.strike_s)
        c->verify_strikes = 0;
    if (now - c->up_reached_t[c->level] >= c->p.relapse_forget_s) {
        c->relapse_dur[c->level] = c->p.relapse_block_s;
        c->up_reached_t[c->level] = -1e9;
    }
    if (c->post_active) return c->level;

    const int over = !guest_bound && (load >= budget || strong_bp);
    c->last_over = over;
    if (over) {
        c->up_streak_s = 0.0;
        c->over_streak++;
        const int strong = strong_bp || load >= c->p.strong_load;
        if (c->level > c->floor && now >= c->down_block_until &&
            (strong || c->over_streak >= c->p.down_windows)) {
            int to = c->level - 1;
            const char *why = strong_bp ? "queue full (render thread behind)"
                              : strong ? "render cost over the interval"
                                       : "render cost over budget";
            /* Armed, and over by the meter (a full interval, or budget for
             * down_windows): jump to the predicted level. Backpressure alone
             * (a meter that under-reads) predicts no jump: single steps. */
            if (c->descent_armed &&
                (load >= c->p.strong_load || c->over_streak >= c->p.down_windows)) {
                const int t = dynrt_descent_target(c, load, c->level);
                if (t < to) {
                    to = t;
                    why = "fast descent (predicted from area)";
                    c->fast_downs++;
                }
            }
            if (c->descent_armed) c->descent_stepped = 1;
            rt_step(c, now, to, 1, load, bp_share, why);
        }
        else if (c->level <= c->floor)
            c->last_reason = "over budget at the floor";
        return c->level;
    }
    c->over_streak = 0;
    if (c->descent_stepped) c->descent_armed = 0;   /* it fits: fine-tune from here */
    if (guest_bound) c->last_reason = "guest-bound (no step)";
    if (c->level >= c->ceiling) { c->up_streak_s = 0.0; return c->level; }
    const int next = c->level + 1;
    if (no_bp && dynrt_predict(c, load, c->level, next) <= c->p.up_load)
        c->up_streak_s += dur;
    else
        c->up_streak_s = 0.0;
    if (c->up_streak_s >= c->p.up_after_s - 1e-9 &&
        now - c->last_step_t >= c->p.up_cooldown_s &&
        now >= c->up_block_until[next]) {
        c->descent_armed = 0;
        rt_step(c, now, next, 0, load, bp_share, "headroom");
    }
    return c->level;
}

int dynrt_sample(DynrtController *c, double now_s, const DynrtSample *s) {
    if (c->forced) return c->level;
    if (!s || s->period_s <= 0.0) return c->level;
    if (s->held || now_s < c->hold_until) {
        if (s->held || c->win_n) rt_discard(c);
        return c->level;
    }
    /* A long interval the queue explains is the render thread being slow,
     * not a gap. */
    const double unexplained = s->wall_s - (s->bp_s > 0.0 ? s->bp_s : 0.0);
    if (unexplained > s->period_s * c->p.gap_factor) {
        dynrt_hold(c, now_s, c->p.gap_hold_s);
        c->last_reason = "gap";
        return c->level;
    }
    c->win_period += s->period_s;
    c->win_wall += s->wall_s;
    c->win_n++;
    if (s->frames > 0 && s->cost_s >= 0.0) {
        c->win_frames += s->frames;
        c->win_cost += s->cost_s;
    }
    if (s->bp_s > 0.0) c->win_bp += s->bp_s;
    if (c->win_period >= c->p.window_s - 1e-9) return rt_close_window(c, now_s);
    return c->level;
}
