#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_sync.h"
#include <assert.h>
int main(void) {
    psx_audio_sync_set_enabled(0);
    assert(psx_audio_sync_target(160) == 160);
    psx_audio_sync_set_enabled(1);
    assert(psx_audio_sync_target(180) == 70);
    assert(psx_audio_sync_set_target(180));
    assert(psx_audio_sync_target(180) == 180);
    assert(psx_audio_sync_set_target(70));
    assert(!psx_audio_sync_set_target(19));
    assert(!psx_audio_sync_set_target(181));
    assert(!psx_audio_sync_set_target(71));
    assert(psx_audio_sync_target(180) == 70);
    psx_audio_sync_set_diagnostics(1); assert(psx_audio_sync_diagnostics());
    psx_audio_sync_set_enabled(0); assert(!psx_audio_sync_diagnostics());
    assert(psx_audio_sync_target(180) == 180);
    psx_audio_sync_set_enabled(1); assert(psx_audio_sync_target(180) == 70);
}
