/* test_bios_module_host.c — the host side of loadable BIOS backends
 * (runtime/src/psx_bios_module.c) against fake modules built by
 * runtime/CMakeLists.txt from tests/bios_module_fake.c.
 *
 * What it proves, with the real host code and a real dlopen/LoadLibrary:
 *   1. A module with the right ABI tag and codegen hash loads, is handed a
 *      full callback table whose pointers alias the host's globals, and is
 *      registered AFTER the builtins (which the constructor seeded).
 *   2. A module with the wrong ABI tag is refused, and so is one with the
 *      wrong codegen hash; neither reaches the registry.
 *   3. Loading the same module twice registers it once.
 *   4. The registry seeded from psx_bios_builtin_registry before main().
 *
 * Every runtime symbol psx_bios_module.c forwards is stubbed here; nothing
 * about the emulation core is exercised. Paths to the three fake modules come
 * from compile definitions set by CMake.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "overlay_api.h"
#include "psx_bios_backend.h"
#include "psx_bios_module.h"

/* ---- stubs for what the host forwards -------------------------------------- */
void psx_arith_overflow(CPUState *cpu) { (void)cpu; }
void psx_break(CPUState *cpu, uint32_t code, uint32_t pc) { (void)cpu; (void)code; (void)pc; }
void psx_unaligned_access(CPUState *cpu, uint32_t addr, uint32_t pc) { (void)cpu; (void)addr; (void)pc; }
void psx_rfe_escape_check(CPUState *cpu) { (void)cpu; }
int  dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop) { (void)cpu; (void)addr; (void)stop; return 0; }
int  dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 0; }
int  dirty_ram_text_native_ok(uint32_t phys) { (void)phys; return 0; }
int  psx_kernel_bless_dispatchable(uint32_t phys) { (void)phys; return 0; }
int  psx_game_address_in_text(uint32_t addr) { (void)addr; return 0; }
int  fntrace_is_game_started(void) { return 0; }
void fntrace_record(CPUState *cpu, uint32_t target) { (void)cpu; (void)target; }
void debug_server_trace_dispatch(uint32_t f) { (void)f; }
void debug_server_log_probe(uint32_t pc, CPUState *cpu) { (void)pc; (void)cpu; }
void psx_segment_miss_record_kind(uint32_t a, uint32_t h, uint32_t r, uint32_t s, uint32_t f, uint32_t k) { (void)a; (void)h; (void)r; (void)s; (void)f; (void)k; }
uint64_t s_frame_count = 0;
uint32_t g_psx_mod_guest_functions = 1;
int psx_mod_dispatch_guest_function(struct CPUState *cpu, uint32_t address) {
    cpu->gpr[2] = address;
    return 1;
}
int (*g_psx_bios_hle_hook)(struct CPUState *cpu, uint32_t phys) = 0;
uint64_t g_dispatch_static_hits = 0;
uint32_t g_debug_current_func_addr = 0;
uint64_t g_psx_bail_flattened = 0;
uint32_t g_psx_ram_mask = 0x1FFFFFu;
void psx_bios_hle_configure(uint32_t a, uint32_t b) { (void)a; (void)b; }
int  autocompile_toolchain_available(void) { return 0; }
const char *overlay_loader_arch_abi(void) { return "test-arch"; }
static OverlayCallbacks s_cbs;
const OverlayCallbacks *overlay_loader_callbacks(void) { return &s_cbs; }

/* One builtin, standing in for the linked OpenBIOS. */
static const PsxBiosImageInfo s_builtin_image = {
    0, 0, 0, 0, 0, 524288u, 0x11111111u, 0u, "", "OPENBIOS", 1,
};
static void builtin_dispatch(struct CPUState *c, uint32_t a) { (void)c; (void)a; }
static void builtin_dispatch_call(struct CPUState *c, uint32_t a, uint32_t r) { (void)c; (void)a; (void)r; }
static const PsxBiosBackend s_builtin = {
    &s_builtin_image, builtin_dispatch, builtin_dispatch_call, 0, 0, 0, 0,
};
const PsxBiosBackend *const psx_bios_builtin_registry[] = { &s_builtin };
const uint32_t psx_bios_builtin_registry_count = 1u;

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#ifndef FAKE_GOOD
#error "FAKE_GOOD / FAKE_BAD_ABI / FAKE_BAD_HASH must name the fake module paths"
#endif

