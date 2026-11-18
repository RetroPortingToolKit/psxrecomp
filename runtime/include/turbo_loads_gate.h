/* Fast Loading engage/hold gate.
 *
 * The turbo-through-loads path runs the guest at a multiple of real time and
 * skips most presents while the game loads. It must engage for data loads
 * (RE3 door transitions: hundreds of sectors of ReadN) and not for the short
 * data reads a game makes while the player has control (RE3: ~11 vblanks of
 * reading every few seconds), nor for streamed audio (XA-ADPCM, CD-DA).
 *
 * Engage: at least TURBO_LOADS_WINDOW_READS of the last TURBO_LOADS_WINDOW
 * vblanks were data reads. Hold: while data reads continue or the drive's
 * load hold (the gap tail between read bursts) is on. Release: after
 * release_frames without either, then the window is cleared. */
#ifndef TURBO_LOADS_GATE_H
#define TURBO_LOADS_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_LOADS_WINDOW        60
#define TURBO_LOADS_WINDOW_READS  40

typedef struct TurboLoadsGate {
    uint64_t history;   /* bit i = data read i vblanks ago */
    int engaged;
    int release;
} TurboLoadsGate;

void turbo_loads_gate_reset(TurboLoadsGate *g);
/* One vblank. data_read: the drive delivered data sectors this vblank (not XA
 * audio, not CD-DA). load_hold: the drive's load-in-progress tail (burst gap)
 * outside XA streaming. Returns 1 while Fast Loading should run. */
int turbo_loads_gate_step(TurboLoadsGate *g, int data_read, int load_hold, int release_frames);
/* Data-read vblanks in the current window (for load telemetry). */
int turbo_loads_gate_window_reads(const TurboLoadsGate *g);

#ifdef __cplusplus
}
#endif

#endif
