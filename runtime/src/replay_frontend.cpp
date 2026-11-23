/* Per-game-session launch policy; input and file ownership stay in replay_session. */
#include "replay_frontend.h"
#include "replay_session.h"
#include <filesystem>
#include <string>

namespace {
bool configured, boot, exit_at_end, fast;
int fast_saved = -1;
int64_t at = -1, limit, recorded;
uint64_t frame = UINT64_MAX;
ReplayState previous = REPLAY_IDLE;
std::string play_path, record_file, record_path;

void restore_fast() {
    if (fast_saved >= 0) replay_host_set_fast_forward(fast_saved);
    fast_saved = -1;
}
}

extern "C" int replay_frontend_configured(void) { return configured; }
extern "C" uint64_t replay_frontend_frame(void) { return frame; }

extern "C" void replay_frontend_shutdown(void) {
    replay_session_shutdown();
    restore_fast();
    /* A mid-session disc change must not pretend to be a new cold boot. */
    configured = true;
    boot = false;
    play_path.clear(); record_file.clear(); record_path.clear();
    previous = REPLAY_IDLE;
    replay_session_set_verdict_path(nullptr);
}

extern "C" void replay_frontend_reset(void) {
    replay_frontend_shutdown();
    configured = false;
    frame = UINT64_MAX;
    recorded = limit = 0;
    at = -1;
    exit_at_end = fast = false;
}

extern "C" void replay_frontend_configure(const ReplayFrontendConfig *c) {
    if (configured || !c) return;
    configured = true;
    play_path = c->play_path ? c->play_path : "";
    record_file = c->record_path ? c->record_path : "";
    /* A specific timed recording must remain usable when the ordinary Mods
     * recording default is On. It names its own anchored starting point. */
    boot = play_path.empty() && c->record_boot && (record_file.empty() || c->record_at < 0);
    at = c->record_at; limit = c->record_frames;
    exit_at_end = c->exit_at_end != 0; fast = c->fast_play != 0;
    replay_session_set_verdict_path(c->verdict_path);
}

extern "C" int replay_frontend_prepare(uint16_t live, const uint8_t sticks[4], uint16_t *b, uint8_t out[4]) {
    if (!configured) return 0;
    ++frame;
    if (!play_path.empty()) {
        const std::string path = play_path;
        play_path.clear();
        if (!replay_session_play_file(path.c_str()) && exit_at_end) replay_host_exit(4, "replay_not_played");
    }
    if (boot && frame == 0) {
        char path[1024] = "";
        if (record_file.empty()) replay_host_boot_path(path, sizeof path);
        record_path = record_file.empty() ? path : record_file;
        if (record_path.empty()) replay_host_osd("Replay not recorded: no save folder for this game", 2600);
        if (record_path.empty() || !replay_session_record_power_on(record_path.c_str())) {
            record_path.clear();
            if (exit_at_end) replay_host_exit(5, "replay_not_recorded");
        }
    } else if (!boot && !record_file.empty() && at >= 0 && frame == static_cast<uint64_t>(at)) {
        record_path = record_file;
        if (!replay_session_record_to(record_file.c_str())) {
            record_path.clear();
            if (exit_at_end) replay_host_exit(5, "replay_not_recorded");
        }
    }
    const ReplayState state = replay_session_state();
    if (state == REPLAY_ARMING || state == REPLAY_RECORDING) {
        if (previous == REPLAY_IDLE) previous = state;
        replay_session_prepare_record_boundary();
        return 0;
    }
    if (state != REPLAY_LOADING && state != REPLAY_PLAYING) return 0;
    return replay_session_boundary(live, sticks, b, out);
}

extern "C" void replay_frontend_observe(uint16_t buttons, const uint8_t sticks[4]) {
    if (!configured) return;
    ReplayState state = replay_session_state();
    if (state == REPLAY_ARMING || state == REPLAY_RECORDING) {
        uint16_t unused; uint8_t axes[4];
        replay_session_boundary(buttons, sticks, &unused, axes);
        if (replay_session_state() == REPLAY_RECORDING && limit > 0 && ++recorded == limit)
            replay_session_stop_record();
    }
    state = replay_session_state();
    if (fast) {
        const bool playing = state == REPLAY_LOADING || state == REPLAY_PLAYING;
        if (playing && fast_saved < 0) {
            fast_saved = replay_host_fast_forward();
            replay_host_set_fast_forward(1);
        } else if (!playing) restore_fast();
    }
    if (exit_at_end && previous != REPLAY_IDLE && state == REPLAY_IDLE) {
        if (previous == REPLAY_RECORDING || previous == REPLAY_ARMING) {
            std::error_code ec;
            const bool written = !record_path.empty() && std::filesystem::exists(record_path, ec);
            replay_host_exit(written ? 0 : 5, "replay_end");
        } else {
            const ReplayResult result = replay_session_last_result();
            replay_host_exit(result == REPLAY_RESULT_IN_SYNC ? 0 : result == REPLAY_RESULT_OUT_OF_SYNC ? 3 : 4, "replay_end");
        }
    }
    previous = state;
}
