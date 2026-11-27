#pragma once

#include "native_bios_services.h"

/* Runtime-only ownership, in guest RAM so current-build snapshots restore it.
 * No generated-code layout or native dispatcher marker depends on this state. */
#define PSX_NATIVE_CD_EVENT_HANDLES 0x0000b680u

static inline void psx_native_cd_seed_boot_handles(
    void (*write_word)(uint32_t, uint32_t)) {
    for (uint32_t i = 0; i < PSX_NATIVE_BOOT_CD_EVENTS; ++i)
        write_word(PSX_NATIVE_CD_EVENT_HANDLES + i * 4u, 0xf1000000u | i);
}
