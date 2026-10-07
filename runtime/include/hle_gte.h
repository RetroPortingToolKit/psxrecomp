#ifndef PSX_HLE_GTE_H
#define PSX_HLE_GTE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;
/* For a build-selected native service which owns operation-level timing.
 * Retains GTE outputs, PGXP and projection capture/replay. ENHANCED omits
 * per-command latency; REFERENCE uses the ordinary timed entry. The caller
 * resolves earlier pending work and publishes its batch completion itself.
 * No mutable mode, scope nesting, or state survives this call. */
void psx_hle_gte_execute(struct CPUState* cpu, uint32_t command);
#ifdef __cplusplus
}
#endif
#endif
