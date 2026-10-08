#pragma once
#include "mod_plugins.h"

namespace psx {

enum class MouseControl { None, Left = 1, Middle, Right, X1, X2, Escape, LeftAlt };
struct LocalMouseHost {
    uint32_t buttons = 0xFFFF;
    bool live = false, focused = false, connected = false, analog = false;
    bool native_right = false, start = false, hold_conflict = false;
};
// Private backend seam also used by deterministic tests. No second event queue:
// the existing owner-thread event drain dispatches synchronously in order.
struct LocalMouseBackend {
    void* context;
    bool (*capture)(void*, bool);
    void (*suppress)(void*, MouseControl);
    void (*notice)(void*, const char*);
};
class LocalMousePolicy {
public:
    explicit LocalMousePolicy(LocalMouseBackend backend) : backend_(backend) {}
    bool install(const PSXModMousePolicy* policy);
    void clear();
    void reset();
    void update(uint64_t now, LocalMouseHost host);
    bool control(uint64_t t, uint64_t now, MouseControl control, bool down, bool repeat);
    // Reconcile a LEFT release the host never delivered (e.g. made while unfocused).
    void sync_left(uint64_t now, bool physically_down);
    void motion(uint64_t t, uint64_t now, double dx, double dy);
    void sample(uint64_t now, uint8_t& rx, uint8_t& ry);
    bool captured() const { return captured_; }
    bool installed() const { return policy_.event != nullptr; }
    MouseControl hold_control() const;
private:
    bool allowed() const;
    bool valid_time(uint64_t t, uint64_t now);
    void dispatch(uint32_t type, uint64_t t = 0, double dx = 0, double dy = 0);
    LocalMouseBackend backend_;
    PSXModMousePolicy policy_{};
    LocalMouseHost host_;
    uint64_t clock_ = 0, event_clock_ = 0, release_clock_ = 0;
    bool clock_valid_ = false, event_valid_ = false;
    bool captured_ = false, hold_down_ = false, hold_blocked_ = false;
    bool activation_down_ = false, activation_owned_ = false;
    bool hold_active_ = false; // accepted interpretation, separate from physical state
    bool conflict_noticed_ = false, capture_failure_noticed_ = false;
};
} // namespace psx
