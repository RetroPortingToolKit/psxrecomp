#ifndef PSX_AUDIO_CATCHUP_H
#define PSX_AUDIO_CATCHUP_H
#include <stdint.h>
typedef struct PsxAudioCatchup { uint64_t last_underruns; int active; } PsxAudioCatchup;
/* Presentation-only recovery: no clock, sample cursor or guest state changes.
 * Ordinary jitter or an absorbed stall must not mute music. Arm only after a
 * real dry callback and at least one whole late frame; retain arming until
 * the existing pacer has caught up. Other intentional sinks reset this gate. */
static inline int psx_audio_catchup_update(PsxAudioCatchup *s, int enabled,
    int other_sink, uint64_t underruns, uint64_t now, uint64_t deadline,
    uint64_t period) {
    const int restarted = underruns < s->last_underruns;
    const int dry = underruns > s->last_underruns;
    s->last_underruns = underruns;
    if (restarted || !enabled || other_sink || !deadline || !period || now < deadline) {
        s->active = 0; return 0;
    }
    if (dry && now - deadline >= period) s->active = 1;
    return s->active;
}
/* Arming after starvation does not mean a backlog exists yet. Let fresh PCM
 * refill the target first; discard only accelerated output above that reserve.
 * This is host queue policy, independent of room IDs or widescreen state. */
static inline int psx_audio_catchup_discard(int armed, double fill_ms,
                                         double target_ms) {
    return armed && target_ms > 0.0 && fill_ms >= target_ms;
}
#endif