int main(void) {
    char err[256];
    const PsxBiosBackend *b;

    /* 4. Seeded before main() by the constructor. */
    CHECK(psx_bios_registry_count == 1, "registry seeded count %u", (unsigned)psx_bios_registry_count);
    CHECK(psx_bios_registry[0] == &s_builtin, "registry[0] is the builtin");
    CHECK(psx_bios_bundled() == &s_builtin, "bundled() finds the builtin");
    CHECK(!psx_bios_has_selectable(), "nothing selectable before a module loads");

    /* 2. Wrong ABI tag: refused, registry untouched. */
    err[0] = 0;
    b = psx_bios_module_load(FAKE_BAD_ABI, err, sizeof(err));
    CHECK(b == 0, "bad-ABI module must be refused");
    CHECK(strstr(err, "ABI tag") != 0, "bad-ABI reason names the tag: %s", err);
    CHECK(psx_bios_registry_count == 1, "bad-ABI module not registered");

    err[0] = 0;
    b = psx_bios_module_load(FAKE_BAD_HASH, err, sizeof(err));
    CHECK(b == 0, "bad-hash module must be refused");
    CHECK(strstr(err, "codegen hash") != 0, "bad-hash reason names the hash: %s", err);
    CHECK(psx_bios_registry_count == 1, "bad-hash module not registered");

    /* 1. The good one loads, inits with the host tables, registers after the builtin. */
    err[0] = 0;
    b = psx_bios_module_load(FAKE_GOOD, err, sizeof(err));
    CHECK(b != 0, "good module loads: %s", err);
    if (b) {
        CHECK(b->image && strcmp(b->image->image_id, "FAKE-0001") == 0, "descriptor identity");
        CHECK(b->image->image_crc32 == 0xDEADBEEFu, "descriptor crc");
        CHECK(psx_bios_registry_count == 2, "registered: count %u", (unsigned)psx_bios_registry_count);
        CHECK(psx_bios_registry[1] == b, "registered after the builtin");
        CHECK(psx_bios_has_selectable(), "a non-bundled backend is now selectable");
        CHECK(psx_bios_find("FAKE-0001") == b, "find() by id");
        CHECK(psx_bios_activate(b) == 1, "activate");
        CHECK(psx_bios_active == b, "active pointer");
        CHECK(psx_bios_image.image_crc32 == 0xDEADBEEFu, "published image copied");
        /* The module saw the host's own depth counter (aliasing, not a copy). */
        {
            PsxBiosModuleCallbacks probe;
            psx_bios_module_fill_callbacks(&probe);
            CHECK(probe.size == sizeof(probe), "table size stamped");
            CHECK(probe.dispatch_depth == &g_psx_dispatch_depth, "depth pointer aliases host");
            CHECK(probe.hle_hook == &g_psx_bios_hle_hook, "hle hook pointer aliases host");
            CHECK(probe.ram_mask == &g_psx_ram_mask, "ram mask pointer aliases host");
            CHECK(probe.mod_guest_functions == &g_psx_mod_guest_functions,
                  "mod count aliases host");
            CHECK(probe.mod_dispatch_guest_function == psx_mod_dispatch_guest_function,
                  "mod callback forwards to host");
        }
    }

    /* 3. Same file again: the descriptor is the same object, registered once. */
    {
        const PsxBiosBackend *again = psx_bios_module_load(FAKE_GOOD, err, sizeof(err));
        CHECK(again == b, "second load returns the same descriptor");
        CHECK(psx_bios_registry_count == 2, "second load does not duplicate: %u",
              (unsigned)psx_bios_registry_count);
    }

    /* Cache path shape: one place formats it; assert the pieces are present. */
    {
        char path[512];
        CHECK(psx_bios_module_cache_path("/x", "SCPH1001", 0x37157331u, path, sizeof(path)),
              "cache path fits");
        CHECK(strstr(path, "/x/cache/bios/test-arch/bm2_") == path, "cache path root: %s", path);
        CHECK(strstr(path, "/SCPH1001_37157331.") != 0, "cache path stem+crc: %s", path);
    }

    /* Identity table: the shipped profiles' images are known; junk is not. */
    CHECK(psx_bios_module_known_id(0x37157331u, 524288u) &&
          strcmp(psx_bios_module_known_id(0x37157331u, 524288u), "SCPH-1001") == 0,
          "SCPH-1001 known");
    CHECK(psx_bios_module_known_id(0x12345678u, 524288u) == 0, "unknown crc rejected");
    CHECK(psx_bios_module_known_id(0x37157331u, 1u) == 0, "wrong size rejected");

    if (s_fail) { printf("%d failure(s)\n", s_fail); return 1; }
    printf("bios module host: ok\n");
    return 0;
}
