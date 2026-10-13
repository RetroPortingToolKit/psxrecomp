/* psx_bios_module.h — a recompiled BIOS backend as a separately loaded module.
 *
 * WHY. A bundled release links only the redistributable OpenBIOS backend: the
 * C a retail image recompiles to is a translation of that image's code, so it
 * never ships (docs/BIOS_SELECTION.md, docs/ci/BUNDLED_RELEASES.md). For a
 * player who owns a retail dump, the release therefore builds that backend on
 * THEIR machine, from THEIR dump, with the overlay toolchain the release
 * already carries — the same mechanism that turns overlays streamed from the
 * player's disc into native code — and loads it here as a shared library.
 *
 * THE CONTRACT. A BIOS module is an overlay-style self-contained library. It
 * imports nothing from the executable (the executable exports no symbols on
 * any platform); every call back into the runtime goes through two callback
 * tables handed over at init:
 *
 *   OverlayCallbacks         (overlay_api.h)  — the part a BIOS shares with
 *                                               overlay shards: cycles, loads,
 *                                               GTE, syscall, dispatch_call…
 *   PsxBiosModuleCallbacks   (this header)    — what only a BIOS dispatcher
 *                                               needs: traps, the dirty-RAM
 *                                               and kernel-bless probes, and
 *                                               the runtime globals the
 *                                               generated dispatch reads and
 *                                               writes (through pointers).
 *
 * The generated C is compiled UNCHANGED with -DPSX_OVERLAY_DLL_BUILD (so the
 * inline RAM-load and cycle fast paths in cpu_state.h / psx_cyc.h /
 * psx_cycles.h become out-of-line calls the glue forwards) plus
 * -DPSX_BIOS_MODULE_BUILD, with this header prepended: the mapping block
 * below renames the handful of runtime globals the emitter declares `extern`
 * to pointer dereferences, exactly as cpu_state.h does for the bail state.
 * A macro'd `extern T g_x;` becomes `extern T (*g_x_p);`, which is the
 * pointer's own declaration — no per-symbol shim is needed in the C.
 *
 * The module exports four functions (psx_bios_module_glue.c.inc):
 *   int  psx_bios_module_abi(void)          == PSX_BIOS_MODULE_ABI_TAG
 *   uint32_t psx_bios_module_codegen_hash(void)  == PSX_OVERLAY_CODEGEN_HASH
 *   int  psx_bios_module_init(const OverlayCallbacks*, const PsxBiosModuleCallbacks*)
 *   const PsxBiosBackend *psx_bios_module_backend(void)
 * and is otherwise opaque. The host (runtime/src/psx_bios_module.c) gates a
 * load on the first two, calls init, then registers the descriptor with
 * psx_bios_register() so every existing selection path sees it as one more
 * linked backend.
 *
 * ABI. Bump PSX_BIOS_MODULE_ABI_VERSION whenever PsxBiosModuleCallbacks
 * changes shape, a mapping is added, or the glue's forwarding semantics
 * change. Append-only growth of the table is still a bump: a module built
 * against a shorter table must not be handed a longer one it never checks.
 * The cache directory name carries this version, the codegen hash, and the
 * flavor, so a stale module is never even opened.
 */
#ifndef PSX_BIOS_MODULE_H
#define PSX_BIOS_MODULE_H

#include <stddef.h>
#include <stdint.h>

/* ---- Name mappings (module build only) -------------------------------------
 * Pure #defines, deliberately with no declarations: each generated `extern`
 * line, and psx_memory.h's own `extern uint32_t g_psx_ram_mask;`, declares
 * the pointer with the right type once the name is rewritten. Prepended by
 * tools/bios_module_build.py before the first #include, so psx_memory.h sees
 * the g_psx_ram_mask mapping too (the emitted normalize() masks with it). */
#if defined(PSX_BIOS_MODULE_BUILD)
/* The one global the emitter uses without its own extern line (cpu_state.h
 * declares it only outside PSX_OVERLAY_DLL_BUILD), so declare the pointer. */
extern uint64_t *g_psx_bm_bail_flattened_p;
#  define g_psx_bios_hle_hook       (*g_psx_bm_hle_hook_p)
#  define g_psx_dispatch_depth      (*g_psx_bm_dispatch_depth_p)
#  define g_dispatch_static_hits    (*g_psx_bm_static_hits_p)
#  define g_debug_current_func_addr (*g_psx_bm_current_func_addr_p)
#  define g_psx_bail_flattened      (*g_psx_bm_bail_flattened_p)
#  define g_psx_ram_mask            (*g_psx_bm_ram_mask_p)
#  define s_frame_count             (*g_psx_bm_frame_count_p)
#  define g_psx_mod_guest_functions (*g_psx_bm_mod_guest_functions_p)
#  define psx_mod_dispatch_guest_function g_psx_bm_mod_dispatch_guest
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
struct OverlayCallbacks;
struct PsxBiosBackend;

#define PSX_BIOS_MODULE_ABI_VERSION 2

/* Combined tag: ABI version (low 16) | codegen flavor (high 16), like
 * PSX_OVERLAY_ABI_TAG. The flavor comes from the same -DPSX_OVERLAY_FLAVOR
 * the executable was built with; the glue and the host both evaluate it. */
#ifndef PSX_OVERLAY_FLAVOR
#define PSX_OVERLAY_FLAVOR 0
#endif
#define PSX_BIOS_MODULE_ABI_TAG \
    ((int)((PSX_BIOS_MODULE_ABI_VERSION & 0xFFFF) | ((PSX_OVERLAY_FLAVOR & 0xFFFF) << 16)))

