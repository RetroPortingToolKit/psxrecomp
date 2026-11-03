#include "host_deadline.h"
#include "host_time.h"
#ifndef PSX_HOST_DEADLINE_TESTING
#include "psx_sdl.h"
#endif

unsigned psx_host_deadline_sleep_us(uint64_t now, uint64_t deadline,
                                    uint64_t frequency, uint64_t max_ticks) {
    uint64_t remaining, us;
    if (!frequency || !max_ticks || now >= deadline) return 0;
    remaining = deadline - now; /* Compare before subtract: no old pacer wrap. */
    if (remaining > max_ticks) remaining = max_ticks;
    if (remaining > frequency) remaining = frequency;
    /* A host wait is not a guest arithmetic contract. Conversion is bounded
     * to one second and avoids overflowing remaining*1000000 for large clocks. */
    us = (uint64_t)((double)remaining * 1000000.0 / (double)frequency);
    return us > 300u ? (unsigned)(us - 200u) : 0u;
}

uint64_t psx_host_wait_until(uint64_t deadline, uint64_t frequency,
                             uint64_t max_ticks) {
    uint64_t start, now;
    if (!deadline || !frequency || !max_ticks) return 0;
    start = SDL_GetPerformanceCounter();
    if (start >= deadline) return 0;
    if (deadline - start > max_ticks)
        deadline = start + max_ticks; /* Below the original deadline: no wrap. */
    now = start;
    for (;;) {
        unsigned us;
        if (now >= deadline) return now - start;
        us = psx_host_deadline_sleep_us(now, deadline, frequency, max_ticks);
        if (us) psx_host_sleep_micros(us);
        /* An interrupted or overshooting timer is resolved by a fresh sample,
         * never by subtracting an assumed sleep from a stale duration. */
        now = SDL_GetPerformanceCounter();
    }
}
