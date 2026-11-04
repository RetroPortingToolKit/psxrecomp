#include "audio_sync.h"
static int enabled, target = PSX_AUDIO_SYNC_DEFAULT_MS, diagnostics;
void psx_audio_sync_set_enabled(int value) {
    enabled = value != 0;
    if (!enabled) { target = PSX_AUDIO_SYNC_DEFAULT_MS; diagnostics = 0; }
}
int psx_audio_sync_enabled(void) { return enabled; }
int psx_audio_sync_set_target(int value) {
    if (value < PSX_AUDIO_SYNC_MIN_MS || value > PSX_AUDIO_SYNC_MAX_MS || value % 5)
        return 0;
    target = value;
    return 1;
}
int psx_audio_sync_target(int fallback) { return enabled ? target : fallback; }
void psx_audio_sync_set_diagnostics(int value) { diagnostics = value != 0; }
int psx_audio_sync_diagnostics(void) { return enabled && diagnostics; }
