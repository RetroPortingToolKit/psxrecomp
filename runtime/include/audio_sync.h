#ifndef PSX_AUDIO_SYNC_H
#define PSX_AUDIO_SYNC_H
#ifdef __cplusplus
extern "C" {
#endif
/* Call before audio device creation. No guest state, clocks or mixer changes. */
void psx_audio_sync_set_enabled(int enabled);
int psx_audio_sync_enabled(void);
int psx_audio_sync_target(int configured_fallback_ms);
#ifdef __cplusplus
}
#endif
#endif
