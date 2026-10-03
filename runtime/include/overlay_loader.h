#ifndef OVERLAY_LOADER_H
#define OVERLAY_LOADER_H

/* overlay_loader — A-1 runtime overlay DLL cache and dynamic dispatch.
 *
 * On CD DMA completion (overlay_capture_on_dma), the loader checks whether
 * a cached DLL exists for the overlay bytes.  If it does, it LoadLibrary's
 * the DLL, calls overlay_init() to wire callbacks, enumerates func_XXXXXXXX
 * exports, and registers each in the dynamic dispatch table.
 *
 * dirty_ram_dispatch calls overlay_loader_dispatch() before the interpreter,
 * so compiled overlay functions get priority over interpretation.
 */

#include "cpu_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The segment an overlay shard is compiled for unless its manifest says
 * otherwise (docs/SEGMENT_AWARE_CODE.md §5.7). Captures record their bytes and
 * dispatch_entry_pcs at the KSEG0 address (overlay_capture.c), and a manifest
 * without an S record keys its entries there. The emitter bakes a shard's
 * segment into every PC it makes: the $ra a jal/jalr writes, its jump and
 * exception-resume PCs, its I-cache fetch tags and its store-PC stamps. The
 * same bytes reached through KUSEG or KSEG1 run at a different architectural
 * PC (a different link value; KSEG1 is uncached), so capture records each
 * dispatch's segment, compile_overlays.py builds a shard per segment with
 * entries (KUSEG and KSEG1 ones in the seg-kuseg/ and seg-kseg1/ cache
 * subdirectories), and a shard runs only for PCs in its own segment; a PC with
 * no shard of its segment is interpreted at its own PC. (OpenBIOS's exception
 * path enters its RAM patch slots at KUSEG.) */
#define PSX_OVERLAY_CODE_SEGMENT 0x80000000u
static inline int psx_overlay_code_segment_pc(uint32_t pc) {
    return (pc & 0xE0000000u) == PSX_OVERLAY_CODE_SEGMENT;
}

/* Called at game handoff to set the cache root directory and game ID.
 * cache_dir: absolute path to the cache root (e.g. "build-dev/cache")
 * game_id:   product code (e.g. "SCUS-94236") */
/* Host callback table handed to every loaded module (overlay shards and BIOS
 * modules alike). Safe before overlay_loader_init. */
struct OverlayCallbacks;
const struct OverlayCallbacks *overlay_loader_callbacks(void);

/* Canonical cache arch-abi tag (caches are namespaced per backend AND per
 * target so a Windows-x64 gcc DLL and, later, a same-OS arm64 build for the
 * same fragment never comingle). compile_overlays.py
 * computes the IDENTICAL string; keep the two mappings in lockstep
 * ("<os>-<arch>": win|linux|macos + x64|arm64|x86). It is this build's own
 * architecture -- the running slice of a universal macOS binary -- and the
 * runtime exports it as PSX_OVERLAY_ARCH_ABI to every compile it spawns
 * (autocompile.c) and passes it to bios_module_build.py, so shards and BIOS
 * modules are compiled for the architecture that loads them, not for the
 * architecture of the Python or the compiler binary. */
#if defined(_WIN32)
#  define PSX_OVERLAY_ARCH_OS_ "win"
#elif defined(__APPLE__)
#  define PSX_OVERLAY_ARCH_OS_ "macos"
#else
#  define PSX_OVERLAY_ARCH_OS_ "linux"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#  define PSX_OVERLAY_ARCH_CPU_ "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#  define PSX_OVERLAY_ARCH_CPU_ "x64"
#elif defined(__i386__) || defined(_M_IX86)
#  define PSX_OVERLAY_ARCH_CPU_ "x86"
#else
#  define PSX_OVERLAY_ARCH_CPU_ "unknown"
#endif
#define PSX_OVERLAY_ARCH_ABI PSX_OVERLAY_ARCH_OS_ "-" PSX_OVERLAY_ARCH_CPU_

