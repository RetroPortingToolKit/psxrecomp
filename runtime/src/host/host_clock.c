/* host_clock.c — see runtime/include/host_clock.h. Transcribed from
 * snesrecomp's runner/src/desktop/host_clock.c; keep them readable side by
 * side, and port fixes both ways. */
#include "host_clock.h"
#include <math.h>
#include <string.h>

void psx_host_clock_reset(PsxHostClock* c, double now,
                          double simulation_hz, double presentation_hz) {
    /* memset, not `= {0}`: the braced form warns under
       -Wmissing-field-initializers when this header is included from C++. */
    memset(c, 0, sizeof *c);
    c->next_simulation   = now;
    c->next_presentation = now;
    c->simulation_hz   = (isfinite(simulation_hz) && simulation_hz > 0)
                       ? simulation_hz : PSX_HOST_NTSC_HZ;
    c->presentation_hz = (isfinite(presentation_hz) && presentation_hz > 0)
                       ? presentation_hz : 60.0;
}

bool psx_host_clock_simulation_due(const PsxHostClock* c, double now) {
    return now >= c->next_simulation;
}

void psx_host_clock_simulation_done(PsxHostClock* c, double now,
                                    bool preserve_debt, double elapsed_periods) {
    double deadline = c->next_simulation + elapsed_periods / c->simulation_hz;
    /* Keep ordinary sub-frame scheduling jitter on the original phase; drop the
     * debt of a genuinely slow frame so the guest never bursts to catch up.
     * A caller may PRESERVE the debt for non-interactive phases whose frames
     * refill a guest-driven audio queue -- the reason snesrecomp has the flag. */
    c->next_simulation = (preserve_debt || now <= deadline)
                       ? deadline : now + 1.0 / c->simulation_hz;
    ++c->simulation_frames;
}

bool psx_host_clock_presentation_due(const PsxHostClock* c, double now) {
    return now >= c->next_presentation;
}

void psx_host_clock_presentation_done(PsxHostClock* c, double now) {
    double period  = 1.0 / c->presentation_hz;
    double overdue = fmax(0.0, now - c->next_presentation);
    uint64_t missed = (uint64_t)floor(overdue / period);
    c->missed_presentations += missed;
    c->next_presentation += (double)(missed + 1) * period;
    ++c->presentations;
}

double psx_host_clock_alpha(const PsxHostClock* c, double now) {
    return fmax(0.0, fmin(1.0, 1.0 - (c->next_simulation - now) * c->simulation_hz));
}

double psx_host_clock_next_deadline(const PsxHostClock* c) {
    return fmin(c->next_simulation, c->next_presentation);
}

bool psx_host_valid_fps(unsigned fps) {
    return fps == 0u || (fps >= 20u && fps <= 360u);
}

double psx_host_presentation_hz(unsigned fps, double display_refresh) {
    if (!psx_host_valid_fps(fps)) fps = 0u;
    if (fps) return (double)fps;
    if (!isfinite(display_refresh) || display_refresh < 1.0) return 60.0;
    return fmin(display_refresh, 360.0);
}
