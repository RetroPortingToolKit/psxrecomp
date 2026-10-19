#include "video_telemetry.h"
#include <atomic>

namespace {
std::atomic<uint64_t> presents{0}, errors{0};
std::atomic<int> status_available{0};
std::atomic<unsigned> generation{0};
std::atomic<int> fields[8]{};
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
    status_available.store(result >= 0, std::memory_order_relaxed);
    (result != 0 ? presents : errors).fetch_add(1, std::memory_order_relaxed);
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
