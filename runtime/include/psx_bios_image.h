/* psx_bios_image.h — the linked recompiled BIOS describes itself.
 *
 * The generated <stem>_dispatch.c DEFINES these symbols; the runtime READS
 * them instead of hardcoding per-image constants (memory.c's old
 * KBLESS_RAM_LO/HI/ROM_OFF, bios_hle.c's old PSX_SHELL_ENTRY_PHYS /
 * KADDR_DELIVER_RET). Because the values are emitted next to the code they
 * describe — couriered verbatim from the BIOS profile by the emitter — they
 * cannot disagree with the linked BIOS. Same seam as psx_bios_kernel_bodies.
 *
 * A zero value means "this BIOS has no such thing" (no kernel-bless window,
 * no shell boot-skip entry, no DeliverEvent HLE return): consumers must treat
 * zero as structurally-unavailable, never as address 0.
 */
#ifndef PSX_BIOS_IMAGE_H
#define PSX_BIOS_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel body-extent table for the kernel-image bless mechanism (memory.c).
 * Emitted by full_function_emitter.cpp next to psx_bios_image. */
typedef struct {
    uint32_t key;      /* dispatch key (RAM, normalized) */
    uint32_t body_lo;  /* RAM extent of code reachable from key */
    uint32_t body_hi;  /* exclusive */
} PsxKernelBody;

/* Kernel-RAM ranges the guest is EXPECTED to overwrite at runtime: the Psy-Q
 * libapi patchers (_patch_gte / _patch_card / _patch_card2 / _patch_pad) and
 * the BIOS's own install stubs rewrite words inside compiled kernel bodies at
 * boot. Declared per image by [[recompiler.install_slots]] and emitted next to
 * the body table.
 *
 * Two consumers, one table:
 *   - memory.c's bless verifier compares a body in segments that SKIP these
 *     ranges, so a body with a live patch inside it can still run native.
 *   - dirty_ram_interp.c surfaces back to static dispatch when straight-line
 *     interpretation reaches a range's `hi`, which the emitter registered as a
 *     continuation key. Only the patched words themselves interpret.
 *
 * This is not HLE: the guest's patched instructions still execute as written,
 * on the dirty-RAM interpreter. Only the code AROUND them changes backend. */
typedef struct {
    uint32_t lo;       /* RAM extent of the patched words [lo, hi) */
    uint32_t hi;       /* exclusive; also the native resume key */
} PsxKernelPatchRange;

/* Published from the ACTIVE backend at selection (psx_bios_backend.c),
 * hence a pointer rather than an array: a build links more than one
 * recompiled BIOS and each carries its own table. Indexing is unchanged
 * for consumers. */
extern const PsxKernelBody *psx_bios_kernel_bodies;
extern uint32_t             psx_bios_kernel_body_count;
extern const PsxKernelPatchRange *psx_bios_kernel_patch_ranges;
extern uint32_t                   psx_bios_kernel_patch_range_count;

/* Native call-stub extents (A0/B0/C0). Layout shared with the generated
 * dispatch, which keeps its own static table. */
typedef struct {
    uint32_t key;
    uint32_t body_lo;
    uint32_t body_hi;
} PsxNativeStub;

typedef struct {
    /* Kernel-bless window (profile address_model copy with kernel_bless):
     * RAM [kbless_ram_lo, kbless_ram_hi) is the BIOS's boot-time verbatim
     * copy of ROM file offset kbless_rom_off. All zero = no bless window. */
    uint32_t kbless_ram_lo;
    uint32_t kbless_ram_hi;
    uint32_t kbless_rom_off;

    /* HLE anchors (profile [recompiler.runtime_exports]); 0 = unavailable. */
    uint32_t shell_entry_phys;   /* boot-skip trigger (bios_hle.c) */
    uint32_t deliver_event_ret;  /* $ra after the kernel DeliverEvent jalr */

    /* Image identity, computed from the ROM bytes at emit time. */
    uint32_t    image_size;      /* bytes */
    uint32_t    image_crc32;     /* IEEE CRC-32 (zlib) */
    uint32_t    image_wordsum;   /* sum of LE u32 words (savestate checksum) */
    const char* image_sha256;    /* 64 lowercase hex chars */
    const char* image_id;        /* profile [program] id, e.g. "SCPH-1001" */

    /* Profile [program.image] redistributable: the BIOS ships WITH the game
     * (e.g. OpenBIOS, MIT). The runtime then resolves the bundled image
     * next to the build and hides the entire BIOS-selection surface
     * (no picker, no bios.cfg, no launcher row). */
    int         image_bundled;
} PsxBiosImageInfo;

/* Assigned from the active backend at selection; not const for that
 * reason. Reads are unchanged. */
extern PsxBiosImageInfo psx_bios_image;

#ifdef __cplusplus
}
#endif

#endif /* PSX_BIOS_IMAGE_H */
