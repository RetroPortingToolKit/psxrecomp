#include "video_telemetry.h"
#include <atomic>
#include <cstdio>
#include <thread>
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); return 1; } } while (0)

int main() {
    PsxVideoTelemetry sample{};
    psx_video_telemetry_snapshot(&sample);
    CHECK(!sample.valid && !sample.presents && !sample.present_errors);
    // A composition is not a swap. A failed present is not a completed one.
    psx_video_telemetry_compose(PSX_VIDEO_SOFTWARE, 1, 320, 240, 640, 480, 640, 480);
    psx_video_telemetry_snapshot(&sample);
    CHECK(sample.valid && sample.renderer == PSX_VIDEO_SOFTWARE && !sample.presents);
    psx_video_telemetry_present(0);
    psx_video_telemetry_snapshot(&sample);
    CHECK(sample.present_errors == 1 && sample.presents == 0);
    CHECK(sample.present_status_available);
    psx_video_telemetry_present(1);
    psx_video_telemetry_snapshot(&sample);
    CHECK(sample.presents == 1 && sample.present_errors == 1);
    psx_video_telemetry_present(-1); // SDL2: the call returned, no status exists.
    psx_video_telemetry_snapshot(&sample);
    CHECK(!sample.present_status_available && sample.presents == 2);
    // A live resize and scale change publish together. A busy writer permits
    // an unavailable snapshot, never a fabricated/mixed coherent result.
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int scale = 1; scale <= 10000; ++scale) {
            psx_video_telemetry_compose(PSX_VIDEO_OPENGL, scale,
                                       320 * scale, 240 * scale,
                                       640 * scale, 480 * scale,
                                       640 * scale, 480 * scale);
            psx_video_telemetry_present(1);
        }
        done.store(true);
    });
    bool consistent = true;
    unsigned observations = 0;
    do {
        psx_video_telemetry_snapshot(&sample);
        if (sample.valid && sample.renderer == PSX_VIDEO_OPENGL) {
            consistent &= sample.source_width == sample.internal_scale * 320 &&
                          sample.drawable_width == sample.internal_scale * 640 &&
                          sample.viewport_height == sample.internal_scale * 480;
            ++observations;
        }
    } while (!done.load());
    producer.join();
    psx_video_telemetry_snapshot(&sample);
    CHECK(consistent && observations > 0);
    CHECK(sample.valid && sample.internal_scale == 10000 && sample.presents == 10002);
    CHECK(sample.present_status_available);
    CHECK(sample.present_errors == 1);
    psx_video_telemetry_snapshot(nullptr);
    return 0;
}
