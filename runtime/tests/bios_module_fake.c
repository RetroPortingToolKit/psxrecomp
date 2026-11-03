/* A fake BIOS module for test_bios_module_host.c: exports the four entry
 * points psx_bios_module.h requires, with a descriptor whose identity the
 * test can assert, and records what init was handed. Built as a shared
 * library by runtime/CMakeLists.txt; never shipped.
 *
 * Which ABI/hash it reports is chosen at compile time so the same source
 * builds the good module and the two bad ones the host must reject. */
#include <stdint.h>
#include <string.h>

#include "cpu_state.h"
#include "overlay_api.h"
#include "psx_bios_backend.h"
#include "psx_bios_module.h"

#ifndef FAKE_ABI_TAG
#define FAKE_ABI_TAG PSX_BIOS_MODULE_ABI_TAG
#endif
#ifndef FAKE_CODEGEN_HASH
#define FAKE_CODEGEN_HASH PSX_OVERLAY_CODEGEN_HASH
#endif

#if defined(_WIN32)
#  define FAKE_EXPORT __declspec(dllexport)
#else
#  define FAKE_EXPORT __attribute__((visibility("default")))
#endif

static int s_dispatch_calls;
static PsxBiosModuleCallbacks s_callbacks;
static void fake_dispatch(struct CPUState *cpu, uint32_t addr) {
    (void)cpu; (void)addr;
    s_dispatch_calls++;
    if (s_callbacks.mod_guest_functions && *s_callbacks.mod_guest_functions &&
        s_callbacks.mod_dispatch_guest_function)
        s_callbacks.mod_dispatch_guest_function(cpu, addr);
}
static void fake_dispatch_call(struct CPUState *cpu, uint32_t addr, uint32_t ra) {
    (void)cpu; (void)addr; (void)ra;
    s_dispatch_calls++;
}

static const PsxBiosImageInfo s_image = {
    0, 0, 0,
    0, 0,
    524288u, 0xDEADBEEFu, 0u,
    "0000000000000000000000000000000000000000000000000000000000000000",
    "FAKE-0001",
    0,
};

static const PsxBiosBackend s_backend = {
    &s_image, fake_dispatch, fake_dispatch_call, 0, 0, 0, 0,
};

/* What init received, for the test to inspect through psx_bios_module_fake_probe. */
static uint32_t s_init_bcbs_size;
static int      s_init_ok;
static int     *s_seen_depth;

FAKE_EXPORT int psx_bios_module_abi(void) { return FAKE_ABI_TAG; }
FAKE_EXPORT uint32_t psx_bios_module_codegen_hash(void) { return (uint32_t)FAKE_CODEGEN_HASH; }
FAKE_EXPORT int psx_bios_module_init(const OverlayCallbacks *cbs,
                                     const PsxBiosModuleCallbacks *bcbs) {
    if (!cbs || !bcbs || bcbs->size < sizeof(PsxBiosModuleCallbacks)) return 0;
    s_callbacks = *bcbs;
    s_init_bcbs_size = bcbs->size;
    s_seen_depth = bcbs->dispatch_depth;
    s_init_ok = 1;
    return 1;
}
FAKE_EXPORT const PsxBiosBackend *psx_bios_module_backend(void) { return &s_backend; }

/* Test-only probe: 1 if init ran with a full table and the depth pointer
 * aliases the host's (the host passes &g_psx_dispatch_depth). */
FAKE_EXPORT int psx_bios_module_fake_probe(int *host_depth) {
    return s_init_ok && s_init_bcbs_size == sizeof(PsxBiosModuleCallbacks) &&
           s_seen_depth == host_depth;
}
