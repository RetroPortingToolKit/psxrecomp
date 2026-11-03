#include "audio_sync.h"
static int enabled;
void psx_audio_sync_set_enabled(int value) { enabled = value != 0; }
int psx_audio_sync_enabled(void) { return enabled; }
int psx_audio_sync_target(int fallback) { return enabled ? 80 : fallback; }
