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
    PsxPresentCadence cadence{};
    psx_video_telemetry_cadence(0,1,&cadence);
    psx_video_telemetry_present_at(1,1000);
    psx_video_telemetry_present_at(1,11000);
    psx_video_telemetry_present_at(-1,21000);
    psx_video_telemetry_present_at(1,51000);
    psx_video_telemetry_cadence(0,0,&cadence);
    CHECK(cadence.valid && cadence.count==3 && cadence.mean_us==16666);
    CHECK(cadence.p50_us==10000 && cadence.max_us==30000 && cadence.hitches==1);
    psx_video_telemetry_cadence(35,0,&cadence);
    CHECK(cadence.valid && cadence.count==1 && cadence.max_us==30000);
    psx_video_telemetry_cadence(0,1,&cadence);
    psx_video_telemetry_cadence(0,0,&cadence);
    CHECK(cadence.valid && !cadence.count);
    psx_video_telemetry_present_at(1,100000);
    psx_video_telemetry_cadence(0,0,&cadence);
    CHECK(cadence.valid && !cadence.count); // producer reset re-anchors timing
    std::atomic<bool> cadence_done{false};
    std::atomic<bool> cadence_consistent{true};
    std::thread presenter([&] {
        for(uint64_t i=1;i<=20000;++i) psx_video_telemetry_present_at(1,100000+i*10000);
        cadence_done=true;
    });
    auto reader=[&](bool resets) {
        unsigned reads=0;
        do {
            PsxPresentCadence c{};
            psx_video_telemetry_cadence(0,resets && (++reads%17==0),&c);
            if(c.valid && c.count && (c.mean_us!=10000 || c.max_us!=10000 || c.hitches!=0))
                cadence_consistent=false;
        }while(!cadence_done.load());
    };
    std::thread other_reader([&]{reader(true);});
    reader(false);
    presenter.join();other_reader.join();
    CHECK(cadence_consistent.load());
    psx_video_telemetry_snapshot(nullptr);
    return 0;
}
