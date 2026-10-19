/* video_recorder end-to-end through a real ffmpeg: synthetic presents on a
 * synthetic guest clock plus SPU_OUT tap audio, then decode the MP4 and check
 * frame count, rate, size, the fitted (resized) frame, and A/V sync — a white
 * frame and an audio click placed at the same guest time must come out at the
 * same media time. Skips (77) when ffmpeg is not on PATH / $PSX_FFMPEG.
 * Build/run: ctest -R video_recorder_e2e_test */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio_trace.h"
#include "video_recorder.h"

#if defined(_WIN32)
#  define popen _popen
#  define pclose _pclose
#endif

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); failures++; } \
} while (0)

static std::vector<uint8_t> run_capture(const std::string &cmd)
{
    std::vector<uint8_t> out;
#if defined(_WIN32)
    /* cmd.exe /c strips the outer quote pair of a line that starts with one. */
    const std::string line = "\"" + cmd + "\"";
#else
    const std::string line = cmd;
#endif
    FILE *p = popen(line.c_str(), "rb");
    if (!p) return out;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.insert(out.end(), buf, buf + n);
    pclose(p);
    return out;
}

static std::string q(const std::string &s) { return "\"" + s + "\""; }

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    char ff[VREC_PATH_MAX];
    if (!video_recorder_find_ffmpeg(nullptr, ff, sizeof ff)) {
        std::printf("video_recorder_e2e_test: ffmpeg not found, skipping\n");
        return 77;
    }
    const std::string out = dir + "/vrec_e2e.mp4";
    std::remove(out.c_str());

    const int W = 320, H = 240, N = 120, CLICK = 60;
    const uint64_t VBLANK = 564480;           /* NTSC guest cycles per vblank */
    const int SPF = 735;                       /* 44100 / 60 */
    audio_trace_init();
    audio_trace_note_spu_clock(0, 0);
    /* Audio that predates the recording must not leak into it. */
    std::vector<int16_t> pre(4410 * 2, 9000);

    video_recorder_set_backend(VREC_BACKEND_OPENGL);
    VrecOptions o;
    vrec_options_default(&o);
    std::snprintf(o.path, sizeof o.path, "%s", out.c_str());
    o.crf = 10;
    o.queue_frames = 2;   /* exercise backpressure */
    char err[512];
    if (!video_recorder_start(&o, err, sizeof err)) {
        std::fprintf(stderr, "start failed: %s\n", err);
        return 1;
    }

    /* Guest timeline starts at cycle C0; the SPU had rendered 4410 frames
     * before it (0.1 s), anchored so that frame 4410 == cycle C0. */
    const uint64_t C0 = 4410ull * 768ull;
    audio_trace_pcm(AUDIO_TAP_SPU_OUT, pre.data(), 4410);
    audio_trace_note_spu_clock(4410, C0);

    std::vector<uint8_t> frame((size_t)W * H * 4);
    std::vector<uint8_t> small_frame((size_t)160 * 160 * 4, 0x80);
    std::vector<int16_t> pcm((size_t)SPF * 2);
    uint32_t sess = 0;
    for (int i = 0; i < N; i++) {
        const int action = video_recorder_present_poll(&sess);
        CHECK(action == VREC_PRESENT_CAPTURE, "capture each present");
        const uint8_t v = (i == CLICK) ? 255 : 16;
        std::memset(frame.data(), v, frame.size());
        if (i == 100) {
            /* One present at another size: fitted into the fixed output. */
            video_recorder_submit_frame(sess, small_frame.data(), 160, 160, 160 * 4, 1,
                                        C0 + (uint64_t)i * VBLANK);
        } else {
            video_recorder_submit_frame(sess, frame.data(), W, H, W * 4, i & 1,
                                        C0 + (uint64_t)i * VBLANK);
        }
        /* The SPU renders this vblank's audio after the present. */
        for (int k = 0; k < SPF; k++) {
            int16_t s = (int16_t)((k % 100) < 50 ? 300 : -300);
            if (i == CLICK && k < 32) s = 30000;   /* click at the white frame */
            pcm[(size_t)k * 2] = pcm[(size_t)k * 2 + 1] = s;
        }
        audio_trace_pcm(AUDIO_TAP_SPU_OUT, pcm.data(), SPF);
        audio_trace_note_spu_clock(4410 + (uint64_t)(i + 1) * SPF, C0 + (uint64_t)(i + 1) * VBLANK);
    }
    CHECK(video_recorder_stop(err, sizeof err), "stop");
    CHECK(video_recorder_present_poll(&sess) == VREC_PRESENT_FLUSH, "flush requested");
    video_recorder_flush_done(sess);
    const VrecState end = video_recorder_wait(120000);
    VrecStatus st;
    video_recorder_get_status(&st);
    if (end != VREC_DONE) std::fprintf(stderr, "state %s: %s\n", video_recorder_state_name(end), st.error);
    CHECK(end == VREC_DONE, "done");
    CHECK(st.presents == (uint64_t)N && st.frames_written == (uint64_t)N, "frames == presents");
    CHECK(st.dropped == 0 && st.unflushed == 0, "no drops");
    CHECK(st.resized_frames == 1, "one fitted frame");
    CHECK(st.width == W && st.height == H, "output size = first frame");
    CHECK(std::fabs(st.fps - 60.0) < 1e-9, "measured 60 fps on the guest clock");
    CHECK(st.audio_frames == (uint64_t)N * SPF, "audio = video duration");
    CHECK(st.audio_anchor == 1 && st.audio_slip_frames == 0, "anchored, no slip");
    CHECK(st.max_av_skew_ms < 0.001, "regular cadence");
    CHECK(st.bytes_out > 0, "output size reported");
    std::printf("e2e: %dx%d fps=%.3f frames=%llu audio=%llu waits=%llu bytes=%llu\n",
                st.width, st.height, st.fps, (unsigned long long)st.frames_written,
                (unsigned long long)st.audio_frames,
                (unsigned long long)st.backpressure_waits, (unsigned long long)st.bytes_out);

    /* Decode: frames as 8x8 gray, audio as mono s16. */
    const std::string ffq = q(ff);
    std::vector<uint8_t> v = run_capture(ffq + " -v error -i " + q(out) +
        " -map 0:v:0 -vf scale=8:8,format=gray -f rawvideo -");
    CHECK(v.size() == (size_t)N * 64, "decoded frame count");
    int white = -1;
    for (int i = 0; i * 64 < (int)v.size(); i++) {
        int sum = 0;
        for (int k = 0; k < 64; k++) sum += v[(size_t)i * 64 + k];
        if (sum / 64 > 200) { white = i; break; }
    }
    CHECK(white == CLICK, "white frame at its present index");
    if (v.size() >= 101u * 64u) {
        /* Frame 100 (160x160 fitted into 320x240): pillarbox edges are black. */
        CHECK(v[100 * 64 + 3 * 8 + 0] < 48 && v[100 * 64 + 3 * 8 + 4] > 90, "fitted frame pillarboxed");
    }
    std::vector<uint8_t> a = run_capture(ffq + " -v error -i " + q(out) +
        " -map 0:a:0 -ac 1 -ar 44100 -f s16le -");
    const size_t samples = a.size() / 2;
    CHECK(samples >= (size_t)N * SPF - 2048 && samples <= (size_t)N * SPF + 2048, "audio length");
    long click = -1;
    for (size_t i = 0; i < samples; i++) {
        int16_t s = (int16_t)(a[i * 2] | (a[i * 2 + 1] << 8));
        if (s > 15000) { click = (long)i; break; }
    }
    const double click_t = click / 44100.0, frame_t = CLICK / 60.0;
    const double offset_ms = (click_t - frame_t) * 1000.0;
    std::printf("e2e: click sample %ld (%.3f s), white frame %d (%.3f s), A/V offset %.2f ms\n",
                click, click_t, white, frame_t, offset_ms);
    CHECK(click >= 0 && std::fabs(offset_ms) < 2.0, "A/V in sync (< 2 ms)");
    /* Pre-recording audio (constant 9000) must not appear at the start. */
    bool leaked = false;
    for (size_t i = 0; i < 400 && i < samples; i++) {
        int16_t s = (int16_t)(a[i * 2] | (a[i * 2 + 1] << 8));
        if (s > 5000) leaked = true;
    }
    CHECK(!leaked, "audio slice starts at the first present");

    if (!failures) {
        std::remove(out.c_str());
        std::remove((out + ".ffmpeg.log").c_str());
    }

    /* Retime path: the live encode runs at the nominal 60/1, but the presents
     * arrive once per PAL vblank (677376 cycles): the measured rate is 50/1
     * and the mux must retime the stream copy so 100 frames last 2.0 s. */
    {
        const std::string out2 = dir + "/vrec_e2e_pal.mp4";
        std::remove(out2.c_str());
        std::snprintf(o.path, sizeof o.path, "%s", out2.c_str());
        o.audio = 0;
        CHECK(video_recorder_start(&o, err, sizeof err), "start (PAL)");
        const uint64_t PAL = 677376;
        for (int i = 0; i < 100; i++) {
            CHECK(video_recorder_present_poll(&sess) == VREC_PRESENT_CAPTURE, "capture (PAL)");
            std::memset(frame.data(), (uint8_t)(i * 2), frame.size());
            video_recorder_submit_frame(sess, frame.data(), W, H, W * 4, 0, (uint64_t)i * PAL);
        }
        CHECK(video_recorder_stop(err, sizeof err), "stop (PAL)");
        CHECK(video_recorder_present_poll(&sess) == VREC_PRESENT_FLUSH, "flush (PAL)");
        video_recorder_flush_done(sess);
        CHECK(video_recorder_wait(120000) == VREC_DONE, "done (PAL)");
        video_recorder_get_status(&st);
        CHECK(std::fabs(st.fps - 50.0) < 1e-9 && st.frames_written == 100, "measured 50/1");
        std::string probe = ff;
        const size_t at = probe.rfind("ffmpeg");
        if (at != std::string::npos) probe.replace(at, 6, "ffprobe");
        FILE *pf = std::fopen(probe.c_str(), "rb");
        if (pf) {
            std::fclose(pf);
            std::vector<uint8_t> r = run_capture(q(probe) + " -v error -select_streams v:0 "
                "-count_frames -show_entries stream=r_frame_rate,nb_read_frames:format=duration "
                "-of default=nw=1 " + q(out2));
            std::string txt(r.begin(), r.end());
            std::printf("e2e PAL probe: %s\n", txt.c_str());
            CHECK(txt.find("r_frame_rate=50/1") != std::string::npos, "stream retimed to 50/1");
            CHECK(txt.find("nb_read_frames=100") != std::string::npos, "100 frames after retime");
            const size_t d = txt.find("duration=");
            CHECK(d != std::string::npos && std::fabs(std::atof(txt.c_str() + d + 9) - 2.0) < 0.03,
                  "duration 2.0 s");
        } else {
            std::printf("e2e: no ffprobe next to ffmpeg; retime checked via status only\n");
        }
        if (!failures) {
            std::remove(out2.c_str());
            std::remove((out2 + ".ffmpeg.log").c_str());
        }
    }
    video_recorder_shutdown();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("video_recorder_e2e_test: ok\n");
    return 0;
}
