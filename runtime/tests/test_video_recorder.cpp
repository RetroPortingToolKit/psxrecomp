/* video_recorder: option handling, helpers and the start/stop state machine
 * (no ffmpeg needed). The ffmpeg end-to-end pipeline is
 * test_video_recorder_e2e.cpp. Build/run: ctest -R video_recorder_test */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "video_recorder.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); failures++; } \
} while (0)

static void set_env(const char *k, const char *v)
{
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static bool contains(const char *hay, const char *needle) { return std::strstr(hay, needle) != nullptr; }

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    char err[512];

    /* ---- options ---- */
    VrecOptions o;
    vrec_options_default(&o);
    CHECK(std::strcmp(o.encoder, "libx264") == 0 && o.crf == 18 && o.audio == 1 &&
          o.queue_frames == 8 && o.fps == 0.0, "defaults");
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "path is required"), "path required");
    std::snprintf(o.path, sizeof o.path, "%s", "clip.mkv");
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, ".mp4"), "mp4 only");
    std::snprintf(o.path, sizeof o.path, "%s", "clip.mp4");
    CHECK(vrec_options_validate(&o, err, sizeof err), "minimal valid");
    o.crf = 52;
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "crf"), "crf range");
    o.crf = 18; o.fps = -1;
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "fps"), "fps negative");
    o.fps = 0.5;
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "fps"), "fps below 1");
    o.fps = 59.94;
    CHECK(vrec_options_validate(&o, err, sizeof err), "fps 59.94");
    o.fps = 0; o.width = 1920;
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "both"), "width needs height");
    o.width = 1921; o.height = 1079;
    CHECK(vrec_options_validate(&o, err, sizeof err) && o.width == 1922 && o.height == 1080,
          "explicit size rounded up to even (yuv420p)");
    o.width = o.height = 0;
    std::snprintf(o.encoder, sizeof o.encoder, "%s", "libx264;calc");
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "encoder"), "encoder name sanitised");
    std::snprintf(o.encoder, sizeof o.encoder, "%s", "h264_nvenc");
    std::snprintf(o.preset, sizeof o.preset, "%s", "p5 -y");
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "preset"), "preset sanitised");
    o.preset[0] = 0;
    o.queue_frames = 1;
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "queue_frames"), "queue min");
    o.queue_frames = 65;
    CHECK(!vrec_options_validate(&o, err, sizeof err) && contains(err, "queue_frames"), "queue max");
    o.queue_frames = 0;
    CHECK(vrec_options_validate(&o, err, sizeof err) && o.queue_frames == 8, "queue 0 = default");
    o.audio = 7;
    CHECK(vrec_options_validate(&o, err, sizeof err) && o.audio == 1, "audio normalised");

    /* ---- helpers ---- */
    char args[16][128];
    CHECK(vrec_split_args("  -tune  film\t-g 120 ", args, 16) == 4 &&
          std::strcmp(args[0], "-tune") == 0 && std::strcmp(args[3], "120") == 0, "split args");
    CHECK(vrec_split_args("", args, 16) == 0, "split empty");
    CHECK(vrec_split_args("a b c", args, 2) == -1, "split overflow");

    int x, y, w, h;
    vrec_fit_rect(1920, 1080, 1920, 1080, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 1920 && h == 1080, "fit identity");
    vrec_fit_rect(1440, 1080, 1920, 1080, &x, &y, &w, &h);   /* 4:3 into 16:9: pillarbox */
    CHECK(x == 240 && y == 0 && w == 1440 && h == 1080, "fit pillarbox");
    vrec_fit_rect(1920, 1080, 1440, 1080, &x, &y, &w, &h);   /* 16:9 into 4:3: letterbox */
    CHECK(x == 0 && y == 135 && w == 1440 && h == 810, "fit letterbox");
    vrec_fit_rect(640, 480, 1920, 1080, &x, &y, &w, &h);     /* upscale */
    CHECK(x == 240 && w == 1440 && h == 1080, "fit upscale");

    int64_t num, den;
    vrec_rationalize_fps(60.0, &num, &den);
    CHECK(num == 60 && den == 1, "60/1");
    vrec_rationalize_fps(59.9995, &num, &den);
    CHECK(num == 60 && den == 1, "near 60 snaps to 60/1");
    vrec_rationalize_fps(60000.0 / 1001.0, &num, &den);
    CHECK(num == 60000 && den == 1001, "59.94 = 60000/1001");
    vrec_rationalize_fps(120.0 * 60.0 / 59.94, &num, &den);   /* interp 120 from 59.94 source, 60 Hz guest */
    CHECK(den == 1000 && std::llabs(num - 120120) <= 1, "interp ratio rate in millihertz");
    vrec_rationalize_fps(50.0, &num, &den);
    CHECK(num == 50 && den == 1, "PAL 50/1");

    /* ---- state machine (no encoder involved) ---- */
    VrecStatus st;
    video_recorder_get_status(&st);
    CHECK(st.state == VREC_IDLE && std::strcmp(video_recorder_state_name(st.state), "idle") == 0, "idle");
    CHECK(!video_recorder_stop(err, sizeof err) && contains(err, "not recording"), "stop while idle");
    CHECK(video_recorder_present_poll(nullptr) == VREC_PRESENT_IDLE, "present idle");

    VrecOptions s;
    vrec_options_default(&s);
    std::snprintf(s.path, sizeof s.path, "%s/vrec_state.mp4", dir.c_str());
    video_recorder_set_backend(VREC_BACKEND_NONE);
    CHECK(!video_recorder_start(&s, err, sizeof err) && contains(err, "--headless") &&
          contains(err, "--offscreen-window"), "headless refused with guidance");
    video_recorder_set_backend(VREC_BACKEND_VULKAN);
    CHECK(!video_recorder_start(&s, err, sizeof err) && contains(err, "Vulkan"), "vulkan refused explicitly");
    video_recorder_set_backend(VREC_BACKEND_SOFTWARE);
