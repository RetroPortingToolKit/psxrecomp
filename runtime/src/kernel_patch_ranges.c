/* kernel_patch_ranges.c — see kernel_patch_ranges.h for the contract. */

#include "kernel_patch_ranges.h"

#include <string.h>

int psx_kernel_patch_cmp(const PsxKernelPatchRange *ranges, uint32_t n,
                         const uint8_t *ram, const uint8_t *rom,
                         uint32_t ram_lo, uint32_t rom_off,
                         uint32_t lo, uint32_t hi,
                         uint64_t *skips_out)
{
    uint32_t at = lo;
    if (hi <= lo) return 0;
    for (uint32_t i = 0; i < n && at < hi; i++) {
        uint32_t plo = ranges[i].lo, phi = ranges[i].hi;
        if (phi <= at) continue;    /* range lies behind the cursor */
        if (plo >= hi) break;       /* sorted: no later range intersects */
        if (plo > at &&
            memcmp(ram + at, rom + rom_off + (at - ram_lo), plo - at) != 0)
            return 1;
        /* Skip the patched words. A range may start below `lo` or end above
         * `hi`; clamp so the cursor only ever advances inside the body. */
        at = phi > hi ? hi : phi;
        if (skips_out) (*skips_out)++;
    }
    if (at < hi &&
        memcmp(ram + at, rom + rom_off + (at - ram_lo), hi - at) != 0)
        return 1;
    return 0;
}

int psx_kernel_patch_ends_at(const PsxKernelPatchRange *ranges, uint32_t n,
                             uint32_t phys)
{
    for (uint32_t i = 0; i < n; i++) {
        if (ranges[i].hi == phys) return 1;
    }
    return 0;
}
