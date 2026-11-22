/* Replay frontend: one game session's replay requests (replay_frontend.h). */
#include "replay_frontend.h"

#include <stdio.h>
#include <string.h>

#include "replay_session.h"

typedef struct {
    int started, exit_at_end, fast_play, record_boot, record_boot_quiet, fast_on;
    const char *play_path, *record_file;
    long long record_at, record_frames, recorded;
    char record_path[1024];
    uint64_t vblanks;
    ReplayState previous;
} Frontend;

static Frontend s_fe;
/* A playback named at startup has played (or been refused) in an earlier
 * session of this process. */
static int s_play_consumed;

int replay_frontend_wants_boot_record(const ReplayFrontendConfig *cfg)
{
    const int anchored = cfg->record_file && cfg->record_at >= 0;
    if (cfg->play_path && !s_play_consumed) return 0;
    if (cfg->record_boot == 1) return 1;
    return cfg->record_mod && cfg->record_boot != 0 && !anchored;
}

void replay_frontend_start(const ReplayFrontendConfig *cfg)
{
    const int boot = replay_frontend_wants_boot_record(cfg);
    memset(&s_fe, 0, sizeof s_fe);
    s_fe.started = 1;
    s_fe.play_path = !s_play_consumed && cfg->play_path && cfg->play_path[0] ? cfg->play_path : NULL;
    s_fe.record_file = cfg->record_file && cfg->record_file[0] ? cfg->record_file : NULL;
    s_fe.exit_at_end = cfg->exit_at_end;
    s_fe.fast_play = cfg->fast_play;
    s_fe.record_at = cfg->record_at;
    s_fe.record_frames = cfg->record_frames;
    s_fe.record_boot = boot;
    /* Only the mod asked: a session that cannot record says so in the log. */
    s_fe.record_boot_quiet = boot && cfg->record_boot != 1;
    s_fe.previous = REPLAY_IDLE;
    if (cfg->verdict_path && cfg->verdict_path[0]) replay_session_set_verdict_path(cfg->verdict_path);
}

int replay_frontend_started(void) { return s_fe.started; }

uint64_t replay_frontend_vblank(void) { return s_fe.started && s_fe.vblanks ? s_fe.vblanks - 1 : UINT64_MAX; }

static int end_status(ReplayState previous)
{
    if (previous == REPLAY_RECORDING || previous == REPLAY_ARMING) {
        FILE *f = s_fe.record_path[0] ? fopen(s_fe.record_path, "rb") : NULL;
        if (f) fclose(f);
        return f ? 0 : 5;
    }
    switch (replay_session_last_result()) {
    case REPLAY_RESULT_IN_SYNC: return 0;
    case REPLAY_RESULT_OUT_OF_SYNC: return 3;
    default: return 4;
    }
}

static int start_boot_record(void)
{
    char why[160] = "";
    if (s_fe.record_boot_quiet && !replay_host_can_record(why, sizeof why)) {
        fprintf(stdout, "replay: not recording this session: %s\n", why);
        fflush(stdout);
        return -1;
    }
    if (s_fe.record_file) snprintf(s_fe.record_path, sizeof s_fe.record_path, "%s", s_fe.record_file);
    else if (!replay_host_boot_path(s_fe.record_path, sizeof s_fe.record_path)) {
        s_fe.record_path[0] = 0;
        replay_host_osd("Replay not recorded: no save folder for this game", 2600);
    }
    if (!s_fe.record_path[0] || !replay_session_record_power_on(s_fe.record_path)) {
        s_fe.record_path[0] = 0;
        if (s_fe.exit_at_end) return 5;
    }
    return -1;
}

int replay_frontend_boundary(ReplayLiveP1 live_p1, void *ctx, uint16_t *out_buttons, uint8_t out_sticks[4],
                             int *deliver)
{
    uint16_t live_buttons = 0xFFFFu;
    uint8_t live_sticks[4] = {0x80, 0x80, 0x80, 0x80};
    uint64_t vb;
    ReplayState now;
    int status = -1;
    *deliver = 0;
    if (!s_fe.started) return -1;
    vb = s_fe.vblanks++;
    if (s_fe.play_path) {
        const char *path = s_fe.play_path;
        s_fe.play_path = NULL;
        s_play_consumed = 1;
        if (!replay_session_play_file(path) && s_fe.exit_at_end) return 4;
    }
    if (s_fe.record_boot && vb == 0) {
        if ((status = start_boot_record()) >= 0) return status;
    } else if (!s_fe.record_boot && s_fe.record_file && s_fe.record_at >= 0 && (long long)vb == s_fe.record_at) {
        snprintf(s_fe.record_path, sizeof s_fe.record_path, "%s", s_fe.record_file);
        if (!replay_session_record_to(s_fe.record_file)) {
            s_fe.record_path[0] = 0;
            if (s_fe.exit_at_end) return 5;
        }
    }
    if (replay_session_state() == REPLAY_IDLE && s_fe.previous == REPLAY_IDLE) return -1;
    if (live_p1) live_p1(ctx, &live_buttons, live_sticks);
    if (replay_session_boundary(live_buttons, live_sticks, out_buttons, out_sticks)) {
        *deliver = 1;
        if (replay_session_state() == REPLAY_RECORDING && s_fe.record_frames > 0 &&
            ++s_fe.recorded == s_fe.record_frames)
            replay_session_stop_record();   /* ends on the next boundary */
    }
    now = replay_session_state();
    if (s_fe.fast_play) {   /* latched fast-forward only while a replay plays */
        const int playing = now == REPLAY_LOADING || now == REPLAY_PLAYING;
        if (playing != s_fe.fast_on) {
            s_fe.fast_on = playing;
            replay_host_fast_forward(playing);
        }
    }
    if (s_fe.exit_at_end && s_fe.previous != REPLAY_IDLE && now == REPLAY_IDLE) status = end_status(s_fe.previous);
    s_fe.previous = now;
    return status;
}

int replay_frontend_end(void)
{
    int status = -1;
    if (!s_fe.started) return -1;
    if (s_fe.fast_on) replay_host_fast_forward(0);
    if (s_fe.exit_at_end && s_fe.previous != REPLAY_IDLE) status = end_status(s_fe.previous);
    memset(&s_fe, 0, sizeof s_fe);
    return status;
}
