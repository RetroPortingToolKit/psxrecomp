/* Replay frontend (replay_frontend.h): the Record Replays default choice,
 * a reboot re-arming the boot recording, a startup playback playing in the
 * first session only, and what a session end puts back. The replay session
 * is a stub that records the calls. */
#include "replay_frontend.h"

#include <stdio.h>
#include <string.h>

#include "replay_session.h"

static int s_failures;
#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);    \
            s_failures++;                                                             \
        }                                                                             \
    } while (0)

/* ---- replay_session stub ---- */
static ReplayState s_state = REPLAY_IDLE;
static ReplayResult s_result = REPLAY_RESULT_NONE;
static int s_play_calls, s_play_ok = 1, s_record_calls, s_osd_calls, s_can_record = 1, s_fast_on;
static char s_last_record[256], s_verdict[256];

ReplayState replay_session_state(void) { return s_state; }
ReplayResult replay_session_last_result(void) { return s_result; }
int replay_session_play_file(const char *path)
{
    (void)path;
    s_play_calls++;
    if (s_play_ok) s_state = REPLAY_PLAYING;
    return s_play_ok;
}
int replay_session_record_to(const char *path) { (void)path; return 0; }
int replay_session_record_power_on(const char *path)
{
    snprintf(s_last_record, sizeof s_last_record, "%s", path);
    s_record_calls++;
    s_state = REPLAY_RECORDING;
    return 1;
}
void replay_session_stop_record(void) {}
int replay_session_boundary(uint16_t live_buttons, const uint8_t live_sticks[4], uint16_t *out_buttons,
                            uint8_t out_sticks[4])
{
    (void)live_sticks;
    *out_buttons = live_buttons;
    memset(out_sticks, 0x80, 4);
    return s_state != REPLAY_IDLE;
}
void replay_session_set_verdict_path(const char *path) { snprintf(s_verdict, sizeof s_verdict, "%s", path); }
void replay_session_shutdown(void) { s_state = REPLAY_IDLE; }
void replay_host_osd(const char *text, int ms) { (void)text; (void)ms; s_osd_calls++; }
int replay_host_can_record(char *why, size_t cap)
{
    if (!s_can_record) snprintf(why, cap, "netplay is active");
    return s_can_record;
}
int replay_host_boot_path(char *out, size_t cap)
{
    snprintf(out, cap, "replays/GAME-boot.psxrpl");
    return 1;
}
void replay_host_fast_forward(int on) { s_fast_on = on; }

static void reset_stub(void)
{
    s_state = REPLAY_IDLE;
    s_result = REPLAY_RESULT_NONE;
    s_play_calls = s_record_calls = s_osd_calls = s_fast_on = 0;
    s_play_ok = s_can_record = 1;
    s_last_record[0] = s_verdict[0] = 0;
}

static ReplayFrontendConfig config(int record_mod)
{
    ReplayFrontendConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.record_boot = -1;
    cfg.record_mod = record_mod;
    cfg.record_at = -1;
    return cfg;
}

/* One vblank; returns the exit status the frontend asks for (-1: none). */
static int vblank(void)
{
    uint16_t b;
    uint8_t s[4];
    int deliver;
    return replay_frontend_boundary(NULL, NULL, &b, s, &deliver);
}

/* A session end as main.cpp does it: the replay session first. */
static int end_session(void)
{
    replay_session_shutdown();
    return replay_frontend_end();
}

int main(void)
{
    ReplayFrontendConfig cfg;

    /* The default choice: the mod is on, nothing else asked. */
    cfg = config(1);
    CHECK(replay_frontend_wants_boot_record(&cfg));
    cfg.record_boot = 0;   /* PSX_REPLAY_RECORD_BOOT=0 turns it off for a run */
    CHECK(!replay_frontend_wants_boot_record(&cfg));
    cfg = config(0);       /* the player turned the mod off */
    CHECK(!replay_frontend_wants_boot_record(&cfg));
    cfg.record_boot = 1;   /* the environment still records */
    CHECK(replay_frontend_wants_boot_record(&cfg));
    cfg = config(1);
    cfg.record_file = "anchored.psxrpl";
    cfg.record_at = 300;   /* an anchored recording replaces the boot one */
    CHECK(!replay_frontend_wants_boot_record(&cfg));

    /* Mod on: the first session records from its vblank 0, into the save folder. */
    reset_stub();
    cfg = config(1);
    CHECK(!replay_frontend_started());
    replay_frontend_start(&cfg);
    CHECK(replay_frontend_started() && replay_frontend_vblank() == UINT64_MAX);
    CHECK(vblank() == -1);
    CHECK(s_record_calls == 1 && !strcmp(s_last_record, "replays/GAME-boot.psxrpl"));
    for (int i = 0; i < 10; ++i) vblank();
    CHECK(replay_frontend_vblank() == 10 && s_record_calls == 1);

    /* A reboot ends the session and its recording; the next session records again. */
    CHECK(end_session() == -1);
    CHECK(!replay_frontend_started() && s_state == REPLAY_IDLE);
    replay_frontend_start(&cfg);
    CHECK(vblank() == -1);
    CHECK(replay_frontend_vblank() == 0 && s_record_calls == 2);
    CHECK(end_session() == -1);

    /* The choice is read again per session: the mod turned off before the reboot. */
    cfg = config(0);
    replay_frontend_start(&cfg);
    vblank();
    CHECK(s_record_calls == 2 && s_state == REPLAY_IDLE);
    CHECK(end_session() == -1);

    /* A session that cannot record (netplay) under the mod's default: no
     * recording and no OSD message. */
    reset_stub();
    s_can_record = 0;
    cfg = config(1);
    replay_frontend_start(&cfg);
    vblank();
    CHECK(s_record_calls == 0 && s_osd_calls == 0);
    CHECK(end_session() == -1);

    /* A startup playback plays in the first session, with fast-forward and a
     * verdict; it is not recorded. The reboot that stops it restores
     * fast-forward and gives the scripted run its exit (4: not played to the end). */
    reset_stub();
    cfg = config(1);
    cfg.play_path = "run.psxrpl";
    cfg.verdict_path = "verdict.json";
    cfg.fast_play = 1;
    cfg.exit_at_end = 1;
    replay_frontend_start(&cfg);
    CHECK(vblank() == -1);
    CHECK(s_play_calls == 1 && s_record_calls == 0 && !strcmp(s_verdict, "verdict.json"));
    CHECK(s_fast_on == 1);
    CHECK(end_session() == 4);
    CHECK(s_fast_on == 0);

    /* The next session does not play the startup replay again; the mod
     * records it instead. */
    replay_frontend_start(&cfg);
    CHECK(replay_frontend_wants_boot_record(&cfg));
    CHECK(vblank() == -1);
    CHECK(s_play_calls == 1 && s_record_calls == 1);
    /* A scripted recording that a reboot ends exits 5 when no file exists. */
    CHECK(end_session() == 5);

    /* A playback that ends on its own: the scripted run exits at the next vblank. */
    reset_stub();
    cfg = config(0);
    cfg.exit_at_end = 1;
    replay_frontend_start(&cfg);
    s_state = REPLAY_PLAYING;   /* as if started by the player */
    CHECK(vblank() == -1);
    s_state = REPLAY_IDLE;
    s_result = REPLAY_RESULT_OUT_OF_SYNC;
    CHECK(vblank() == 3);
    CHECK(end_session() == -1);

    if (s_failures) {
        fprintf(stderr, "replay_frontend_test: %d failure(s)\n", s_failures);
        return 1;
    }
    puts("replay_frontend_test: ok");
    return 0;
}
