#include "frame_interpolation.h"

#include <math.h>
#include <string.h>

void frame_interpolation_schedule_reset(FrameInterpolationSchedule *schedule) {
    if (schedule) memset(schedule, 0, sizeof(*schedule));
}

int frame_interpolation_schedule_begin(FrameInterpolationSchedule *schedule,
                                       uint64_t now, uint64_t frequency,
                                       double source_hz, double target_hz) {
    return frame_interpolation_schedule_begin_phase(
        schedule, now, frequency, source_hz, target_hz, 0.0, 1.0);
}

static double clamp01(double v) {
    if (!(v > 0.0)) return 0.0;   /* also maps NaN to 0 */
    return v > 1.0 ? 1.0 : v;
}

int frame_interpolation_schedule_begin_phase(FrameInterpolationSchedule *schedule,
                                             uint64_t now, uint64_t frequency,
                                             double source_hz, double target_hz,
                                             double phase_lo, double phase_hi) {
    double source_period;
    double target_period;
    double now_d = (double)now;

    if (!schedule || frequency == 0 || !isfinite(source_hz) ||
        !isfinite(target_hz) || source_hz < 1.0 || target_hz < source_hz ||
        source_hz > 1000.0 || target_hz > 1000.0) {
        if (schedule) frame_interpolation_schedule_reset(schedule);
        return 0;
    }

    source_period = (double)frequency / source_hz;
    target_period = (double)frequency / target_hz;
    if (source_period < 1.0 || target_period < 1.0) {
        frame_interpolation_schedule_reset(schedule);
        return 0;
    }

    /* Re-anchor after startup, clock discontinuity, or a sustained overrun.
     * Normal guest work lands after the prior deadline but before the next one;
     * retaining the old anchor makes that work consume the pacing budget. */
    if (schedule->source_deadline <= 0.0 ||
        now_d + source_period < schedule->source_deadline ||
        now_d > schedule->source_deadline + source_period * 4.0) {
        schedule->frame_start = now_d;
        schedule->source_deadline = now_d + source_period;
        schedule->next_present_deadline = now_d + target_period;
    } else {
        schedule->frame_start = schedule->source_deadline;
        schedule->source_deadline += source_period;
    }

    schedule->frame_end = schedule->source_deadline;
    schedule->target_period = target_period;
    schedule->phase_lo = clamp01(phase_lo);
    schedule->phase_hi = clamp01(phase_hi);
    if (schedule->phase_hi < schedule->phase_lo)
        schedule->phase_hi = schedule->phase_lo;

    /* Coalesce stale output deadlines. Keep the newest missed deadline so an
     * over-budget guest frame can update the window once, then resume cadence. */
    while (schedule->next_present_deadline + target_period <= now_d)
        schedule->next_present_deadline += target_period;
    return 1;
}

int frame_interpolation_schedule_next(FrameInterpolationSchedule *schedule,
                                      uint64_t now, uint64_t *deadline,
                                      float *alpha) {
    double d;
    double span;
    double a;

    if (!schedule || schedule->target_period <= 0.0 ||
        schedule->next_present_deadline > schedule->frame_end + 0.5)
        return 0;

    d = schedule->next_present_deadline;
    schedule->next_present_deadline += schedule->target_period;
    if (d < (double)now) d = (double)now;
    if (d > schedule->frame_end) d = schedule->frame_end;

    span = schedule->frame_end - schedule->frame_start;
    a = span > 0.0 ? (d - schedule->frame_start) / span : 1.0;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    a = schedule->phase_lo + a * (schedule->phase_hi - schedule->phase_lo);
    if (deadline) *deadline = (uint64_t)(d + 0.5);
    if (alpha) *alpha = (float)a;
    return 1;
}

int frame_interpolation_schedule_due(FrameInterpolationSchedule *schedule,
                                     uint64_t now, double horizon,
                                     uint64_t *deadline) {
    double now_d = (double)now;
    if (!schedule || schedule->target_period <= 0.0 ||
        schedule->next_present_deadline <= 0.0)
        return 0;
    while (schedule->next_present_deadline + schedule->target_period <= now_d &&
           schedule->next_present_deadline + schedule->target_period <= horizon)
        schedule->next_present_deadline += schedule->target_period;
    if (schedule->next_present_deadline > now_d ||
        schedule->next_present_deadline > horizon)
        return 0;
    if (deadline) *deadline = (uint64_t)(schedule->next_present_deadline + 0.5);
    return 1;
}

void frame_interpolation_schedule_consume(FrameInterpolationSchedule *schedule) {
    if (schedule && schedule->target_period > 0.0)
        schedule->next_present_deadline += schedule->target_period;
}

uint64_t frame_interpolation_schedule_end(
    const FrameInterpolationSchedule *schedule) {
    if (!schedule || schedule->frame_end <= 0.0) return 0;
    return (uint64_t)(schedule->frame_end + 0.5);
}

void frame_flip_tracker_reset(FrameFlipTracker *tracker) {
    if (!tracker) return;
    tracker->since_flip = 0;
    tracker->period = 1;
    tracker->frames = 0;
}

int frame_flip_is_new_frame(int geometry_changed, int history_empty,
                            int redrawn, int origin_x, int origin_y,
                            int last_x, int last_y) {
    return geometry_changed || history_empty || redrawn ||
           origin_x != last_x || origin_y != last_y;
}

uint32_t frame_flip_tracker_vblank(FrameFlipTracker *tracker, int new_frame,
                                   double *phase_lo, double *phase_hi) {
    uint32_t k, p;
    if (!tracker) {
        if (phase_lo) *phase_lo = 0.0;
        if (phase_hi) *phase_hi = 1.0;
        return 0;
    }
    if (tracker->period == 0) tracker->period = 1;
    if (new_frame) {
        if (tracker->frames > 0) {
            p = tracker->since_flip;
            if (p < 1) p = 1;
            if (p > FRAME_FLIP_PERIOD_MAX) p = FRAME_FLIP_PERIOD_MAX;
            tracker->period = p;
        }
        if (tracker->frames < 0xFFFFFFFFu) tracker->frames++;
        tracker->since_flip = 0;
    }
    k = tracker->since_flip;
    p = tracker->period;
    if (phase_lo) *phase_lo = clamp01((double)k / (double)p);
    if (phase_hi) *phase_hi = clamp01((double)(k + 1u) / (double)p);
    if (tracker->since_flip < 0xFFFFu) tracker->since_flip++;
    return k;
}
