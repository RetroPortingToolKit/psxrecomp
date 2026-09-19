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
    /* Identity, kernel-bless window and HLE anchors for this image. Copied
     * into the global psx_bios_image when this backend is selected. */
    const PsxBiosImageInfo *image;

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

/* Look up a compiled-in backend by its profile id ("SCPH-1001", "OPENBIOS").
 * Null if this build does not carry it. */
const PsxBiosBackend *psx_bios_find(const char *image_id);

/* Select a backend and publish it (also assigns the global psx_bios_image).
 * Returns 0 if backend is null. */
int psx_bios_activate(const PsxBiosBackend *backend);

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