/* "<os>-<arch>" of this build (PSX_OVERLAY_ARCH_ABI), the cache layout's
 * arch-abi segment. Also keyed into the BIOS module cache. */
const char *overlay_loader_arch_abi(void);

void overlay_loader_init(const char *cache_dir, const char *game_id,
                         uint32_t config_hash);

/* Per-function native-disable for small timing-sensitive overlay routines.
 * Addresses may be KUSEG/KSEG0/KSEG1; the loader keys by physical address. */
int overlay_loader_native_block_add(uint32_t addr);

/* (sljit removed 2026-07-15: overlay_loader_apply_live_policy was declared
 * here to resolve the sljit live-execution policy after code_provider_init.) */

/* Called from overlay_capture_on_dma after the capture-set insert.
 * Computes CRC32 of bytes, checks cache, loads DLL if present.
 * load_addr: physical RAM address, size/bytes: the transferred data. */
void overlay_loader_check_cache(uint32_t load_addr, uint32_t size,
                                const uint8_t *bytes);

/* Called from dirty_ram_dispatch before the interpreter.
 * Returns 1 and calls the compiled function if addr is registered,
 * 0 if not found (fall through to interpreter). */
int overlay_loader_dispatch(CPUState *cpu, uint32_t addr);

/* Freeze new DLL discovery/load (lazy try_load_region, live publish commit,
 * rescan). Already-registered natives keep running. Required for rollback
 * resim / selfcheck: overlay registration is host-only and not in the snap,
 * so a load mid-pass#1 that is visible at the start of pass#2 forks BB-edge
 * IRQ cadence (matched clocks, mismatched GPRs). */
void overlay_loader_set_load_freeze(int freeze);
int  overlay_loader_load_frozen(void);

/* Drop the negative lazy-lookup memo (host-only). Call on snap restore so
 * resim#1 and resim#2 take the same dispatch path. */
void overlay_loader_clear_lazy_miss(void);

/* After RAM restore: bust overlay candidate gen fast-path + static-match
 * cache so native vs interp is re-decided against restored bytes. */
void overlay_loader_resync_validation_after_restore(void);

/* Step 2.8: re-scan the cache dir for DLLs compiled after init and clear the
 * checked-regions memo so the next dispatch reconsiders the cache. Idempotent
 * (loaded DLLs stay loaded); emu thread only. */
void overlay_loader_rescan(void);

/* Two-phase live publication: the compile-output watcher maps a freshly
 * published image off the emulation thread, then the emulation thread performs
 * callback wiring, manifest validation, and candidate registration. The opaque
 * object owns exactly one speculative library reference until commit/discard. */
typedef struct OverlayPreparedImage OverlayPreparedImage;
OverlayPreparedImage *overlay_loader_prepare_published(const char *dll_path);
int overlay_loader_commit_published(OverlayPreparedImage *image);
void overlay_loader_discard_prepared(OverlayPreparedImage *image);

/* True if the cache holds <region_start8>_<crc8>.{dll,so}. */
int overlay_loader_has_cached_crc(uint32_t region_start, uint32_t crc);

/* Returns number of functions currently registered in the dynamic table. */
int overlay_loader_registered_count(void);

/* Returns full loader state for TCP diagnostics. */
void overlay_loader_get_status(int *active, int *registered,
                               int *regions_checked,
                               char *cache_dir_out, int cache_dir_len,
                               char *game_id_out,   int game_id_len,
                               uint32_t *checked_out, int checked_max,
                               int *checked_written,
                               uint32_t *last_crc_out, int *last_file_found_out);

/* Most recent loader event string (DLL load success/failure). Surfaced via
 * the overlay_loader_status TCP command — no stderr logging (Rule 3). */
const char *overlay_loader_last_msg(void);

