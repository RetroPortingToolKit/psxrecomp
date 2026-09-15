/* Cache fixtures use the overlay cycle-charge seam, but install no functional
 * CPU observer. Fail loudly if that prerequisite ever changes. */
#include "psx_icache.h"
#include <stdlib.h>
void psx_cpu_step_boundary(CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (g_psx_cpu_step_boundary_callback) abort();
}