/* What the BIOS dispatcher needs beyond OverlayCallbacks. Function members
 * are the runtime's real implementations; pointer members alias the
 * runtime's globals so a module read/write is the runtime's read/write. */
typedef struct PsxBiosModuleCallbacks {
    uint32_t size;   /* sizeof(PsxBiosModuleCallbacks) on the host side */

    /* Traps (runtime/src/traps.c; strict_translator emits these). */
    void (*arith_overflow)(struct CPUState *cpu);
    void (*brk)(struct CPUState *cpu, uint32_t code, uint32_t pc);
    void (*unaligned_access)(struct CPUState *cpu, uint32_t addr, uint32_t pc);
    void (*rfe_escape_check)(struct CPUState *cpu);

    /* Dispatch probes the generated dispatcher consults per iteration. */
    int  (*dirty_ram_dispatch)(struct CPUState *cpu, uint32_t addr, uint32_t stop_addr);
    int  (*dirty_ram_is_dirty)(uint32_t phys);
    int  (*dirty_ram_text_native_ok)(uint32_t phys);
    int  (*kernel_bless_dispatchable)(uint32_t phys);
    int  (*game_address_in_text)(uint32_t addr);
    int  (*fntrace_is_game_started)(void);
    void (*fntrace_record)(struct CPUState *cpu, uint32_t target);
    void (*trace_dispatch)(uint32_t func_addr);
    /* Retail images emit probe sites (SCPH-1001 does; OpenBIOS does not). */
    void (*log_probe)(uint32_t pc, struct CPUState *cpu);
    /* Segment-aware dispatch: a PC served by another segment's home body is
     * recorded in the runtime's segment-miss ring (psx_segment_miss.h). */
    void (*segment_miss_record_kind)(uint32_t addr, uint32_t home, uint32_t ra,
                                     uint32_t sp, uint32_t frame, uint32_t kind);

    /* Runtime-owned state the dispatcher reads/writes. Never NULL from a
     * host at this ABI; the glue keeps local dummies for a pre-init call. */
    int      (**hle_hook)(struct CPUState *cpu, uint32_t phys);   /* bios_hle.c */
    int       *dispatch_depth;      /* psx_bios_backend.c */
    uint64_t  *static_hits;         /* generated game dispatch counter */
    uint32_t  *current_func_addr;   /* debug_server.c */
    uint64_t  *bail_flattened;      /* traps.c */
    uint32_t  *ram_mask;            /* psx_ram_geometry.c (live 2/8 MiB) */
    uint64_t  *frame_count;         /* debug_server.c s_frame_count (miss records) */
    /* Mod-defined guest function table (mod_runtime.cpp). The generated
     * dispatch consults the registration count at the top of every
     * iteration. Added in ABI 2 alongside the emitter's mod-guest-function
     * template. */
    uint32_t *mod_guest_functions;  /* registration count */
    int (*mod_dispatch_guest_function)(struct CPUState *cpu, uint32_t addr);
} PsxBiosModuleCallbacks;

/* ---- Host side (runtime/src/psx_bios_module.c) ----------------------------- */
#if !defined(PSX_BIOS_MODULE_BUILD)

/* Can this process turn a retail dump into a loaded backend? True when a
 * bundled backend is linked (this is a product build, not a setup host) and
 * the overlay toolchain is present beside the executable. */
int psx_bios_module_supported(const char *exe_dir);

/* The profile identity a dump of this size/CRC would build with, or NULL
 * when no shipped profile matches (then no module can be built). */
const char *psx_bios_module_known_id(uint32_t crc32, uint32_t size);

/* Load a previously built module for this dump if one is cached, else build
 * it with the toolchain (blocking; seconds with tcc, minutes with gcc on a
 * large image), then load and register it. Returns the registered backend
 * or NULL with `err` filled. `exe_dir` is the executable's directory; the
 * cache lives under <exe_dir>/cache/bios/. */
const struct PsxBiosBackend *psx_bios_module_acquire(const char *dump_path,
                                                     const char *exe_dir,
                                                     int allow_build,
                                                     char *err, size_t err_cap);

/* 1 when a module for this dump is already built and cached (no build
 * needed to select it), 0 otherwise or when the dump is not a known image. */
int psx_bios_module_is_cached(const char *dump_path, const char *exe_dir);

/* Cached module path for a dump identity, whether or not it exists. */
int psx_bios_module_cache_path(const char *exe_dir, const char *stem,
                               uint32_t crc32, char *out, size_t cap);

/* Load one module file: ABI + codegen-hash gate, init, register. */
const struct PsxBiosBackend *psx_bios_module_load(const char *path,
                                                  char *err, size_t err_cap);

/* Callback tables the host hands to a module (also used by the test host). */
void psx_bios_module_fill_callbacks(PsxBiosModuleCallbacks *out);

/* Last build/load diagnostic line, for logs and the launcher. */
const char *psx_bios_module_last_message(void);

/* Progress sink for a build in flight (launcher progress modal). pct is 0..1
 * when a stage boundary is known, negative otherwise; msg is the builder's
 * current line. Called on the thread running psx_bios_module_acquire. NULL
 * clears it. */
typedef void (*PsxBiosModuleProgressFn)(void *ctx, float pct, const char *msg);
void psx_bios_module_set_progress(PsxBiosModuleProgressFn fn, void *ctx);

#endif /* !PSX_BIOS_MODULE_BUILD */

#ifdef __cplusplus
}
#endif

#endif /* PSX_BIOS_MODULE_H */
