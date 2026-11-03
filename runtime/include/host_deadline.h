#ifndef PSX_HOST_DEADLINE_H
#define PSX_HOST_DEADLINE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One monotonic counter sample per decision. Individual sleeps are bounded
 * by max_ticks and one second; a 200us margin absorbs ordinary timer jitter. */
unsigned psx_host_deadline_sleep_us(uint64_t now, uint64_t deadline,
                                    uint64_t frequency, uint64_t max_ticks);
/* Wait at most max_ticks from entry (apart from OS scheduling overshoot).
 * Return elapsed counter ticks for the presentation idle budget. Past/invalid
 * deadlines do not wait. Does not own or advance a guest/presentation schedule. */
uint64_t psx_host_wait_until(uint64_t deadline, uint64_t frequency,
                             uint64_t max_ticks);
#ifdef __cplusplus
}
#endif
#endif
