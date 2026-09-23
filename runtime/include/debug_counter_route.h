#ifndef PSX_DEBUG_COUNTER_ROUTE_H
#define PSX_DEBUG_COUNTER_ROUTE_H
#include <stdint.h>

typedef struct { uint32_t frames; uint16_t buttons; } DebugInputRouteStep;

/* Advance a route by a guest counter, not by host polling frequency. Unsigned
 * subtraction allows normal wraparound; reset/load stops the route. */
static int debug_counter_route_advance(const DebugInputRouteStep *steps,
    uint32_t count, uint32_t *index, uint32_t *remaining,
    uint32_t *last, uint32_t now) {
    uint32_t elapsed=now-*last;
    *last=now;
    if (elapsed>8000) return 0;
    while (*index<count && elapsed) {
        if (elapsed<*remaining) { *remaining-=elapsed;break; }
        elapsed-=*remaining;
        if (++*index<count) *remaining=steps[*index].frames;
    }
    return *index<count;
}
#endif
