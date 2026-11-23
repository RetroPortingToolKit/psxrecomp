#ifndef PSX_REPLAY_FRONTEND_H
#define PSX_REPLAY_FRONTEND_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ReplayFrontendConfig {
    const char *play_path, *verdict_path, *record_path;
    int record_boot, exit_at_end, fast_play;
    int64_t record_at, record_frames;
} ReplayFrontendConfig;

/* Cold-session policy. Shutdown (disc swap/exit) does not start another boot
 * recording; only reset followed by configure starts a new game session. */
void replay_frontend_reset(void);
void replay_frontend_shutdown(void);
int replay_frontend_configured(void);
void replay_frontend_configure(const ReplayFrontendConfig *config);
uint64_t replay_frontend_frame(void);
int replay_frontend_prepare(uint16_t live, const uint8_t sticks[4], uint16_t *buttons, uint8_t out_sticks[4]);
/* Actual SIO input, after all ordinary samplers, before guest execution. */
void replay_frontend_observe(uint16_t buttons, const uint8_t sticks[4]);

/* The visible, persisted host-only Mods choice. Plan reset disables the
 * request; the selected plugin activates it before cold-session configure. */
void replay_recording_request(int enabled);
int replay_recording_requested(void);

/* Existing main/frontend host owners. */
void replay_host_boot_path(char *out, size_t cap);
int replay_host_fast_forward(void);
void replay_host_set_fast_forward(int enabled);
void replay_host_exit(int status, const char *origin);
#ifdef __cplusplus
}
#endif
#endif
