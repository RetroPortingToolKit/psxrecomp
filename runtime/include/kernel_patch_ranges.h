/* kernel_patch_ranges.h — the pure half of the kernel patch-range mechanism.
 *
 * The guest legitimately overwrites words inside compiled BIOS kernel bodies
 * at boot: the Psy-Q libapi patchers (_patch_gte / _patch_card / _patch_card2
 * / _patch_pad) and the BIOS's own install stubs. Those ranges are declared
 * per image by [[recompiler.install_slots]] and emitted next to the kernel
 * body table (psx_bios_image.h, PsxKernelPatchRange).
 *
 * These two functions are the whole decision, kept free of runtime globals so
 * they can be tested directly (runtime/tests/test_kernel_patch_ranges.c).
 * memory.c wraps them with its snapshotted window constants; dirty_ram_interp.c
 * reads the second one through that wrapper.
 *
 * Rule 18: the patched words still execute, on the dirty-RAM interpreter,
 * exactly as the guest wrote them. Nothing here synthesises behaviour — it
 * only decides which backend runs which words.
 */
#ifndef PSX_KERNEL_PATCH_RANGES_H
#define PSX_KERNEL_PATCH_RANGES_H

#include <stdint.h>

#include "psx_bios_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compare live kernel RAM [lo, hi) against its ROM source, SKIPPING every
 * declared patch range that intersects it. Returns 0 when every compared
 * segment matches (memcmp's "equal" sense), non-zero otherwise.
 *
 * ram/rom point at the two images; `ram_lo` is the RAM address that
 * rom[rom_off] corresponds to, so a byte at RAM address a is ram[a] and
 * rom[rom_off + (a - ram_lo)]. `ranges` must be sorted by lo and
 * non-overlapping (BiosAddressModel::from_config validates and sorts, so a
 * malformed profile cannot reach here). `skips_out`, when non-null, is
 * incremented once per skipped segment.
 *
 * A body with NO intersecting range degenerates to a single memcmp of the
 * whole extent — the original behaviour, unchanged. */
int psx_kernel_patch_cmp(const PsxKernelPatchRange *ranges, uint32_t n,
                         const uint8_t *ram, const uint8_t *rom,
                         uint32_t ram_lo, uint32_t rom_off,
                         uint32_t lo, uint32_t hi,
                         uint64_t *skips_out);

/* Does a declared range END at this RAM address? The emitter registered each
 * range's hi as a continuation key, so the interpreter hands straight-line
 * flow back to static dispatch there and only the patched words interpret. */
int psx_kernel_patch_ends_at(const PsxKernelPatchRange *ranges, uint32_t n,
                             uint32_t phys);

#ifdef __cplusplus
}
#endif

#endif /* PSX_KERNEL_PATCH_RANGES_H */
