#pragma once

#include <stdint.h>

/* Always-on host diagnostics. Completed scopes survive startup for the debug
 * TCP reader. Nested scopes overlap: do not sum a parent and its children.
 * No guest clocks, filesystem paths, logging, or environment switches. */
#define HOST_LAUNCH_TIMING_CAPACITY 256u
#define HOST_LAUNCH_TIMING_LABEL 64u

typedef enum HostLaunchStage {
    HOST_LAUNCH_PROVIDER_COMMIT,
    HOST_LAUNCH_RUNTIME_COMMIT,
    HOST_LAUNCH_COMMIT,
    HOST_LAUNCH_DISC_HASH,
    HOST_LAUNCH_PREPARE_RESOURCES,
    HOST_LAUNCH_MEDIA_PROVIDER,
    HOST_LAUNCH_RESOLVE,
    HOST_LAUNCH_OVERLAY_VERIFY,
    HOST_LAUNCH_DERIVED_DISC,
    HOST_LAUNCH_SAVE_STATE,
    HOST_LAUNCH_BUILD_DISC_INDEX,
    HOST_LAUNCH_OVERLAY_WORKER,
    HOST_LAUNCH_OVERLAY_JOIN,
    HOST_LAUNCH_PLUGIN_ACTIVATION,
    HOST_LAUNCH_PLUGIN_CALLBACK,
    HOST_LAUNCH_PROVIDER_NETPLAY_COMMIT
} HostLaunchStage;

typedef struct HostLaunchTimingEvent {
    uint64_t seq;             /* completion order, starting at 1 */
    uint64_t scope_id;        /* unique start identity, independent of seq */
    uint64_t parent_id;       /* same-thread enclosing scope, or 0 */
    uint64_t start_us;        /* steady-clock epoch, not guest time */
    uint64_t duration_us;
    uint32_t stage;
    uint32_t ok;              /* explicit success; early return/throw = 0 */
    uint32_t labels_truncated;
    char package_id[HOST_LAUNCH_TIMING_LABEL];
    char feature_id[HOST_LAUNCH_TIMING_LABEL];
} HostLaunchTimingEvent;

#ifdef __cplusplus
extern "C" {
#endif
/* Newest min(cap, retained) completions, oldest first. Null/zero output is a
 * metadata query. total is lifetime completed count; overwritten excludes
 * additional entries omitted by a reader's smaller cap. Never drains/reset. */
uint32_t host_launch_timing_snapshot(HostLaunchTimingEvent* out, uint32_t cap,
                                    uint64_t* total, uint64_t* overwritten);
const char* host_launch_timing_stage_name(uint32_t stage);
#ifdef __cplusplus
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

namespace PSXRecompV4 {
/* Header-only to keep package-manager fixtures independent of mod runtime.
 * The external-linkage inline accessors' local statics have one instance
 * across C++ translation units, not one ring per includer. */
class HostLaunchTimingRing {
    std::mutex mutex_;
    std::array<HostLaunchTimingEvent, HOST_LAUNCH_TIMING_CAPACITY> events_{};
    uint64_t started_ = 0, completed_ = 0;
public:
    uint64_t begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ++started_;
    }
    void finish(HostLaunchTimingEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        event.seq = ++completed_;
        events_[(event.seq - 1) % events_.size()] = event;
    }
    uint32_t snapshot(HostLaunchTimingEvent* out, uint32_t cap,
                      uint64_t* total, uint64_t* overwritten) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (total) *total = completed_;
        if (overwritten) *overwritten = completed_ > events_.size()
            ? completed_ - events_.size() : 0;
        if (!out) return 0;
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(
            std::min<uint64_t>(cap, events_.size()), completed_));
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t seq = completed_ - n + i + 1;
            out[i] = events_[(seq - 1) % events_.size()];
        }
        return n;
    }
};

inline HostLaunchTimingRing& host_launch_timing_ring() {
    static HostLaunchTimingRing ring;
    return ring;
}

struct HostLaunchTimingParent { HostLaunchTimingRing* ring; uint64_t id; };
inline HostLaunchTimingParent& host_launch_timing_parent() {
    static thread_local HostLaunchTimingParent parent{};
    return parent;
}
inline uint64_t host_launch_now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

class HostLaunchTimingScope {
    HostLaunchTimingRing& ring_;
    HostLaunchTimingEvent event_{};
    HostLaunchTimingParent previous_;
    void label(char* out, const char* text) {
        if (!text) return;
        const auto size = std::strlen(text);
        const auto n = std::min<size_t>(size, HOST_LAUNCH_TIMING_LABEL - 1u);
        std::memcpy(out, text, n);
        event_.labels_truncated |= size != n;
    }
public:
    explicit HostLaunchTimingScope(HostLaunchStage stage,
        const char* package = nullptr, const char* feature = nullptr,
        HostLaunchTimingRing& ring = host_launch_timing_ring())
        : ring_(ring), previous_(host_launch_timing_parent()) {
        event_.scope_id = ring_.begin();
        event_.parent_id = previous_.ring == &ring_ ? previous_.id : 0;
        event_.stage = stage;
        label(event_.package_id, package);
        label(event_.feature_id, feature);
        event_.start_us = host_launch_now_us();
        host_launch_timing_parent() = {&ring_, event_.scope_id};
    }
    HostLaunchTimingScope(const HostLaunchTimingScope&) = delete;
    HostLaunchTimingScope& operator=(const HostLaunchTimingScope&) = delete;
    ~HostLaunchTimingScope() {
        event_.duration_us = host_launch_now_us() - event_.start_us;
        host_launch_timing_parent() = previous_;
        ring_.finish(event_);
    }
    void success() { event_.ok = 1; }
};
} // namespace PSXRecompV4
#endif
