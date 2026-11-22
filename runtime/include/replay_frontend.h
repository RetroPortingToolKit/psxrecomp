#ifndef PSX_REPLAY_FRONTEND_H
#define PSX_REPLAY_FRONTEND_H
/* The replay frontend of one game session: which replay to play or record,
 * started at the session's first vblank, and the exit a scripted run takes
 * when it ends (replay_session.h does the recording and playback).
 *
 * A session is one boot of the game: a reboot or a rematch ends it
 * (replay_frontend_end) and the next one starts from scratch
 * (replay_frontend_start), so a boot recording re-arms at the new session's
 * first vblank and the Record Replays choice is read again. A playback named
 * by the command line or the environment plays in the first session only. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ReplayFrontendConfig {
    const char *play_path;     /* replay to play (--replay, PSX_REPLAY_FILE); NULL for none */
    const char *record_file;   /* PSX_REPLAY_RECORD_FILE; NULL for the default path */
    const char *verdict_path;  /* PSX_REPLAY_VERDICT; NULL for none */
    int exit_at_end;           /* PSX_REPLAY_EXIT_AT_END=1 */
    int fast_play;             /* PSX_REPLAY_FAST=1, --replay-fast */
    int record_boot;           /* PSX_REPLAY_RECORD_BOOT: -1 unset, 0 off, 1 on */
    int record_mod;            /* the Record Replays mod is enabled this session */
    long long record_at;       /* PSX_REPLAY_RECORD_AT; -1 for none */
    long long record_frames;   /* PSX_REPLAY_RECORD_FRAMES; 0 for no limit */
} ReplayFrontendConfig;

/* 1 when a session with `cfg` records from power-on: PSX_REPLAY_RECORD_BOOT=1,
 * or the mod unless PSX_REPLAY_RECORD_BOOT=0, an anchored recording or a
 * playback is asked for. */
int replay_frontend_wants_boot_record(const ReplayFrontendConfig *cfg);

/* Starts a session; the next replay_frontend_boundary is its vblank 0. */
void replay_frontend_start(const ReplayFrontendConfig *cfg);
/* 1 between replay_frontend_start and replay_frontend_end. */
int replay_frontend_started(void);
/* This session's vblank count before the current one (UINT64_MAX before
 * the first): 0 at its first vblank. */
uint64_t replay_frontend_vblank(void);

/* The P1 state the host would deliver this frame (sticks LX,LY,RX,RY). */
typedef void (*ReplayLiveP1)(void *ctx, uint16_t *buttons, uint8_t sticks[4]);

/* Once per guest vblank, before P1 is sampled; live_p1 is called only while
 * a replay runs. Sets *deliver to 1 when P1 must take *out_buttons and
 * out_sticks instead. Returns -1 to go on, or the process exit status a
 * scripted run (exit_at_end) takes now. */
int replay_frontend_boundary(ReplayLiveP1 live_p1, void *ctx, uint16_t *out_buttons, uint8_t out_sticks[4],
                             int *deliver);

/* Session end, after replay_session_shutdown: restores fast-forward.
 * Returns -1, or the exit status of a scripted run whose replay the end
 * stopped. */
int replay_frontend_end(void);

/* ---- Host hooks (main.cpp; stubs in the unit test) ---- */
/* <save root>/replays/<serial>-boot-<UTC time>.psxrpl in out; 0 when there
 * is no save folder. */
int replay_host_boot_path(char *out, size_t cap);
/* Latches fast-forward on (1) for a playback, or back to the player's
 * choice (0). */
void replay_host_fast_forward(int on);

#ifdef __cplusplus
}
#endif
#endif
