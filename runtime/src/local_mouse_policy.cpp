#include "local_mouse_policy.h"
#include <algorithm>
#include <cmath>

namespace psx {
namespace { constexpr uint64_t StallNs = 250000000; }

bool LocalMousePolicy::install(const PSXModMousePolicy* policy) {
    if (installed() || !policy || policy->struct_size != sizeof(*policy) ||
        policy->hold_control > PSX_MOD_MOUSE_HOLD_LEFT_ALT ||
        !policy->eligible || !policy->event || !policy->sample) return false;
    policy_ = *policy;
    reset();
    return true;
}
void LocalMousePolicy::clear() {
    reset();
    policy_ = {};
    host_ = {};
    hold_down_ = hold_blocked_ = hold_active_ = false;
    activation_down_ = activation_owned_ = false;
    event_valid_ = false;
    clock_valid_ = false;
    clock_ = event_clock_ = release_clock_ = 0;
    conflict_noticed_ = capture_failure_noticed_ = false;
    persistent_ = transitions_ = suspended_ = false;
}
bool LocalMousePolicy::persistent_capture(bool enabled) {
    if (!installed()) return false;
    reset();
    persistent_ = enabled;
    return true;
}
bool LocalMousePolicy::capture_across_transitions(bool enabled) {
    if (!installed()) return false;
    reset();
    transitions_ = enabled;
    return true;
}
void LocalMousePolicy::dispatch(uint32_t type, uint64_t t, double dx, double dy) {
    if (!installed()) return;
    const PSXModMouseEvent event{sizeof(PSXModMouseEvent), type, t, dx, dy};
    policy_.event(&event);
}
void LocalMousePolicy::reset() {
    if (captured_) backend_.capture(backend_.context, false);
    captured_ = false;
    suspended_ = false;
    release_clock_ = clock_;
    event_valid_ = false;
    hold_blocked_ = hold_down_;
    hold_active_ = false;
    dispatch(PSX_MOD_MOUSE_RESET); // releases remain immediate even with bad time
}
MouseControl LocalMousePolicy::hold_control() const {
    if (policy_.hold_control == PSX_MOD_MOUSE_HOLD_RIGHT) return MouseControl::Right;
    if (policy_.hold_control == PSX_MOD_MOUSE_HOLD_LEFT_ALT) return MouseControl::LeftAlt;
    return MouseControl::None;
}
bool LocalMousePolicy::allowed() const {
    return installed() && host_.live && host_.focused && host_.connected &&
        host_.analog && !host_.native_right && !host_.start && policy_.eligible(host_.buttons);
}
bool LocalMousePolicy::capture_allowed() const {
    return installed() && host_.live && host_.focused && host_.connected &&
        host_.analog && !host_.native_right;
}
void LocalMousePolicy::suspend() {
    release_clock_ = clock_;
    event_valid_ = false;
    hold_blocked_ = hold_down_;
    hold_active_ = false;
    if (!suspended_) dispatch(PSX_MOD_MOUSE_RESET);
    suspended_ = true;
}
void LocalMousePolicy::update(uint64_t now, LocalMouseHost host) {
    const bool stalled = clock_valid_ && (now < clock_ || now - clock_ > StallNs);
    clock_ = now;
    clock_valid_ = true;
    host_ = host;
    if (transitions_ && persistent_ && captured_ && capture_allowed()) {
        if (stalled || !allowed()) suspend();
        else if (suspended_) {
            // Reject motion queued during the transition; resuming never
            // replays an old camera target or requires another click.
            release_clock_ = now;
            event_valid_ = false;
            suspended_ = false;
        }
    } else if (stalled || !allowed()) reset();
    if (host.hold_conflict && !conflict_noticed_) {
        backend_.notice(backend_.context, "Mouse hold disabled: control is bound to P1");
        conflict_noticed_ = true;
    }
    if (!host.hold_conflict) conflict_noticed_ = false;
    if (captured_ && host.hold_conflict && !hold_blocked_) {
        hold_blocked_ = true;
        if (hold_active_) {
            hold_active_ = false;
            dispatch(PSX_MOD_MOUSE_HOLD_RELEASE);
            event_clock_ = std::max(event_clock_, now);
            event_valid_ = true;
        }
    }
}
bool LocalMousePolicy::valid_time(uint64_t t, uint64_t now) {
    if (t > now || t < release_clock_ || now - t > StallNs ||
        (event_valid_ && t < event_clock_)) {
        reset();
        return false;
    }
    event_clock_ = t;
    event_valid_ = true;
    return true;
}
bool LocalMousePolicy::control(uint64_t t, uint64_t now, MouseControl control,
                               bool down, bool repeat) {
    if (control == MouseControl::Left) {
        const bool fresh = down && !activation_down_ && !repeat;
        activation_down_ = down;
        if (!down) {
            if (persistent_ && captured_ && (allowed() ||
                    (transitions_ && capture_allowed()))) {
                const bool owned = activation_owned_;
                activation_owned_ = false;
                // t=0 is the backend's release reconciliation, not a queued
                // event. Malformed real event times still release capture.
                if (t && !(transitions_ && t < release_clock_)) (void)valid_time(t, now);
                return owned;
            }
            // Release is unconditional: a future/backwards event time cannot
            // leave capture or the reducer's separate direction hold active.
            // A valid ordered release while still eligible owns its event
            // boundary, rather than the later time at which SDL drains it.
            // After an interruption or malformed release, retain reset()'s
            // processing-time barrier so queued input cannot rearm capture.
            const bool ordered_release = captured_ && allowed() &&
                t <= now && now - t <= StallNs && t >= release_clock_ &&
                (!event_valid_ || t >= event_clock_) &&
                (!clock_valid_ || (now >= clock_ && now - clock_ <= StallNs));
            const bool owned = activation_owned_;
            activation_owned_ = false;
            if (captured_ || owned) reset();
            if (ordered_release) release_clock_ = t;
            return owned;
        }
        // reset() retains physical down-state. A held button after a menu,
        // focus loss, native takeover or bad input is never a new activation.
        if (!fresh || captured_ || !allowed() || !valid_time(t, now)) return false;
        backend_.suppress(backend_.context, control);
        activation_owned_ = true;
        if (!backend_.capture(backend_.context, true)) {
            (void)backend_.capture(backend_.context, false);
            reset();
            if (!capture_failure_noticed_) {
                backend_.notice(backend_.context, "Mouse capture failed; native controls remain active");
                capture_failure_noticed_ = true;
            }
            return true;
        }
        captured_ = true;
        hold_blocked_ = hold_down_;
        dispatch(PSX_MOD_MOUSE_ACQUIRED, t);
        return true;
    }
    // Hold-release is never timestamp-gated. Track while released too so a
    // held control across acquisition/focus loss cannot become a new press.
    if (control != MouseControl::None && control == hold_control()) {
        const bool fresh = down && !hold_down_ && !repeat;
        hold_down_ = down;
        if (!down) {
            hold_blocked_ = false;
            if (captured_ && hold_active_) {
                hold_active_ = false;
                dispatch(PSX_MOD_MOUSE_HOLD_RELEASE);
                // Release is immediate, but cannot move the event watermark
                // backwards and let older motion refill the cleared target.
                const uint64_t release_time = t <= now && now - t <= StallNs ? t : clock_;
                event_clock_ = std::max(event_clock_, release_time);
                event_valid_ = true;
            }
        } else if (captured_ && allowed() && fresh && !host_.hold_conflict && !hold_blocked_) {
            if (valid_time(t, now)) {
                hold_active_ = true;
                dispatch(PSX_MOD_MOUSE_HOLD_PRESS, t);
            }
        }
    }
    if (down && control == MouseControl::Escape && captured_) {
        backend_.suppress(backend_.context, control);
        reset();
        return true;
    }
    return false;
}
void LocalMousePolicy::sync_left(uint64_t now, bool physically_down) {
    if (!physically_down && activation_down_)
        control(0, now, MouseControl::Left, false, false);
}
void LocalMousePolicy::motion(uint64_t t, uint64_t now, double dx, double dy) {
    if (!captured_) return;
    if (!allowed()) {
        if (transitions_ && persistent_ && capture_allowed()) suspend();
        else reset();
        return;
    }
    if (transitions_ && t < release_clock_) return;
    if (!std::isfinite(dx) || !std::isfinite(dy) ||
        std::abs(dx) > 1e9 || std::abs(dy) > 1e9) { reset(); return; }
    if (!valid_time(t, now) || (dx == 0.0 && dy == 0.0)) return;
    dispatch(PSX_MOD_MOUSE_MOTION, t, dx, dy);
}
void LocalMousePolicy::sample(uint64_t now, uint8_t& rx, uint8_t& ry) {
    if (!captured_) return;
    if (!allowed()) {
        if (transitions_ && persistent_ && capture_allowed()) suspend();
        else reset();
        return;
    }
    PSXModMouseOutput out{sizeof(PSXModMouseOutput), 0, 128, 128};
    policy_.sample(now, &out);
    if (out.struct_size != sizeof(out) || out.override_right > 1 || out.rx > 255 || out.ry > 255) {
        reset();
        return;
    }
    if (out.override_right) { rx = uint8_t(out.rx); ry = uint8_t(out.ry); }
}
} // namespace psx
