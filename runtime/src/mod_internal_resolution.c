#include "mod_plugins.h"
#include "mod_internal_resolution.h"

static uint32_t requested_lines;

int psx_mod_set_internal_resolution(uint32_t target_lines) {
    if (target_lines && (target_lines < 120u || target_lines > 8192u))
        return 0;
    requested_lines = target_lines;
    return 1;
}

uint32_t psx_mod_internal_resolution_request(void) {
    return requested_lines;
}

void psx_mod_internal_resolution_reset(void) {
    requested_lines = 0;
}
