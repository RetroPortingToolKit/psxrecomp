/*
 * host_clock.h — the host's simulation/presentation pacing clock.
 *
 * TWO INDEPENDENT DEADLINES: when the next guest frame is due, and when the
 * next PRESENTED frame is due. They coincide for a host that presents once per
 * guest frame, which is what this engine does today. Decoupling them lets the
 * display run at its own steady cadence while the guest runs at whatever rate
 * the machine manages. `alpha` is the interpolation weight between two
 * simulated frames for a presenter that wants to blend.
 *
 * PORTED FROM snesrecomp, runner/src/desktop/host_clock.{h,c}, where it is
 * SnesHostClock and had itself been lifted out of SuperMetroidRecomp's
 * sm_video.c. The logic is transcribed unchanged; only the names and the
 * default frame rate differ. Keeping it recognisable across the three
 * recompilers is deliberate: a divergence should be a deliberate edit with a
 * reason, not drift.
 *
 * WHY IT IS HERE. `frame_pacing.c` is a DEADLINE CAP — it holds the frame to
 * 59.94 Hz when the guest is running fast, and when the guest is slower than
 * real time it re-anchors and runs free (frame_pacing.c, the debt comment).
 * That is the right answer for pacing the SIMULATION and is unchanged. It has
 * no answer for the display's cadence when the guest cannot keep up: every
 * frame is then rounded up to a whole refresh interval and the result is
 * visible judder. This clock is the other half — it decides WHEN TO DRAW,
 * independently of how fast the guest is going.
 *
 * It is also the engine-side half of the seam docs/LAUNCHER_CORE.md cuts:
 * "pacing *of the simulation*" stays in the engine, "pacing of presentation,
 * vsync policy" moves to the launcher. Having the presentation clock as its own
 * object is what makes that split a move rather than a rewrite.
 *
 * Measured on the sibling N64 project, which took this port first: decoupling
 * at 30 Hz made the guest 11.6 % FASTER while giving a steady cadence, because
 * a frame that is not DRAWN was still SIMULATED. Capping the cadence instead --
 * holding the emulation thread to the deadline -- cost 37 % of guest speed.
 */
#ifndef PSXRECOMP_HOST_CLOCK_H
#define PSXRECOMP_HOST_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NTSC PSX frame rate; matches main.cpp's PSX_FRAME_PERIOD_MS = 1000/59.94. */
#define PSX_HOST_NTSC_HZ 59.94

typedef struct PsxHostClock {
    double   next_simulation, next_presentation;
    double   simulation_hz, presentation_hz;
    uint64_t simulation_frames, presentations, missed_presentations;
} PsxHostClock;

void   psx_host_clock_reset(PsxHostClock* c, double now,
                            double simulation_hz, double presentation_hz);
bool   psx_host_clock_simulation_due(const PsxHostClock* c, double now);
/* `elapsed_periods` is how many guest frame periods the simulated frame
 * actually spanned; a lag frame spans more than one. */
void   psx_host_clock_simulation_done(PsxHostClock* c, double now,
                                      bool preserve_debt, double elapsed_periods);
bool   psx_host_clock_presentation_due(const PsxHostClock* c, double now);
void   psx_host_clock_presentation_done(PsxHostClock* c, double now);
double psx_host_clock_alpha(const PsxHostClock* c, double now);
double psx_host_clock_next_deadline(const PsxHostClock* c);

/* Which presentation rate to run at: an explicit fps when it is one the clock
 * supports, else the display's refresh, clamped. 0 = display / every frame.
 * Accepts any rate from 20 up, because the point is running BELOW the panel
 * rate when the guest cannot keep up, not above it. */
bool   psx_host_valid_fps(unsigned fps);
double psx_host_presentation_hz(unsigned fps, double display_refresh);

#ifdef __cplusplus
}
#endif
#endif /* PSXRECOMP_HOST_CLOCK_H */
