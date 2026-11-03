#ifndef PSX_CAPTURE_RING_H
#define PSX_CAPTURE_RING_H
#include "cpu_state.h"
#include <stdint.h>
struct RbeSnapRing;
#ifdef __cplusplus
extern "C" {
#endif
/* Native raw capture into the ring's unpublished reusable spare. */
int psx_capture_ring_save(struct RbeSnapRing *ring, uint32_t tick,
                          const CPUState *cpu, uint32_t bios, uint32_t entry);
#ifdef __cplusplus
}
#endif
#endif