#ifdef PSX_NO_DEBUG_TOOLS
    CHECK(!video_recorder_start(&s, err, sizeof err) && contains(err, "PSX_DEBUG_TOOLS=ON"),
          "production audio refusal explains the missing PCM tap");
    s.audio = 0;
#endif
    std::snprintf(s.ffmpeg, sizeof s.ffmpeg, "%s/definitely-not-here/ffmpeg.exe", dir.c_str());
    CHECK(!video_recorder_start(&s, err, sizeof err) && contains(err, "ffmpeg not found at"),
          "missing explicit ffmpeg is a clear error");
    s.ffmpeg[0] = 0;
    set_env("PSX_FFMPEG", (dir + "/nope-ffmpeg").c_str());
    CHECK(!video_recorder_start(&s, err, sizeof err) && contains(err, "PSX_FFMPEG"),
          "bad PSX_FFMPEG is a clear error");
    set_env("PSX_FFMPEG", nullptr);

    /* PATH lookup finds a file named ffmpeg; with an empty PATH nothing. */
    const std::string fake_dir = dir;
#if defined(_WIN32)
    const std::string fake = fake_dir + "/ffmpeg.exe";
#else
    const std::string fake = fake_dir + "/ffmpeg";
#endif
    {
        FILE *f = std::fopen(fake.c_str(), "wb");
        CHECK(f != nullptr, "create fake ffmpeg");
        if (f) std::fclose(f);
    }
    const char *old_path = std::getenv("PATH");
    const std::string saved = old_path ? old_path : "";
    set_env("PATH", fake_dir.c_str());
    char found[VREC_PATH_MAX];
    CHECK(video_recorder_find_ffmpeg(nullptr, found, sizeof found) && contains(found, "ffmpeg"),
          "PATH lookup");
    set_env("PATH", (fake_dir + "/empty-subdir-that-does-not-exist").c_str());
    CHECK(!video_recorder_find_ffmpeg(nullptr, found, sizeof found), "PATH miss");
    CHECK(!video_recorder_start(&s, err, sizeof err) && contains(err, "not found on PATH"),
          "start reports ffmpeg missing from PATH");

    /* With the fake on PATH, start succeeds (spawn happens lazily at the first
     * frame), a second start is refused, and stop with no presents finishes
     * in error "no frames" without ever spawning anything. */
    set_env("PATH", fake_dir.c_str());
    CHECK(video_recorder_start(&s, err, sizeof err), "start");
    video_recorder_get_status(&st);
    CHECK(st.state == VREC_RECORDING && st.session >= 1, "recording");
    CHECK(!video_recorder_start(&s, err, sizeof err) && contains(err, "already recording"), "double start refused");
    CHECK(video_recorder_stop(err, sizeof err), "stop");
    CHECK(video_recorder_present_poll(nullptr) == VREC_PRESENT_IDLE, "no flush needed with no presents");
    CHECK(video_recorder_wait(10000) == VREC_ERROR, "finishes");
    video_recorder_get_status(&st);
    CHECK(contains(st.error, "no frames"), "no-frames error");
    CHECK(!video_recorder_stop(err, sizeof err), "stop after error refused");

    /* Session/present protocol: a capture is counted, a submit for an older
     * session is ignored, FLUSH comes once stop is requested. */
    CHECK(video_recorder_start(&s, err, sizeof err), "restart after error");
    uint32_t sess = 0;
    CHECK(video_recorder_present_poll(&sess) == VREC_PRESENT_CAPTURE, "capture");
    video_recorder_submit_frame(sess - 1, "\0\0\0\0", 1, 1, 4, 0, 0);   /* stale: ignored */
    video_recorder_get_status(&st);
    CHECK(st.presents == 1 && st.frames_written == 0 && st.width == 0, "stale session ignored");
    video_recorder_note_lost_frame(sess);
    CHECK(video_recorder_stop(err, sizeof err), "stop with a pending capture");
    uint32_t sess2 = 0;
    CHECK(video_recorder_flush_wanted(&sess2) && sess2 == sess, "flush wanted");
    CHECK(video_recorder_present_poll(&sess2) == VREC_PRESENT_FLUSH, "flush");
    video_recorder_flush_done(sess);
    CHECK(video_recorder_wait(10000) == VREC_ERROR, "finishes (no frames)");
    video_recorder_get_status(&st);
    CHECK(st.dropped == 1 && st.unflushed == 0, "lost frame counted as dropped");
    std::remove(fake.c_str());
    std::remove((dir + "/vrec_state.mp4.ffmpeg.log").c_str());
    set_env("PATH", saved.c_str());

    video_recorder_shutdown();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("video_recorder_test: ok\n");
    return 0;
}
