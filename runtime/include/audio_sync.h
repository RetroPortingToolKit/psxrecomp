#ifndef PSX_AUDIO_SYNC_H
#define PSX_AUDIO_SYNC_H
#ifdef __cplusplus
extern "C" {
#endif
/* Call before audio device creation. No guest state, clocks or mixer changes. */
#define PSX_AUDIO_SYNC_MIN_MS 20
#define PSX_AUDIO_SYNC_MAX_MS 180
#define PSX_AUDIO_SYNC_DEFAULT_MS 70
/* Session configuration only; call during committed mod activation before
 * device creation. Disabling resets the request, not the user's saved option. */
void psx_audio_sync_set_enabled(int enabled);
int psx_audio_sync_set_target(int target_ms);
void psx_audio_sync_set_diagnostics(int enabled);
int psx_audio_sync_diagnostics(void);
int psx_audio_sync_enabled(void);
int psx_audio_sync_target(int configured_fallback_ms);
#ifdef __cplusplus
}
#endif
#endif
