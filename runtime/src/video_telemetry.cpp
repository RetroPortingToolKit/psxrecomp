#include "video_telemetry.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

namespace {
std::atomic<uint64_t> presents{0}, errors{0};
std::atomic<int> status_available{0};
std::atomic<unsigned> generation{0};
std::atomic<int> fields[8]{};
constexpr unsigned cadence_capacity = 4096;
std::atomic<uint32_t> intervals[cadence_capacity]{};
std::atomic<uint64_t> cadence_version{0}, cadence_count{0};
std::atomic<bool> cadence_reset{false};
// Only the current present owner touches these. Readers request reset rather
// than changing the producer's cursor or timestamp.
uint64_t last_present_us;
bool have_last_present;
}

extern "C" void psx_video_telemetry_compose(int renderer, int scale, int sw, int sh,
                                           int dw, int dh, int vw, int vh) {
    const int values[] = {renderer, scale, sw, sh, dw, dh, vw, vh};
    // Single composing owner. Atomic fields avoid data races even when the
    // bounded reader abandons an in-progress publication. This observer never
    // waits for the renderer, queries GL, hashes pixels or changes guest state.
    generation.fetch_add(1);
    for (unsigned i = 0; i < 8; ++i) fields[i].store(values[i]);
    generation.fetch_add(1);
}

extern "C" void psx_video_telemetry_present(int result) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    psx_video_telemetry_present_at(result,
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

extern "C" void psx_video_telemetry_present_at(int result, uint64_t now_us) {
    status_available.store(result >= 0, std::memory_order_relaxed);
    (result != 0 ? presents : errors).fetch_add(1, std::memory_order_relaxed);
    if (result == 0) return;
    cadence_version.fetch_add(1);
    if (cadence_reset.exchange(false)) {
        cadence_count.store(0);
        have_last_present = false;
    }
    if (have_last_present && now_us > last_present_us) {
        const uint64_t count = cadence_count.load();
        const auto delta = std::min<uint64_t>(now_us - last_present_us, UINT32_MAX);
        intervals[count % cadence_capacity].store(static_cast<uint32_t>(delta));
        cadence_count.store(count + 1);
    }
    last_present_us = now_us;
    have_last_present = true;
    cadence_version.fetch_add(1);
}

extern "C" void psx_video_telemetry_cadence(int window_ms, int reset, PsxPresentCadence *out) {
    if (!out) return;
    *out = {};
    uint32_t values[cadence_capacity]; // each reader owns its sorting storage
    uint64_t sum = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        out->count = 0; sum = 0;
        const uint64_t before = cadence_version.load();
        if (before & 1u) continue;
        if (cadence_reset.load()) { out->valid = 1; break; }
        const uint64_t count = cadence_count.load();
        const unsigned n = static_cast<unsigned>(std::min<uint64_t>(count, cadence_capacity));
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t value = intervals[(count - 1 - i) % cadence_capacity].load();
            if (window_ms > 0 && sum + value > static_cast<uint64_t>(window_ms) * 1000) break;
            sum += value; values[out->count++] = value;
        }
        if (before == cadence_version.load()) { out->valid = 1; break; }
    }
    if (reset) cadence_reset.store(true);
    if (!out->valid) { *out = {}; return; }
    if (!out->count) return;
    std::sort(values, values + out->count);
    const auto k = out->count;
    out->mean_us = static_cast<uint32_t>(sum / k);
    out->p50_us = values[k / 2];
    out->p95_us = values[(k - 1) * 95 / 100];
    out->p99_us = values[(k - 1) * 99 / 100];
    out->max_us = values[k - 1];
    for (unsigned i = 0; i < k; ++i)
        if (static_cast<uint64_t>(values[i]) * 2 > static_cast<uint64_t>(out->p50_us) * 3)
            ++out->hitches;
    out->span_ms = sum / 1000.0;
    out->fps = sum ? k * 1e6 / static_cast<double>(sum) : 0;
}

extern "C" int psx_video_telemetry_cadence_json(int window_ms, int reset, char *out, int capacity) {
    PsxPresentCadence r{};
    psx_video_telemetry_cadence(window_ms, reset, &r);
    return std::snprintf(out, capacity,
        "\"valid\":%s,\"measurement\":\"host_api_completion\",\"swaps\":%u,\"span_ms\":%.1f,"
        "\"fps\":%.2f,\"mean_us\":%u,\"p50_us\":%u,\"p95_us\":%u,\"p99_us\":%u,\"max_us\":%u,\"hitches\":%u",
        r.valid ? "true" : "false", r.count, r.span_ms, r.fps, r.mean_us,
        r.p50_us, r.p95_us, r.p99_us, r.max_us, r.hitches);
}

extern "C" void psx_video_telemetry_snapshot(PsxVideoTelemetry *out) {
    if (!out) return;
    *out = {};
    out->presents = presents.load(std::memory_order_relaxed);
    out->present_errors = errors.load(std::memory_order_relaxed);
    out->present_status_available = status_available.load(std::memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const unsigned before = generation.load();
        if (!before || (before & 1u)) continue;
        int values[8];
        for (unsigned i = 0; i < 8; ++i) values[i] = fields[i].load();
        if (before != generation.load()) continue;
        out->valid = 1;
        out->renderer = values[0]; out->internal_scale = values[1];
        out->source_width = values[2]; out->source_height = values[3];
        out->drawable_width = values[4]; out->drawable_height = values[5];
        out->viewport_width = values[6]; out->viewport_height = values[7];
        return;
    }
}
