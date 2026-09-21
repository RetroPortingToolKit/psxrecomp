/* psx_bios_backend.h — the routing seam between the runtime and whichever
 * recompiled BIOS is active.
 *
 * Every build links more than one recompiled BIOS (bundled OpenBIOS plus a
 * retail image). Each one exports exactly ONE symbol — its backend descriptor,
 * named <STEM>_psx_bios_backend — and everything else it defines is either
 * static or stem-prefixed, so the images cannot collide at link time.
 *
 * The runtime picks a backend at startup (docs/BIOS_SELECTION.md: no explicit
 * player choice -> OpenBIOS; an explicit, identity-matching choice -> that
 * image) and publishes it as psx_bios_active.
 *
 * Callers do not go through this struct. The unprefixed psx_dispatch() /
 * psx_dispatch_call() the game's generated C and the runtime already call are
 * thin forwarders (psx_bios_backend.c) that route to the active backend, and
 * psx_bios_image is assigned from it at selection time — so adding a second
 * BIOS changed no call sites.
 *
 * Adding another BIOS later is one more descriptor and one more registry
 * entry; it is not another round of symbol collisions.
 */
#ifndef PSX_BIOS_BACKEND_H
#define PSX_BIOS_BACKEND_H

#include <stdint.h>

#include "psx_bios_image.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

typedef struct PsxBiosBackend {
    /* Every image this backend's recompiled code is valid for. Entry 0 is the
     * reference the code was generated from; any further entry is a verified
     * claim that each seeded function body is byte-identical to it, declared
     * as [[program.accepted]] and checked by tools/add_retail_bios.py.
     *
     * This exists because the retail v2.2/v3.0 images share their boot code
     * and kernel and differ only in the shell, which carries no seeds and is
     * interpreted: generating from SCPH-1001 and from SCPH-5501 with the
     * shared kernel corpus yields byte-identical C apart from the identity
     * fields. One backend therefore serves several images, and a player
     * switches between them with no rebuild.
     *
     * The shared fields (kbless window, HLE anchors) are identical in every
     * entry — they describe bytes the verification proved common. Only
     * identity differs, which is exactly what savestate scoping, netplay
     * agreement and provenance need. The matched entry, not entry 0, is what
     * psx_bios_activate() publishes as psx_bios_image. */
    const PsxBiosImageInfo *images;
    uint32_t                image_count;

    /* Dispatch entry points for this image. */
    void (*dispatch)(struct CPUState *cpu, uint32_t addr);
    void (*dispatch_call)(struct CPUState *cpu, uint32_t addr,
                          uint32_t return_addr);

    /* Kernel body-extent table used by the kernel-image bless mechanism
     * (memory.c). Published as psx_bios_kernel_bodies/_count on selection.
     *
     * The native call-stub table is deliberately absent: it is only read by
     * the generated dispatch itself, so it stays static there. */
    const PsxKernelBody *kernel_bodies;
    uint32_t             kernel_body_count;

    /* Kernel-RAM ranges the guest legitimately patches at runtime, from this
     * image's [[recompiler.install_slots]] (psx_bios_image.h). The bless
     * verifier skips them and the dirty-RAM interpreter resumes native at
     * each range's hi. Null/0 for an image that declares no slots. */
    const PsxKernelPatchRange *kernel_patch_ranges;
    uint32_t                   kernel_patch_range_count;
} PsxBiosBackend;

/* The backend in use. Null before psx_bios_select() runs; every forwarder and
 * every consumer of psx_bios_image depends on it being set first. */
extern const PsxBiosBackend *psx_bios_active;

/* Backends compiled into this binary, in preference order (bundled OpenBIOS
 * first). Emitted by the build as psx_bios_registry.c. */
extern const PsxBiosBackend *const psx_bios_registry[];
extern const uint32_t              psx_bios_registry_count;

/* Look up a compiled-in backend by an image id ("SCPH-5501", "OPENBIOS"),
 * searching every accepted image of every backend. When `out_image` is
 * non-null it receives the matching entry. Null if this build cannot run it. */
const PsxBiosBackend *psx_bios_find(const char *image_id,
                                    const PsxBiosImageInfo **out_image);

/* Find the backend and image whose declared identity matches these bytes.
 * size + CRC32 must both agree: a mismatch is a guaranteed wild jump, so
 * identity decides WHICH image may run, not merely whether to warn. */
const PsxBiosBackend *psx_bios_match(uint32_t size, uint32_t crc32,
                                     const PsxBiosImageInfo **out_image);

/* Select a backend and publish `image` (one of its own entries) as the global
 * psx_bios_image. Passing null for `image` selects entry 0, the reference.
 * Returns 0 if backend is null or image is not one of its entries. */
int psx_bios_activate(const PsxBiosBackend *backend,
                      const PsxBiosImageInfo *image);

/* How many distinct images this build can run (sum over linked backends). */
uint32_t psx_bios_image_total(void);

/* The i'th runnable image across all linked backends, or null. Lets callers
 * list what a player may supply without knowing the backend layout. */
const PsxBiosImageInfo *psx_bios_image_at(uint32_t index,
                                          const PsxBiosBackend **out_backend);

/* The bundled, redistributable backend (image_bundled != 0), or null if this
 * build has none. */
const PsxBiosBackend *psx_bios_bundled(void);

/* True if a player-supplied image is meaningful for this build (some linked
 * backend is not the bundled one). */
int psx_bios_has_selectable(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_BIOS_BACKEND_H */
