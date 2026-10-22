#include "psx_thread_local.h"
/* psx_bios_backend.c — routing between the runtime and the active BIOS.
 *
 * A build links every recompiled BIOS it ships (bundled OpenBIOS plus a retail
 * image). Each exports one descriptor; this file picks one, publishes it, and
 * owns the symbols that used to be defined by the generated dispatch back when
 * exactly one BIOS was linked.
 *
 * Nothing else had to change: psx_dispatch()/psx_dispatch_call() keep their
 * names here as forwarders, so the game's generated C and all existing runtime
 * call sites are untouched, and psx_bios_image keeps working as a global
 * because it is assigned on selection.
 *
 * Selection policy itself lives in main.cpp (it needs the player's choice and
 * the game config); this file only provides the mechanism.
 */

#include <string.h>

#include "bios_hle.h"
#include "cpu_state.h"
#include "psx_bios_backend.h"
#include "psx_execution.h"

/* ── State published to the rest of the runtime ─────────────────────────── */

const PsxBiosBackend *psx_bios_active = 0;

/* Assigned from the active backend. Zeroed until then, which reads as
 * "structurally unavailable" for every field — the documented meaning of 0 in
 * psx_bios_image.h — rather than as stale data from some other image. */
PsxBiosImageInfo psx_bios_image;

const PsxKernelBody *psx_bios_kernel_bodies     = 0;
uint32_t             psx_bios_kernel_body_count = 0;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges     = 0;
uint32_t                   psx_bios_kernel_patch_range_count = 0;

/* Dispatch nesting depth. Shared dispatch state, not per-image: each generated
 * dispatch used to define its own copy, which is precisely why two of them
 * could not be linked together. */
PSX_THREAD_LOCAL int g_psx_dispatch_depth = 0;

/* ── Registry ───────────────────────────────────────────────────────────────
 *
 * The build lists the backends it LINKS in psx_bios_builtin_registry (written
 * by runtime.cmake as psx_bios_registry.c). The registry every consumer walks
 * is this mutable table: it starts as a copy of the builtins and grows when a
 * BIOS module built on the player's machine registers itself
 * (psx_bios_module.c). A backend is never removed: the published image and
 * kernel tables may point into it for the life of the process. */
#define PSX_BIOS_REGISTRY_CAP 8

const PsxBiosBackend *psx_bios_registry[PSX_BIOS_REGISTRY_CAP];
uint32_t              psx_bios_registry_count = 0;
static int            s_registry_seeded = 0;

static void registry_seed(void)
{
    uint32_t i;
    if (s_registry_seeded) return;
    s_registry_seeded = 1;
    for (i = 0; i < psx_bios_builtin_registry_count && i < PSX_BIOS_REGISTRY_CAP; i++) {
        const PsxBiosBackend *b = psx_bios_builtin_registry[i];
        if (b) psx_bios_registry[psx_bios_registry_count++] = b;
    }
}

int psx_bios_register(const PsxBiosBackend *backend)
{
    uint32_t i;
    registry_seed();
    if (!backend || !backend->image) return 0;
    for (i = 0; i < psx_bios_registry_count; i++)
        if (psx_bios_registry[i] == backend) return 1;
    if (psx_bios_registry_count >= PSX_BIOS_REGISTRY_CAP) return 0;
    psx_bios_registry[psx_bios_registry_count++] = backend;
    return 1;
}

uint32_t psx_bios_registry_seeded_count(void)
{
    registry_seed();
    return psx_bios_registry_count;
}

/* main.cpp walks psx_bios_registry[] directly in a dozen places, some before
 * any lookup here has run, and "count == 0" there means "setup host". Seed
 * before main() so those walks are never wrong; the same constructor idiom
 * the generated BIOS uses for its CPS marker. */
#if defined(_MSC_VER)
static void psx_bios_registry_ctor(void) { registry_seed(); }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (*psx_bios_registry_ctor_p)(void) = psx_bios_registry_ctor;
#else
__attribute__((constructor)) static void psx_bios_registry_ctor(void) { registry_seed(); }
#endif

/* ── Forwarders ─────────────────────────────────────────────────────────────
 *
 * The generated BIOS C defines <STEM>_psx_dispatch; these keep the unprefixed
 * names that ~66 runtime call sites and the game's generated C already use. */

PSX_THREAD_LOCAL PsxExecutionDispatch g_psx_execution_dispatch;
PSX_THREAD_LOCAL PsxExecutionAbort g_psx_execution_abort;

void psx_dispatch(CPUState *cpu, uint32_t addr)
{
    if (g_psx_execution_dispatch) { g_psx_execution_dispatch(cpu, addr, 0); return; }
    if (!psx_bios_active || !psx_bios_active->dispatch) return;
    psx_bios_active->dispatch(cpu, addr);
}

void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t return_addr)
{
    if (g_psx_execution_dispatch) { g_psx_execution_dispatch(cpu, addr, return_addr); return; }
    if (!psx_bios_active || !psx_bios_active->dispatch_call) return;
    psx_bios_active->dispatch_call(cpu, addr, return_addr);
}

/* ── Selection ──────────────────────────────────────────────────────────── */

const PsxBiosBackend *psx_bios_find(const char *image_id)
{
    registry_seed();
    if (!image_id) return 0;
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend *b = psx_bios_registry[i];
        if (b && b->image && b->image->image_id &&
            strcmp(b->image->image_id, image_id) == 0)
            return b;
    }
    return 0;
}

const PsxBiosBackend *psx_bios_bundled(void)
{
    registry_seed();
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend *b = psx_bios_registry[i];
        if (b && b->image && b->image->image_bundled) return b;
    }
    return 0;
}

int psx_bios_activate(const PsxBiosBackend *backend)
{
    if (!backend || !backend->image) return 0;
    psx_bios_active             = backend;
    psx_bios_image              = *backend->image;
    psx_bios_kernel_bodies      = backend->kernel_bodies;
    psx_bios_kernel_body_count  = backend->kernel_body_count;
    psx_bios_kernel_patch_ranges      = backend->kernel_patch_ranges;
    psx_bios_kernel_patch_range_count = backend->kernel_patch_range_count;
    /* Soft-return rematch can switch OPENBIOS ↔ SCPH without process exit.
     * Drop the prior image's call-HLE / boot-skip hook immediately so a
     * sticky SCPH DeliverEvent path cannot run against OpenBIOS ROM bytes
     * before session_reboot re-runs psx_bios_hle_plan + configure. */
    psx_bios_hle_configure(0, 0);
    return 1;
}

/* Is there any image the player could supply, i.e. a linked backend that is
 * NOT the bundled one? Drives whether the launcher shows its BIOS row: with
 * both a bundled and a retail image linked, a player must be able to opt into
 * the retail one (and clear back). */
int psx_bios_has_selectable(void)
{
    registry_seed();
    for (uint32_t i = 0; i < psx_bios_registry_count; i++) {
        const PsxBiosBackend *b = psx_bios_registry[i];
        if (b && b->image && !b->image->image_bundled) return 1;
    }
    return 0;
}