/* Inc3 §8.5: a write into the code range of a currently-executing native
 * overlay entry cannot be recovered lazily — that entry is permanently
 * blacklisted to the interpreter. Called from the psx_write_* store path
 * (memory.c) only when the written page is a watched overlay code page. */
void overlay_loader_active_write_check(uint32_t phys, uint32_t size);
/* Invalidate negative lazy-lookups after a write to manifested code pages. */
void overlay_loader_note_code_write(void);

/* Inc3 counters, surfaced via overlay_loader_status (field meanings remapped
 * to the per-entry model — see overlay_loader.c getters). */
void overlay_loader_get_counters(uint32_t *loads, uint32_t *invalidations,
                                 uint32_t *unregistered,
                                 uint64_t *disp_native, uint64_t *disp_interp,
                                 uint64_t *stale_blocked,
                                 uint32_t *last_write_pc,
                                 uint32_t *last_write_addr,
                                 uint32_t *last_write_size,
                                 int *regions, uint32_t *revalidations);
void overlay_loader_get_load_timing(uint64_t *total_us, uint64_t *max_us,
                                    uint64_t *last_us);
/* Opt-in PSX_RUNTIME_PERF_DIAG sampler: returns and clears the hottest native
 * owner since the preceding call. Counts owner activations, INCLUDING CPS
 * continuations, not guest function invocations (use fntrace for entries).
 * The bounded direct-mapped sampler can undercount on collision; it cannot
 * inflate a count. Disabled runs pay no table update cost. */
void overlay_loader_take_hot_native(uint32_t *pc, uint64_t *calls);
/* Exact shadow-differential summary for opt-in perf diagnostics. */
void overlay_loader_get_shadow_summary(uint64_t *calls, uint64_t *divergences,
                                       uint32_t *first_divergence_pc);

void overlay_loader_get_reload_debug(int *r0_valid, uint32_t *r0_writes,
                                     uint32_t *r0_fn_lo, uint32_t *r0_fn_hi,
                                     uint32_t *r0_crc_live,
                                     uint32_t *reval_attempts,
                                     uint32_t *reval_crc_miss,
                                     uint32_t *last_reval_crc);

/* Dispatches that skipped the per-dispatch code-range crc32 via the unchanged
 * page-generation fast path (overlay-cache v2 P2). */
uint64_t overlay_loader_gen_fastpath(void);
int      overlay_loader_range_link_count(void);
int      overlay_loader_range_index_overflow(void);
int      overlay_loader_lazy_manifest_count(void);
int      overlay_loader_lazy_manifest_overflow(void);
uint64_t overlay_loader_candidate_overflow(void);
uint64_t overlay_loader_pair_aliases(void);
/* KUSEG/KSEG1 (and unmapped-segment) dispatches the loader left to the
 * interpreter: no shard compiled for their segment is cached or valid yet
 * (docs/SEGMENT_AWARE_CODE.md §5.7). Capture records their segment, so a
 * warm cache takes this to 0. */
uint64_t overlay_loader_segment_alias_interp(void);
/* KUSEG/KSEG1 dispatches run natively by a shard compiled for their segment. */
uint64_t overlay_loader_segment_native(void);
int      overlay_loader_dump_lazy_at(uint32_t addr, char *out, int cap);

/* Overlay CI wrapper early-return attribution (PSX_POST_LOAD_PROBE). */
void overlay_loader_get_ci_skip_diag(uint64_t *unit, uint64_t *supp,
                                     uint64_t *none, uint64_t *sr,
                                     uint64_t *deliv, uint64_t *enter);
int  overlay_loader_call_unit_depth(void);
/* Native-shard nesting (active-candidate stack depth, in-progress entry),
 * saved before and restored after a landing that longjmps out of a shard
 * and resumes the interrupted code (render_pass.c watchdog abort). */
void overlay_loader_native_nesting(int *active_depth, uint32_t *inprogress);
void overlay_loader_set_native_nesting(int active_depth, uint32_t inprogress);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_LOADER_H */
