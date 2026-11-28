/* The SPU also has direct native callers (mods, snapshots and host pumping).
 * Publish preceding CD/device input before it catches its mixer up. During
 * device catch-up, callbacks must keep the intermediate event clock and must
 * not recursively synchronize. The maintained mixer body stays shared. */
#include "psx_cycles.h"
static uint64_t psx_spu_observation_cycle_count(void) {
    if (!psx_in_device_service) psx_devices_observe();
    return psx_get_cycle_count();
}
#define psx_get_cycle_count psx_spu_observation_cycle_count
/* Snapshot CLOCK is already authoritative. Servicing devices before the
 * remaining device sections are restored would replay the discarded epoch. */
#define PSX_SPU_RESTORE_CYCLE_COUNT() psx_cycle_count
#include "spu.c"
