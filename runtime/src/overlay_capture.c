#include "overlay_capture.h"
#include "overlay_loader.h"
#include "host_span_ring.h"
#include "dirty_ram_interp.h"
#include "code_provider.h"
#include "crc32.h"
#include "turbo_loads_gate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include "psx_sdl.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  define CAPTURE_MKDIR(path) _mkdir(path)
#  define CAPTURE_FSYNC(file) _commit(_fileno(file))
#  define CAPTURE_PID() ((unsigned long)GetCurrentProcessId())
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define CAPTURE_MKDIR(path) mkdir((path), 0755)
#  define CAPTURE_FSYNC(file) fsync(fileno(file))
#  define CAPTURE_PID() ((unsigned long)getpid())
#endif

/* ---- Capture set --------------------------------------------------------
 * Tracks which physical addresses have received CD DMA loads since game
 * handoff.  Used for dedup (state machine) and A-layer compilation queue.
 * The JSON output uses dirty_ram regions, not individual DMA blocks.
 * -------------------------------------------------------------------------*/

typedef enum {
    OV_QUEUED    = 0,  /* captured, not yet compiled              */
    OV_COMPILING = 1,  /* background thread working on it         */
    OV_COMPILED  = 2,  /* DLL written, dispatch table patched     */
} OvState;

typedef struct {
    uint32_t  load_addr;   /* physical RAM address                */
    uint32_t  size;        /* byte count of this DMA block        */
    uint8_t  *bytes;       /* unpatched copy taken at DMA time    */
    OvState   state;
} OvEntry;

#define MAX_OVERLAYS 4096

static OvEntry  s_entries[MAX_OVERLAYS];
static int      s_count    = 0;
static char     s_capture_path[768];
static int      s_active   = 0;
static int      s_enabled  = 0;   /* config gate; off unless overlay cache enabled */
static SDL_mutex *s_commit_mutex;
static SDL_atomic_t s_snapshot_seq;
static uint64_t s_latest_commit_seq;
/* Nonzero while a replay records or plays. The compiler reads the latest file
 * (--captures), and a compile is requested only after an autocapture commit.
 * The outgoing-overlay snapshots that DMA makes are committed by a writer
 * thread and can replace that file before the compiler opens it, so the
 * compiled bytes depended on host timing: an MDK (SLES-00599) recording
 * compiled 000F7000_4F69E5B0 and its playback, the same guest bytes, compiled
 * 000F7000_570A0CE6 from a later DMA snapshot (PS1B-698). While this is set,
 * those snapshots go to the history only and the compiler reads the
 * autocapture it was requested for. Written on the emulation thread, read by
 * the writer. A playback sets it only for a recording made this way (its
 * code log says so); an older recording's units were compiled from whichever
 * file was the latest, so its playback keeps that input. */
static SDL_atomic_t s_latest_autocap_only;

static int      s_history_enabled = 0;
static char     s_history_addendum[600];
static char     s_history_persist_dir[600];
static char     s_history_game_id[64];
static char     s_history_session[64];
static uint32_t s_history_sequence;
static uint64_t s_history_last_sig;
static int      s_history_written;
/* One lock for the durable history: the three values above and the files it
 * writes (the addendum, and the copies in the persist folder).
 * persist_history_snapshot runs on whichever thread committed the snapshot,
 * after s_commit_mutex is released: the periodic capture's writer, the writer
 * of the queue of outgoing snapshots, and the emulation thread (at a manual
 * dump, at shutdown, when a snapshot cannot be queued). With no lock of its
 * own two of them appended to the addendum at the same time. A record that
 * holds a snapshot leaves the stdio buffer in several writes, so the two lines
 * mixed and a reader lost both; two records could also take the same sequence
 * number.
 * Created when the history is switched on. With the option off (the default)
 * it does not exist and nothing takes it. No other lock is taken while it is
 * held. */
static SDL_mutex *s_history_mutex;
static SDL_atomic_t s_autocap_write_state; /* 0 idle, 1 writing, 2 complete */

static uint64_t capture_next_sequence(void)
{
    /* Never contend with the writer's long-held file commit mutex here: this
     * runs at a pre-write boundary on the emulation thread. */
    return (uint32_t)SDL_AtomicAdd(&s_snapshot_seq, 1) + 1u;
}

static int preserve_snapshot_async(uint32_t scope_lo, uint32_t scope_hi);
static int preserve_writer_start(void);
static int preserve_queue_has_room(void);
/* Outgoing snapshots committed on the emulation thread because the queue was
 * at its cap, and snapshots that could be neither written nor queued and were
 * dropped (at the cap with a store that cannot be written). Emulation thread
 * only. */
static unsigned s_preserve_sync_commits;
static unsigned s_preserve_lost;
/* The queue's writer thread: failed write attempts, and entries given up at
 * shutdown. Under s_preserve_mutex. */
static unsigned s_preserve_write_failures;
static unsigned s_preserve_abandoned;

static int capture_process_id(void)
{
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}

static void sanitize_filename(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;
    if (!dst_size) return;
    while (src && *src && out + 1 < dst_size) {
        unsigned char c = (unsigned char)*src++;
        dst[out++] = (char)(isalnum(c) || c == '-' || c == '_' ? c : '_');
    }
    dst[out] = '\0';
}

static int sync_file(FILE *f)
{
    if (fflush(f) != 0) return 0;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static int atomic_replace_file(const char *tmp_path, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(tmp_path, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp_path, path) == 0;
#endif
}

static uint64_t capture_file_sig(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint64_t h = 1469598103934665603ull;
    unsigned char buf[65536];
    size_t n, i;
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    if (ferror(f)) h = 0;
    fclose(f);
    return h;
}

/* Directory portion of `path` (no trailing separator); "." when `path` has
 * no separator. Colocates durable-history bookkeeping (the addendum.jsonl
 * and optional persist_dir copies) with whichever canonical capture manifest
 * path is currently configured, whether that came from
 * overlay_capture_set_out_dir or the game-specific overlay_capture_set_path. */
static void capture_path_dirname(char *dst, size_t dst_size, const char *path)
{
    const char *cut = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!cut || bslash > cut)) cut = bslash;
#endif
    if (!dst_size) return;
    if (cut) {
        size_t len = (size_t)(cut - path);
        if (len >= dst_size) len = dst_size - 1;
        memcpy(dst, path, len);
        dst[len] = '\0';
    } else {
        snprintf(dst, dst_size, ".");
    }
}

/* ---- Base64 encoder ----------------------------------------------------- */

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void write_b64(FILE *f, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        fputc(k_b64[(b >> 18) & 0x3F], f);
        fputc(k_b64[(b >> 12) & 0x3F], f);
        fputc(i + 1 < len ? k_b64[(b >>  6) & 0x3F] : '=', f);
        fputc(i + 2 < len ? k_b64[(b      ) & 0x3F] : '=', f);
    }
}

/* ---- Public API --------------------------------------------------------- */

void overlay_capture_set_out_dir(const char *out_dir)
{
    snprintf(s_capture_path, sizeof(s_capture_path),
             "%s/overlay_captures.json", out_dir ? out_dir : ".");
    if (!s_commit_mutex) s_commit_mutex = SDL_CreateMutex();
    (void)preserve_writer_start();
}

int overlay_capture_set_path(const char *path)
{
    const char *resolved = (path && path[0]) ? path : "overlay_captures.json";
    if (strlen(resolved) >= sizeof(s_capture_path)) {
        fprintf(stderr, "psxrecomp: overlay capture path is too long (%zu bytes)\n",
                strlen(resolved));
        return 0;
    }
    snprintf(s_capture_path, sizeof(s_capture_path), "%s", resolved);
    if (!s_commit_mutex) s_commit_mutex = SDL_CreateMutex();
    (void)preserve_writer_start();
    return 1;
}

void overlay_capture_init(const char *out_dir)
{
    overlay_capture_set_out_dir(out_dir);
}

void overlay_capture_set_enabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

void overlay_capture_configure_history(int enabled, const char *persist_dir,
                                       const char *game_id)
{
    time_t now;
    struct tm tm_now;
    char stamp[32];
    if (enabled && !s_history_mutex) {
        s_history_mutex = SDL_CreateMutex();
        if (!s_history_mutex) {
            /* Records written without the lock cannot be trusted. */
            fprintf(stderr,
                    "psxrecomp: overlay capture history stays off: no lock for it (%s)\n",
                    SDL_GetError());
            enabled = 0;
        }
    }
    if (s_history_mutex) SDL_LockMutex(s_history_mutex);
    s_history_enabled = enabled ? 1 : 0;
    s_history_persist_dir[0] = '\0';
    s_history_sequence = 0;
    s_history_last_sig = 0;
    s_history_written = 0;
    sanitize_filename(s_history_game_id, sizeof(s_history_game_id),
                      game_id && game_id[0] ? game_id : "UNKNOWN");
    if (persist_dir && persist_dir[0]) {
        snprintf(s_history_persist_dir, sizeof(s_history_persist_dir), "%s",
                 persist_dir);
    }
    {
        char dir[512];
        capture_path_dirname(dir, sizeof(dir),
                             s_capture_path[0] ? s_capture_path
                                               : "overlay_captures.json");
        snprintf(s_history_addendum, sizeof(s_history_addendum),
                 "%s/overlay_captures.addendum.jsonl", dir);
    }

    now = time(NULL);
#ifdef _WIN32
    gmtime_s(&tm_now, &now);
#else
    gmtime_r(&now, &tm_now);
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm_now);
    snprintf(s_history_session, sizeof(s_history_session), "%s-p%d",
             stamp, capture_process_id());
    if (s_history_mutex) SDL_UnlockMutex(s_history_mutex);
    if (s_history_enabled) {
        fprintf(stdout,
                "psxrecomp: durable overlay capture history enabled (%s%s%s)\n",
                s_history_addendum,
                s_history_persist_dir[0] ? "; immutable dir=" : "",
                s_history_persist_dir[0] ? s_history_persist_dir : "");
    }
}

static void overlay_capture_on_dma_impl(uint32_t load_addr, uint32_t size,
                                        const uint8_t *bytes);
void overlay_capture_on_dma(uint32_t load_addr, uint32_t size,
                             const uint8_t *bytes)
{
    const uint64_t span_t0 = host_span_begin();
    overlay_capture_on_dma_impl(load_addr, size, bytes);
    host_span_end(HSPAN_CAPTURE_ON_DMA, span_t0, load_addr);
}

static void overlay_capture_on_dma_impl(uint32_t load_addr, uint32_t size,
                                        const uint8_t *bytes)
{
    int i;
    OvEntry *e;
    extern int fntrace_is_game_started(void);

    if (!s_enabled) return;   /* overlay cache disabled in config */
    if (size == 0) return;

    /* Auto-activate on the first post-game-handoff DMA. */
    if (!s_active) {
        if (!fntrace_is_game_started()) return;
        s_active = 1;
    }

    /* This table reconstructs only the CURRENT DMA image for the legacy CRC
     * helper. Executed historical variants live in the additive snapshot
     * journal; retaining every DMA payload here caused unbounded session heap
     * growth and made A->B->A report stale B bytes. */
    for (i = 0; i < s_count; i++) {
        if (s_entries[i].load_addr == load_addr) {
            if (s_entries[i].size == size && s_entries[i].bytes &&
                memcmp(s_entries[i].bytes, bytes, size) == 0)
                return;
            uint8_t *replacement = (uint8_t *)malloc(size);
            if (!replacement) return;
            memcpy(replacement, bytes, size);
            free(s_entries[i].bytes);
            s_entries[i].bytes = replacement;
            s_entries[i].size = size;
            s_entries[i].state = OV_QUEUED;
            overlay_loader_check_cache(load_addr, size, bytes);
            return;
        }
    }

    if (s_count >= MAX_OVERLAYS) return;

    e = &s_entries[s_count++];
    e->load_addr = load_addr;
    e->size      = size;
    e->state     = OV_QUEUED;
    e->bytes     = (uint8_t *)malloc(size);
    if (!e->bytes) { s_count--; return; }
    memcpy(e->bytes, bytes, size);

    /* A-1: check if a compiled DLL exists in cache for these bytes. */
    overlay_loader_check_cache(load_addr, size, bytes);
    /* A-2: push onto compilation queue (to be added in A-2). */
}

/* ---- JSON output — assembled dirty_ram regions -------------------------- */

/* Emit every dirty-page run within [win_lo_page, win_hi_page) as a capture
 * region. Runs are clamped to the window so region_start keys are stable
 * across window boundaries: the kernel window must never merge with
 * boot-text pages (both can be dirty and adjacent — kernel via CPU stores,
 * boot-text via overlay CD DMA), and a boot-text run must never merge
 * across the overlay-region floor (existing [FLOOR,...) DLL keys would
 * shift). Dirty boot-text runs ARE captured — post-baseline they are live
 * overlay code (see dirty_ram_interp.h window model). */
static void write_json_window(FILE *f, uint32_t win_lo_page,
                              uint32_t win_hi_page, int *first_region,
                              const uint32_t *bitmap,
                              const uint32_t *dispatch_pc_bitmap,
                              const uint32_t *dispatch_seg_bitmap,
                              const uint32_t *exec_pc_bitmap,
                              const uint8_t *ram_base)
{
    uint32_t page_sz = 4096u;
    uint32_t page, run_start;
    int      in_run;

    in_run    = 0;
    run_start = 0;

    for (page = win_lo_page; page <= win_hi_page; page++) {
        int dirty = 0;
        if (page < win_hi_page) {
            uint32_t word = bitmap[page >> 5];
            dirty = (word >> (page & 31u)) & 1u;
        }

        if (dirty && !in_run) {
            in_run    = 1;
            run_start = page;
        } else if (!dirty && in_run) {
            uint32_t phys = run_start * page_sz;
            uint32_t size = (page - run_start) * page_sz;
            /* Carry one coherent guard instruction beyond a dirty-page run.
             * A MIPS branch/jump at the final word of the run (...FFC) always
             * executes its delay slot in the next page (...000).  Supplying
             * that word lets overlay codegen emit exact semantics and lets its
             * range manifest hash/watch the slot. Keep the established region
             * start. A four-byte overlap across an artificial kernel/boot or
             * boot/overlay capture-window boundary is required: those windows
             * stabilize region keys, but they are not MIPS execution barriers.
             * Adjacent dirty pages were already folded into this run. */
            const uint32_t ram_size = psx_ram_live_bytes();
            uint32_t guard_bytes = 0u;
            if (phys <= ram_size && size <= ram_size - phys &&
                phys + size <= ram_size - 4u) {
                size += 4u;
                guard_bytes = 4u;
            }
            uint32_t virt = PSX_OVERLAY_CODE_SEGMENT | psx_ram_canonical_offset(phys);
            in_run = 0;

            /* The readable delay-slot guard is not executable evidence. */
            const uint32_t pc_hi = phys + size - guard_bytes;

            /* Seeds: only per-PC interpreter hits — execution-verified. */
            int nexec = 0;
            int ndisp = 0;
            for (uint32_t ep = phys; ep < pc_hi; ep += 4u) {
                uint32_t wi = ep >> 2;
                if ((dispatch_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u)
                    ndisp++;
                if ((exec_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u)
                    nexec++;
            }

            /* Skip pure-data regions — nothing to compile. */
            if (ndisp == 0 && nexec == 0) {
                continue;
            }

            /* Execution-time capture (§2.2 fix): snapshot the overlay region
             * from LIVE RAM, i.e. the bytes as they ACTUALLY EXECUTE — after the
             * game's load-time fixups/relocation. The old code assembled
             * "unpatched" bytes from DMA-time blocks, which omitted relocated
             * jump tables and other fixed-up data: the recompiler then could not
             * resolve `jr`-jump-tables (their address tables were wrong), fell
             * back to call_by_address, and native diverged from the interpreter
             * (the village→overworld blue screen). Live RAM is the faithful
             * image. Generations do not merge: a page's execution evidence is
             * cleared before any DMA or store changes its bytes (see the
             * variant-capture automation comment). */
            if (!*first_region) fprintf(f, ",\n");
            *first_region = 0;

            fprintf(f, "  {\n");
            fprintf(f, "    \"schema\": \"psxrecomp overlay capture v3\",\n");
            fprintf(f, "    \"load_addr\": \"0x%08X\",\n", virt);
            fprintf(f, "    \"size\": %u,\n", size);
            /* How many of the trailing bytes above are the delay-slot guard,
             * i.e. NOT part of the dirty-page run.  The consumer must keep
             * them READABLE (a branch at the run's last word emits its slot
             * from here) while never discovering them as code: the word after
             * a guard word is not in this region at all.  Stated explicitly so
             * the recompiler is TOLD rather than inferring it from `size`
             * (tools/compile_overlays.py capture_guard_bytes ->
             * recompiler/include/ps1_exe_parser.h exe_tag). */
            fprintf(f, "    \"guard_bytes\": %u,\n", guard_bytes);
            fprintf(f, "    \"bytes_b64\": \"");
            write_b64(f, ram_base + phys, size);
            fprintf(f, "\",\n");

            fprintf(f, "    \"executed_pcs\": [");
            int emitted_exec = 0;
            for (uint32_t ep = phys; ep < pc_hi; ep += 4u) {
                uint32_t wi = ep >> 2;
                if (!((exec_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u))
                    continue;
                uint32_t seed_virt = PSX_OVERLAY_CODE_SEGMENT | ep;
                if (emitted_exec++) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "],\n");

            fprintf(f, "    \"dispatch_entry_pcs\": [");
            int emitted_dispatch = 0;
            for (uint32_t ep = phys; ep < pc_hi; ep += 4u) {
                uint32_t wi = ep >> 2;
                if (!((dispatch_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u))
                    continue;
                uint32_t seed_virt = PSX_OVERLAY_CODE_SEGMENT | ep;
                if (emitted_dispatch++) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "],\n");

            /* Schema v3 (docs/SEGMENT_AWARE_CODE.md §5.7): the segments each
             * dispatch entry above entered through, each PC spelled in its
             * segment. dispatch_entry_pcs keeps its v2 meaning and spelling
             * (entered in any segment, written at KSEG0); a reader of a v2
             * capture, which has no such field, takes every entry as KSEG0. */
            fprintf(f, "    \"dispatch_entry_segments\": {");
            for (int seg = 0; seg < PSX_CODE_SEGMENT_COUNT; seg++) {
                const uint32_t *seg_bitmap =
                    dispatch_seg_bitmap + (size_t)seg * DIRTY_RAM_EXEC_BITMAP_WORDS;
                int emitted_seg = 0;
                fprintf(f, "%s\"%s\": [", seg ? ", " : "",
                        psx_code_segment_name(seg));
                for (uint32_t ep = phys; ep < pc_hi; ep += 4u) {
                    uint32_t wi = ep >> 2;
                    if (!((dispatch_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u) ||
                        !((seg_bitmap[wi >> 5] >> (wi & 31u)) & 1u))
                        continue;
                    if (emitted_seg++) fprintf(f, ", ");
                    fprintf(f, "\"0x%08X\"", psx_code_segment_base(seg) | ep);
                }
                fprintf(f, "]");
            }
            fprintf(f, "},\n");

            fprintf(f, "    \"function_entry_pcs\": [],\n");

            fprintf(f, "    \"seeds\": [");
            emitted_dispatch = 0;
            for (uint32_t ep = phys; ep < pc_hi; ep += 4u) {
                uint32_t wi = ep >> 2;
                if (!((dispatch_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u))
                    continue;
                uint32_t seed_virt = PSX_OVERLAY_CODE_SEGMENT | ep;
                if (emitted_dispatch++) fprintf(f, ", ");
                fprintf(f, "\"0x%08X\"", seed_virt);
            }
            fprintf(f, "]\n");
            fprintf(f, "  }");
        }
    }
}

static int write_json_snapshot(const char *path, uint32_t bw,
                               const uint32_t *bitmap,
                               const uint32_t *dispatch_pc_bitmap,
                               const uint32_t *dispatch_seg_bitmap,
                               const uint32_t *exec_pc_bitmap,
                               const uint8_t *ram_base)
{
    FILE    *f;
    uint32_t page_sz;
    int      first_region;
    page_sz = 4096u;
    f = fopen(path, "wb");
    if (!f) return 0;

    fprintf(f, "[\n");
    first_region = 1;

    /* Kernel window, then dirty boot-text, then the overlay region (see
     * dirty_ram_interp.h for the window model). The boot-text window
     * [KERNEL_WINDOW_END, FLOOR) emits only genuinely-overlaid runs: the
     * game-start baseline cleared the EXE-load false positives, so a dirty
     * page there is runtime overlay code by definition. Windows stay
     * separate so runs clamp at the boundaries and region_start keys match
     * try_load_region's clamped walkback (existing overlay-region DLL keys
     * are unchanged). */
    write_json_window(f, 0u,
                      DIRTY_RAM_KERNEL_WINDOW_END / page_sz, &first_region,
                      bitmap, dispatch_pc_bitmap, dispatch_seg_bitmap,
                      exec_pc_bitmap, ram_base);
    write_json_window(f, DIRTY_RAM_KERNEL_WINDOW_END / page_sz,
                      OVERLAY_REGION_FLOOR / page_sz, &first_region,
                      bitmap, dispatch_pc_bitmap, dispatch_seg_bitmap,
                      exec_pc_bitmap, ram_base);
    write_json_window(f, OVERLAY_REGION_FLOOR / page_sz, bw * 32u,
                      &first_region, bitmap, dispatch_pc_bitmap,
                      dispatch_seg_bitmap, exec_pc_bitmap, ram_base);

    fprintf(f, "\n]\n");
    {
        int ok = !ferror(f) && sync_file(f);
        if (fclose(f) != 0) ok = 0;
        return ok;
    }
}

/* Which call of a commit failed, and with what code, for the commit's error
 * line. "additive capture history write failed" and "latest capture write
 * failed" named a file and nothing else, so a run could not say whether the
 * copy or the rename failed, or why. Written and read by
 * capture_commit_temp under s_commit_mutex. Diagnostic text only: nothing
 * branches on it, and the retry of a failed commit is unchanged. */
static char s_commit_failure[200];

/* A C library call failed: errno and, on Windows, the system's own code for
 * the same failure (5 access denied, 32 sharing violation, ...). */
static void capture_note_crt_failure(const char *call)
{
#ifdef _WIN32
    snprintf(s_commit_failure, sizeof(s_commit_failure),
             "%s: errno %d (%s), Windows error %lu", call, errno, strerror(errno),
             (unsigned long)_doserrno);
#else
    snprintf(s_commit_failure, sizeof(s_commit_failure),
             "%s: errno %d (%s)", call, errno, strerror(errno));
#endif
}

/* atomic_replace_file failed: MoveFileExA on Windows, rename elsewhere. */
static void capture_note_replace_failure(const char *call)
{
#ifdef _WIN32
    snprintf(s_commit_failure, sizeof(s_commit_failure),
             "%s: Windows error %lu", call, (unsigned long)GetLastError());
#else
    snprintf(s_commit_failure, sizeof(s_commit_failure),
             "%s: errno %d (%s)", call, errno, strerror(errno));
#endif
}

static int capture_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    unsigned char buf[65536];
    size_t n;
    int ok = 1;
    if (!in) { capture_note_crt_failure("open the snapshot to copy it"); return 0; }
    out = fopen(dst, "wb");
    if (!out) {
        capture_note_crt_failure("create the history temp file");
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            capture_note_crt_failure("write the history temp file");
            ok = 0;
            break;
        }
    }
    if (ferror(in)) {
        if (ok) capture_note_crt_failure("read the snapshot to copy it");
        ok = 0;
    }
    if (ok && (fflush(out) != 0 || CAPTURE_FSYNC(out) != 0)) {
        capture_note_crt_failure("flush the history temp file");
        ok = 0;
    }
    if (fclose(out) != 0) {
        if (ok) capture_note_crt_failure("close the history temp file");
        ok = 0;
    }
    fclose(in);
    if (!ok) remove(dst);
    return ok;
}

static int capture_files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    unsigned char ba[65536], bb[65536];
    int equal = 1;
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || (na && memcmp(ba, bb, na) != 0)) { equal = 0; break; }
        if (na == 0) break;
    }
    if (ferror(fa) || ferror(fb)) equal = 0;
    fclose(fa); fclose(fb);
    return equal;
}

static int copy_file_atomic(const char *src_path, const char *dst_path)
{
    char tmp_path[700];
    unsigned char buf[65536];
    FILE *src, *dst;
    size_t n;
    int ok = 1;
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-p%d", dst_path,
             capture_process_id());
    src = fopen(src_path, "rb");
    if (!src) return 0;
    dst = fopen(tmp_path, "wb");
    if (!dst) { fclose(src); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = 0; break; }
    }
    if (ferror(src)) ok = 0;
    fclose(src);
    if (ok) ok = sync_file(dst);
    if (fclose(dst) != 0) ok = 0;
    if (!ok || !atomic_replace_file(tmp_path, dst_path)) {
        remove(tmp_path);
        return 0;
    }
    return 1;
}

/* Append the canonical JSON array as one minified JSONL record. Whitespace is
 * stripped only outside strings, so bytes_b64 remains byte-for-byte intact.
 * A crash can damage only the final line; every earlier launch remains a valid
 * independently parseable record. */
static int append_history_record(const char *snapshot_path, const char *reason,
                                 uint64_t sig, uint32_t sequence)
{
    FILE *src = fopen(snapshot_path, "rb");
    FILE *dst, *tail;
    int c, in_string = 0, escape = 0, ok = 1;
    int needs_separator = 0;
    if (!src) return 0;
    tail = fopen(s_history_addendum, "rb");
    if (tail) {
        if (fseek(tail, -1, SEEK_END) == 0) {
            c = fgetc(tail);
            needs_separator = c != '\n';
        }
        fclose(tail);
    }
    dst = fopen(s_history_addendum, "ab");
    if (!dst) { fclose(src); return 0; }
    /* Quarantine an incomplete final record from a previous hard kill. */
    if (needs_separator && fputc('\n', dst) == EOF) ok = 0;
    if (ok && fprintf(dst,
            "{\"schema\":\"psxrecomp overlay capture addendum v1\","
            "\"game\":\"%s\",\"session\":\"%s\",\"sequence\":%u,"
            "\"reason\":\"%s\",\"fnv64\":\"%016llX\",\"captures\":",
            s_history_game_id, s_history_session, sequence,
            reason ? reason : "snapshot", (unsigned long long)sig) < 0) ok = 0;
    while (ok && (c = fgetc(src)) != EOF) {
        if (in_string) {
            if (fputc(c, dst) == EOF) { ok = 0; break; }
            if (escape) escape = 0;
            else if (c == '\\') escape = 1;
            else if (c == '"') in_string = 0;
        } else {
            if (c == '"') {
                in_string = 1;
                if (fputc(c, dst) == EOF) { ok = 0; break; }
            } else if (!isspace((unsigned char)c)) {
                if (fputc(c, dst) == EOF) { ok = 0; break; }
            }
        }
    }
    if (ferror(src) || in_string) ok = 0;
    fclose(src);
    if (ok && fprintf(dst, "}\n") < 0) ok = 0;
    if (ok) ok = sync_file(dst);
    if (fclose(dst) != 0) ok = 0;
    return ok;
}

static int write_json_string(FILE *dst, const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    if (fputc('"', dst) == EOF) return 0;
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', dst) == EOF || fputc(*p, dst) == EOF) return 0;
        } else if (*p < 0x20) {
            if (fprintf(dst, "\\u%04X", (unsigned)*p) < 0) return 0;
        } else if (fputc(*p, dst) == EOF) {
            return 0;
        }
    }
    return fputc('"', dst) != EOF;
}

/* Dev persistence already owns an atomically published immutable full JSON
 * snapshot. Append only a durable reference to it instead of embedding the
 * same multi-megabyte capture array again every autocap interval. Production
 * configs without persist_dir continue to use the self-contained v1 record. */
static int append_history_reference(const char *persist_path, const char *reason,
                                    uint64_t sig, uint32_t sequence)
{
    FILE *dst, *tail;
    int c, needs_separator = 0, ok = 1;
    tail = fopen(s_history_addendum, "rb");
    if (tail) {
        if (fseek(tail, -1, SEEK_END) == 0) {
            c = fgetc(tail);
            needs_separator = c != '\n';
        }
        fclose(tail);
    }
    dst = fopen(s_history_addendum, "ab");
    if (!dst) return 0;
    if (needs_separator && fputc('\n', dst) == EOF) ok = 0;
    if (ok && fprintf(dst,
            "{\"schema\":\"psxrecomp overlay capture addendum v2\","
            "\"game\":\"%s\",\"session\":\"%s\",\"sequence\":%u,"
            "\"reason\":\"%s\",\"fnv64\":\"%016llX\",\"snapshot\":",
            s_history_game_id, s_history_session, sequence,
            reason ? reason : "snapshot", (unsigned long long)sig) < 0) ok = 0;
    if (ok) ok = write_json_string(dst, persist_path);
    if (ok && fputs("}\n", dst) == EOF) ok = 0;
    if (ok) ok = sync_file(dst);
    if (fclose(dst) != 0) ok = 0;
    return ok;
}

/* Under s_history_mutex: see persist_history_snapshot. */
static void persist_history_snapshot_locked(const char *snapshot_path,
                                            const char *reason, uint64_t sig)
{
    char persist_path[800];
    uint32_t sequence;
    int addendum_saved;
    int persist_saved = s_history_persist_dir[0] ? 0 : 1;
    if (!s_history_enabled || !sig) return;
    if (s_history_written && sig == s_history_last_sig) return;
    sequence = ++s_history_sequence;

    if (s_history_persist_dir[0]) {
        snprintf(persist_path, sizeof(persist_path),
                 "%s/%s_%s_%04u_%016llX.json",
                 s_history_persist_dir, s_history_game_id, s_history_session,
                 sequence, (unsigned long long)sig);
        if (copy_file_atomic(snapshot_path, persist_path)) persist_saved = 1;
        else fprintf(stderr,
            "psxrecomp: failed to persist overlay capture snapshot %s\n",
            persist_path);
    }
    if (s_history_persist_dir[0] && persist_saved)
        addendum_saved = append_history_reference(
            persist_path, reason, sig, sequence);
    else
        addendum_saved = append_history_record(
            snapshot_path, reason, sig, sequence);
    if (!addendum_saved)
        fprintf(stderr, "psxrecomp: failed to append overlay capture history %s\n",
                s_history_addendum);
    /* Retry an unchanged snapshot if either configured durable sink failed.
     * Repeated addendum records or immutable files are harmless: vault merges
     * are content-keyed and idempotent. */
    if (addendum_saved && persist_saved) {
        s_history_last_sig = sig;
        s_history_written = 1;
    }
}

/* One record at a time, whichever thread committed the snapshot. The lock
 * covers the whole step: the "same snapshot as the last record" check, the
 * sequence number, the copy into the persist folder and the append. So a
 * record is one whole line, the numbers rise in the order of the lines (one is
 * left out only when its write fails), and the check reads what the last
 * record really was. */
static void persist_history_snapshot(const char *snapshot_path,
                                     const char *reason, uint64_t sig)
{
    /* Off, the default: no lock is taken and nothing is written. */
    if (!s_history_enabled || !sig) return;
    if (s_history_mutex) SDL_LockMutex(s_history_mutex);
    persist_history_snapshot_locked(snapshot_path, reason, sig);
    if (s_history_mutex) SDL_UnlockMutex(s_history_mutex);
}

/* Persist one coherent snapshot for three deliberately different roles:
 *   - capture_path is the legacy/latest manifest consumed by existing tools;
 *   - capture_path.d/<content-hash>.json is immutable additive history used
 *     by compile_overlays.py / the coverage vault;
 *   - (opt-in) overlay_captures.addendum.jsonl (+ persist_dir) is a durable
 *     session-tagged history log for auditing/regeneration across restarts.
 * compile_overlays.py unions the first two. Different byte variants at the
 * same load address therefore survive future captures, rebuilds, and
 * regenerations. */
static uint64_t capture_commit_temp(const char *temp_path, const char *reason,
                                    uint64_t sequence)
{
    char contribution_dir[900];
    char contribution[1024];
    char contribution_tmp[1040];
    FILE *probe;
    uint64_t sig = capture_file_sig(temp_path);
    int history_ok = 0;
    int base_ok = 1;
    if (!sig) { remove(temp_path); return 0; }
    if (s_commit_mutex) SDL_LockMutex(s_commit_mutex);

    snprintf(contribution_dir, sizeof(contribution_dir), "%s.d", s_capture_path);
    (void)CAPTURE_MKDIR(contribution_dir); /* already-exists is success */
    for (unsigned collision = 0;; collision++) {
        if (collision == 0)
            snprintf(contribution, sizeof(contribution), "%s/%016llX.json",
                     contribution_dir, (unsigned long long)sig);
        else
            snprintf(contribution, sizeof(contribution), "%s/%016llX-%u.json",
                     contribution_dir, (unsigned long long)sig, collision);
        probe = fopen(contribution, "rb");
        if (!probe) break;
        fclose(probe);
        if (capture_files_equal(temp_path, contribution)) break;
    }
    probe = fopen(contribution, "rb");
    if (probe) {
        fclose(probe); /* exact snapshot already retained */
        history_ok = 1;
    } else {
        snprintf(contribution_tmp, sizeof(contribution_tmp), "%s.%lu.%llu.tmp",
                 contribution, CAPTURE_PID(), (unsigned long long)sequence);
        s_commit_failure[0] = '\0';
        if (capture_copy_file(temp_path, contribution_tmp)) {
            if (atomic_replace_file(contribution_tmp, contribution))
                history_ok = 1;
            else
                capture_note_replace_failure("rename the history temp file into place");
        }
        if (!history_ok) {
            remove(contribution_tmp);
            fprintf(stderr,
                "psxrecomp: ERROR: additive capture history write failed: %s [%s]\n",
                contribution, s_commit_failure);
        }
    }
    /* Snapshots may be formatted concurrently, so only the newest snapshot is
     * allowed to replace the compatibility/latest path. Immutable history is
     * authoritative for additivity and is never discarded by this ordering. */
    if (sequence >= s_latest_commit_seq &&
        !(SDL_AtomicGet(&s_latest_autocap_only) && !strncmp(reason, "preserve", 8))) {
        base_ok = atomic_replace_file(temp_path, s_capture_path);
        if (base_ok) s_latest_commit_seq = sequence;
        else {
            capture_note_replace_failure("replace the latest file");
            remove(temp_path);
            fprintf(stderr, "psxrecomp: ERROR: latest capture write failed: %s [%s]\n",
                    s_capture_path, s_commit_failure);
        }
    } else {
        remove(temp_path);
    }
    if (s_commit_mutex) SDL_UnlockMutex(s_commit_mutex);
    /* A durable immutable contribution is sufficient to advance the evidence
     * epoch. A failed latest-copy update is noisy but cannot lose that history.
     * Persist history from the immutable contribution file (not the racy
     * "latest" path, which another writer can replace at any time), and
     * outside the commit lock since durable-history I/O (an optional
     * persist_dir copy plus a JSONL append) can be comparatively slow. That
     * step has its own lock, s_history_mutex. */
    if (history_ok) persist_history_snapshot(contribution, reason, sig);
    return history_ok ? sig : 0;
}

static uint64_t write_and_commit_snapshot(uint32_t bw,
                                          const uint32_t *bitmap,
                                          const uint32_t *dispatch_pc_bitmap,
                                          const uint32_t *dispatch_seg_bitmap,
                                          const uint32_t *exec_pc_bitmap,
                                          const uint8_t *ram_base,
                                          const char *reason,
                                          uint64_t sequence)
{
    char temp_path[840];
    snprintf(temp_path, sizeof(temp_path), "%s.%lu.%llu.tmp", s_capture_path,
             CAPTURE_PID(), (unsigned long long)sequence);
    if (!write_json_snapshot(temp_path, bw, bitmap, dispatch_pc_bitmap,
                             dispatch_seg_bitmap, exec_pc_bitmap, ram_base))
        return 0;
    return capture_commit_temp(temp_path, reason, sequence);
}

static void capture_executed_pages(uint32_t *bitmap, uint32_t bw,
                                   const uint32_t *exec_pc_bitmap,
                                   uint32_t scope_lo, uint32_t scope_hi,
                                   int include_halo)
{
    const uint32_t ram_size = psx_ram_live_bytes();
    memset(bitmap, 0, (size_t)bw * sizeof(uint32_t));
    if (scope_hi > ram_size) scope_hi = ram_size;
    if (scope_lo >= scope_hi) return;
    uint32_t first_page = scope_lo >> 12;
    uint32_t last_page = (scope_hi - 1u) >> 12;
    uint32_t scan_lo = include_halo && first_page ? first_page - 1u : first_page;
    uint32_t scan_hi = include_halo && last_page + 1u < ram_size / 4096u
                     ? last_page + 1u : last_page;
    for (uint32_t page = scan_lo; page <= scan_hi; page++) {
        uint32_t first_exec_word = page * (4096u / 4u);
        int observed = 0;
        for (uint32_t b = 0; b < 4096u / 4u / 32u; b++) {
            if (exec_pc_bitmap[(first_exec_word >> 5) + b]) {
                observed = 1;
                break;
            }
        }
        if (observed && (page >> 5) < bw)
            bitmap[page >> 5] |= 1u << (page & 31u);
    }
}

static uint64_t overlay_capture_write_current(const char *reason,
                                              uint32_t scope_lo,
                                              uint32_t scope_hi,
                                              int include_halo)
{
    extern uint8_t *memory_get_ram_ptr(void);
    uint32_t bw = dirty_ram_get_bitmap_word_count();
    uint32_t *bitmap;
    uint64_t sig;
    if (!s_active) return 0;
    bitmap = (uint32_t *)malloc((size_t)bw * sizeof(uint32_t));
    if (!bitmap) return 0;
    capture_executed_pages(bitmap, bw, g_dirty_ram_exec_pc_bitmap,
                           scope_lo, scope_hi, include_halo);
    sig = write_and_commit_snapshot(bw, bitmap,
                                    g_dirty_ram_dispatch_pc_bitmap,
                                    &g_dirty_ram_dispatch_seg_bitmap[0][0],
                                    g_dirty_ram_exec_pc_bitmap,
                                    memory_get_ram_ptr(),
                                    reason,
                                    capture_next_sequence());
    free(bitmap);
    return sig;
}

void overlay_capture_write_json(void)
{
    /* Sticky dirty pages describe everything written since boot. They do not
     * describe a coherent code variant: mutable data between two code pages
     * can otherwise merge them into a giant region that changes every run.
     * Final/manual captures use the same executed-page scope as autocapture. */
    (void)overlay_capture_write_current("shutdown-or-manual",
                                        0, psx_ram_live_bytes(), 0);
}

static void overlay_capture_before_dma_impl(uint32_t load_addr, uint32_t size);
void overlay_capture_before_dma(uint32_t load_addr, uint32_t size)
{
    const uint64_t span_t0 = host_span_begin();
    overlay_capture_before_dma_impl(load_addr, size);
    host_span_end(HSPAN_CAPTURE_BEFORE_DMA, span_t0, load_addr);
}

static void overlay_capture_before_dma_impl(uint32_t load_addr, uint32_t size)
{
    if (!s_enabled || !s_active || size == 0) return;
    uint32_t lo = psx_ram_canonical_offset(load_addr);
    uint32_t hi = lo + size;
    if (lo >= psx_ram_live_bytes()) return;
    if (hi > psx_ram_live_bytes() || hi < lo) hi = psx_ram_live_bytes();

    /* Snapshot complete pages touched by this DMA. Page scope prevents sticky
     * dirty runs from turning a small sector replacement into a multi-megabyte
     * journal, while whole-page evidence catches a write to one half of a page
     * before it can hybridize code executed in the other half. */
    const uint32_t page_shift = 12u;
    uint32_t first_page = lo >> page_shift;
    uint32_t last_page = (hi - 1u) >> page_shift;
    uint32_t evidence_lo = first_page << page_shift;
    uint32_t evidence_hi = (last_page + 1u) << page_shift;

    /* Preserve an outgoing variant only when code in the affected/mergeable
     * region was interpreted. Clear only that region after the RAM+bitmap copy
     * is queued, leaving unrelated live variants in the current epoch. */
    int observed = 0;
    uint32_t first_word = evidence_lo >> 2;
    uint32_t last_word = evidence_hi >> 2;
    for (uint32_t wi = first_word; wi < last_word; wi++) {
        if ((g_dirty_ram_exec_pc_bitmap[wi >> 5] >> (wi & 31u)) & 1u) {
            observed = 1;
            break;
        }
    }
    if (!observed) return;
    /* Each queued snapshot is a copy of the whole guest RAM. With the queue at
     * its cap (PRESERVE_QUEUE_CAP) the copy is not made: the same pages are
     * committed here, from live RAM, before the DMA changes them. The store
     * gets the same content either way, since history is kept by content hash
     * and not by order. */
    int at_cap = !preserve_queue_has_room();
    if (at_cap || !preserve_snapshot_async(evidence_lo, evidence_hi)) {
        /* Allocation/thread startup failure is rare and already exceptional.
         * Fall back to a synchronous durable commit rather than let the RAM
         * overwrite bind old evidence to new bytes. If storage itself fails,
         * report the unavoidable loss but still clear the stale association. */
        int kept = overlay_capture_write_current("preserve-sync-fallback",
                                                 evidence_lo, evidence_hi, 1) != 0;
        if (kept && at_cap) s_preserve_sync_commits++;
        /* A write that fails at the cap is dropped and counted, not queued
         * past the cap. Queued, the cap would be no bound while the store
         * cannot be written (a read-only folder, a full disk): every disc read
         * would add a copy of guest RAM, and the quit would retry each one.
         * The queue keeps what it holds; the writer still retries those. */
        if (!kept) {
            /* Once: in this state every disc read would print it. The count is
             * in the line at shutdown. */
            if (s_preserve_lost++ == 0)
                fprintf(stderr,
                    "psxrecomp: ERROR: could not preserve outgoing overlay evidence; discarding stale epoch "
                    "(further losses are counted, not printed)\n");
        }
    }
    uint32_t first_bitmap_word = first_word >> 5;
    uint32_t bitmap_words = (last_word - first_word) >> 5;
    memset(&g_dirty_ram_exec_pc_bitmap[first_bitmap_word], 0,
           (size_t)bitmap_words * sizeof(uint32_t));
    dirty_ram_dispatch_evidence_clear(first_bitmap_word, bitmap_words);
    for (uint32_t page = first_page; page <= last_page; page++)
        g_dirty_ram_exec_page_bitmap[page >> 5] &= ~(1u << (page & 31u));
}

int overlay_capture_count(void)
{
    return s_count;
}

/* ---- Variant-capture automation (step 2.8) -------------------------------
 * Trigger = sustained dirty-RAM-interp pressure inside a capture window
 * (g_dirty_window_dispatches), sampled every AUTOCAP_CHECK_FRAMES vblanks,
 * plus a cooldown. A fire may happen while the drive is reading: every
 * vblank is a coherent moment, because a CD DMA clears the execution evidence
 * of the pages it covers before its first word (overlay_capture_before_dma)
 * and every RAM write through psx_write_word, guest store or DMA word, clears
 * its page's evidence before the bytes change (memory.c
 * overlay_watch_note_write). A snapshot only binds evidence to bytes that
 * ran; unexecuted bytes of a page still being filled may be in it, and the
 * loader's CRC over each compiled range rejects code built from them once
 * they change. Titles that keep the drive reading for a whole scene
 * (streamed commentary, a drive parked by re-reading one sector) would
 * otherwise never capture that scene.
 * No fire happens while turbo loads run vblanks faster than real time
 * (turbo_loads_engaged): the interval and cooldown frame budgets would pass
 * in a fraction of their wall time, and captures and background compiles of
 * transient load-time images would compete with the accelerated load. A
 * title that reads at normal pace keeps capturing.
 * On fire: write overlay_captures.json and kick the configured background
 * compile (autocompile.c). Dedup is the tool's job — compile_overlays.py
 * skips image CRCs already in the cache — and the loop self-limits: once
 * the hot code goes native the pressure signal drops to zero and no more
 * captures fire. */
/* Cadence/threshold retune 2026-07-06 (measured, Tomba2 attract): a single
 * interp dispatch can chain ~129K insns, so DISPATCH count under-reports
 * pressure by orders of magnitude — a scene burning >50% of wall time in the
 * interpreter produced ~250 dispatches per old 10s window, hovering at the
 * old 256 gate (triggers effectively never fired mid-scene). Pressure is now
 * EITHER signal: dispatch count (many short stubs) OR interpreted-insn count
 * (few long chains). ~250K insns/window ≈ interp >~2.5% of wall — worth a
 * capture. Faster cadence + short cooldown are safe: fires are still gated
 * on real-time vblanks (no turbo load) and an idle provider, and the loop
 * self-limits as coverage converges (pressure → 0). */
/* There is deliberately NO lifetime trigger cap. A 64-fire "session backstop"
 * shipped 2026-07-06 and was exhausted ~4h into an attract soak, silently
 * freezing coverage growth with interp still ~50% of wall in 2D scenes —
 * convergence IS many fires over a long session. The runaway case a cap was
 * papering over (pressure that never converts: excluded PCs, failing shards
 * — fires every cooldown forever, spawning futile provider runs) is handled
 * by FUTILITY BACKOFF instead: a fire is skipped when the capture manifest
 * is byte-identical to the last compile request AND that request registered
 * no new candidates; skips retry on exponential backoff and any change in
 * either signal resets to normal cadence. Concurrency is already bounded
 * (cp->busy defers fires while a compile is in flight) and identical images
 * dedup downstream (compile_overlays.py skips cached CRCs). */
#define AUTOCAP_CHECK_FRAMES    120u   /* ~2 s between pressure samples     */
#define AUTOCAP_MIN_DISPATCHES  128u   /* catches ~120/s FMV helper gaps    */
#define AUTOCAP_MIN_INSNS       100000ull /* long compute gap pressure gate */
#define AUTOCAP_COOLDOWN_FRAMES 300u   /* >= ~5 s between auto-fires        */
/* Host-time twins of the two frame gates, at their 60 Hz equivalents. Each
 * gate opens on whichever budget expires FIRST: frame counting alone
 * deadlocks recovery, because the stall autocapture exists to fix is exactly
 * when vblanks stop arriving (at 0.26 fps, 120 frames is ~8 minutes of wall
 * clock). At full speed the budgets coincide, so cadence is unchanged. */
#define AUTOCAP_CHECK_MS        2000ull
#define AUTOCAP_COOLDOWN_MS     5000ull
#define AUTOCAP_BACKOFF_MAX     64u    /* futile-retry ceiling: 64*5s ≈ 5min */
#define AUTOCAP_WRITE_RETRY_MAX_FRAMES 60u /* cap failed-I/O retry at ~1 s */
#define AUTOCAP_REPLAY_WRITE_ATTEMPTS 3u
#define AUTOCAP_REPLAY_PROVIDER_FAILURES 3u

static int      s_autocap_enabled    = 0;
static uint64_t s_autocap_last_check = 0;
static uint64_t s_autocap_last_fire  = 0;
static uint64_t s_autocap_last_disp  = 0;
static uint64_t s_autocap_last_insns = 0;
static uint32_t s_autocap_triggers   = 0;
static uint64_t s_autocap_last_delta = 0;
static uint64_t s_autocap_last_insns_delta = 0;
static uint64_t s_autocap_next_ok    = 0;    /* frame gate for next attempt  */
static uint64_t s_autocap_sig_at_req = 0;    /* manifest FNV at last request */
static int      s_autocap_reg_at_req = -1;   /* loader candidates at last req */
static uint32_t s_autocap_backoff    = 1;    /* cooldown multiplier (futile)  */
static uint32_t s_autocap_futile     = 0;    /* futile skips (telemetry)      */
static uint64_t s_autocap_provider_sig_pending = 0;
static uint64_t s_autocap_provider_retry_frame = 0;
static unsigned s_autocap_provider_attempts = 0;
static uint32_t s_autocap_provider_initial_failures = 0;
static uint64_t s_autocap_last_check_ms = 0; /* host twin of last_check      */
static uint64_t s_autocap_next_ok_ms    = 0; /* host twin of next_ok         */
static void (*s_autocap_observer)(void);
static int s_autocap_replay_paced;

static uint64_t autocap_now_ms(void) { return (uint64_t)SDL_GetTicks64(); }

/* Wedge visibility: which pre-sample gate the tick last returned at, and
 * when the pressure sample last completed. A latched gate is silent from
 * outside — triggers, futility and pressure counters simply stop moving —
 * so record it and let autocompile_status report it. */
static uint64_t s_autocap_last_sample_ms = 0;
static int      s_autocap_gate_hit       = 0;

/* Gate identifiers, in the order overlay_autocapture_tick() tests them. */
enum {
    AUTOCAP_GATE_NONE = 0,      /* reached the pressure sample          */
    AUTOCAP_GATE_WRITE_JOIN,    /* worker finished, snapshot requeued   */
    AUTOCAP_GATE_WRITE_RETRY,   /* manifest write failed, retrying      */
    AUTOCAP_GATE_PROVIDER,      /* compile request pending              */
    AUTOCAP_GATE_DISABLED,      /* autocapture off / capture inactive   */
    AUTOCAP_GATE_WRITE_BUSY,    /* worker thread still running          */
    AUTOCAP_GATE_INTERVAL,      /* check budget not yet elapsed         */
};

typedef struct {
    uint8_t *ram;
    uint32_t *dispatch_pc_bitmap;
    uint32_t *dispatch_seg_bitmap;   /* [PSX_CODE_SEGMENT_COUNT][words] */
    uint32_t *exec_pc_bitmap;
    uint32_t *bitmap;
    uint32_t bitmap_words;
    uint64_t manifest_sig;
    uint64_t sequence;
    uint64_t retry_frame;
    uint64_t retry_ms;
    unsigned attempts;
} AutocapWriteJob;
static SDL_Thread *s_autocap_write_thread;
static AutocapWriteJob *s_autocap_write_job;

typedef struct PreserveWriteJob {
    AutocapWriteJob snapshot;
    struct PreserveWriteJob *next;
    unsigned attempts;
} PreserveWriteJob;
static SDL_mutex *s_preserve_mutex;
static SDL_cond *s_preserve_cond;
static SDL_Thread *s_preserve_thread;
static PreserveWriteJob *s_preserve_head;
static PreserveWriteJob *s_preserve_tail;
static int s_preserve_stop;

/* The queue of outgoing snapshots had no limit. Each entry holds a copy of
 * the whole 2 MiB of guest RAM and two 64 KiB PC bitmaps, about 2.2 MB, and
 * one low-priority thread writes them out. A game that reads disc sectors
 * into pages where interpreted code runs makes one entry per sector read; in
 * a cold start, where every dispatch is interpreted, that outran the writer,
 * and the process grew from about 1 GB to several GB in seconds. A second
 * start, with the code native, stayed near 1 GB.
 *
 * 64 entries are about 143 MB. That takes an overlay swap of 64 code pages
 * without the emulation thread writing anything, and keeps the worst case
 * near a seventh of what a start uses anyway. Past the cap
 * overlay_capture_before_dma commits on the emulation thread instead.
 *
 * Counts entries that are queued or being written: an entry holds its memory
 * until the writer has committed it. Guarded by s_preserve_mutex. */
#define PRESERVE_QUEUE_CAP 64u
static unsigned s_preserve_cap = PRESERVE_QUEUE_CAP;
static unsigned s_preserve_held;
static unsigned s_preserve_held_peak;
static unsigned s_preserve_enqueued;
#ifdef PSX_OVERLAY_CAPTURE_TEST
static int s_preserve_test_hold;    /* the writer takes no entry while set */
#endif

static int preserve_queue_has_room(void)
{
    int room;
    if (!s_preserve_mutex) return 1;    /* the writer was never started: empty */
    SDL_LockMutex(s_preserve_mutex);
    room = s_preserve_held < s_preserve_cap;
    SDL_UnlockMutex(s_preserve_mutex);
    return room;
}

/* The writer is done with an entry: committed, or given up at shutdown. */
static void preserve_entry_released(void)
{
    SDL_LockMutex(s_preserve_mutex);
    if (s_preserve_held) s_preserve_held--;
    SDL_UnlockMutex(s_preserve_mutex);
}

void overlay_autocapture_set_enabled(int on) {
    s_autocap_enabled = on ? 1 : 0;
}

void overlay_autocapture_get_status(int *enabled, uint32_t *triggers,
                                    uint64_t *last_delta) {
    if (enabled)    *enabled    = s_autocap_enabled;
    if (triggers)   *triggers   = s_autocap_triggers;
    if (last_delta) *last_delta = s_autocap_last_delta;
}

uint64_t overlay_autocapture_last_insns_delta(void) {
    return s_autocap_last_insns_delta;
}

void overlay_autocapture_get_futility(uint32_t *backoff, uint32_t *futile) {
    if (backoff) *backoff = s_autocap_backoff;
    if (futile)  *futile  = s_autocap_futile;
}

/* Wedge report: the latched gate and time since the last completed pressure
 * sample. Sampling is host-time paced, so a long dry stretch means a gate
 * latched — autocapture is wedged and nothing new will ever compile — not
 * that the game is merely slow. (Seen live: a full disk made the manifest
 * write retry forever; the only outward symptom was "the game got slow".) */
const char *overlay_autocapture_gate_name(void) {
    switch (s_autocap_gate_hit) {
    case AUTOCAP_GATE_WRITE_JOIN:  return "write-join";
    case AUTOCAP_GATE_WRITE_RETRY: return "write-retry";
    case AUTOCAP_GATE_PROVIDER:    return "provider-pending";
    case AUTOCAP_GATE_DISABLED:    return "disabled";
    case AUTOCAP_GATE_WRITE_BUSY:  return "write-busy";
    case AUTOCAP_GATE_INTERVAL:    return "interval";
    default:                       return "none";
    }
}

/* Milliseconds since the last completed pressure sample; 0 before the first
 * sample (nothing to conclude yet). */
uint64_t overlay_autocapture_ms_since_sample(void) {
    if (!s_autocap_last_sample_ms) return 0;
    uint64_t now = autocap_now_ms();
    return now > s_autocap_last_sample_ms ? now - s_autocap_last_sample_ms : 0;
}

void overlay_autocapture_get_gates(int *capture_active, int *write_state,
                                   int *write_job_pending,
                                   unsigned *write_job_attempts,
                                   int *provider_pending,
                                   unsigned *provider_attempts) {
    if (capture_active)     *capture_active     = s_active;
    if (write_state)        *write_state        =
        SDL_AtomicGet(&s_autocap_write_state);
    if (write_job_pending)  *write_job_pending  = s_autocap_write_job ? 1 : 0;
    if (write_job_attempts) *write_job_attempts =
        s_autocap_write_job ? s_autocap_write_job->attempts : 0u;
    if (provider_pending)   *provider_pending   =
        s_autocap_provider_sig_pending != 0;
    if (provider_attempts)  *provider_attempts  = s_autocap_provider_attempts;
}

#ifdef PSX_OVERLAY_CAPTURE_TEST
int overlay_capture_test_write_state(void) {
    return SDL_AtomicGet(&s_autocap_write_state);
}
unsigned overlay_capture_test_write_attempts(void) {
    return s_autocap_write_job ? s_autocap_write_job->attempts : 0u;
}
int overlay_capture_test_provider_pending(void) {
    return s_autocap_provider_sig_pending != 0;
}
/* Runs the history copy and returns its failure text ("" on success). */
const char *overlay_capture_test_copy_file(const char *src, const char *dst) {
    s_commit_failure[0] = '\0';
    (void)capture_copy_file(src, dst);
    return s_commit_failure;
}
/* The queue of outgoing snapshots: a cap of 0 restores the built-in one. */
void overlay_capture_test_set_preserve_cap(unsigned cap) {
    if (!s_preserve_mutex) s_preserve_mutex = SDL_CreateMutex();
    SDL_LockMutex(s_preserve_mutex);
    s_preserve_cap = cap ? cap : PRESERVE_QUEUE_CAP;
    SDL_UnlockMutex(s_preserve_mutex);
}
unsigned overlay_capture_test_preserve_cap(void) { return s_preserve_cap; }
/* While held the writer takes no entry, so a test fills the queue exactly. */
void overlay_capture_test_hold_preserve_writer(int hold) {
    if (!s_preserve_mutex) s_preserve_mutex = SDL_CreateMutex();
    if (!s_preserve_cond) s_preserve_cond = SDL_CreateCond();
    SDL_LockMutex(s_preserve_mutex);
    s_preserve_test_hold = hold ? 1 : 0;
    SDL_CondSignal(s_preserve_cond);
    SDL_UnlockMutex(s_preserve_mutex);
}
/* Snapshots dropped on the emulation thread; failed writes of the writer
 * thread; entries it gave up at shutdown. Read before
 * overlay_capture_test_preserve_counts, which starts them again. */
void overlay_capture_test_preserve_losses(unsigned *dropped, unsigned *writer_failures,
                                          unsigned *given_up) {
    if (!s_preserve_mutex) s_preserve_mutex = SDL_CreateMutex();
    SDL_LockMutex(s_preserve_mutex);
    if (dropped) *dropped = s_preserve_lost;
    if (writer_failures) *writer_failures = s_preserve_write_failures;
    if (given_up) *given_up = s_preserve_abandoned;
    SDL_UnlockMutex(s_preserve_mutex);
}
/* Entries queued or being written now; the most there ever were; entries ever
 * queued; snapshots committed on the emulation thread at the cap. The call
 * also starts the "ever" counts and the losses again. */
void overlay_capture_test_preserve_counts(unsigned *held, unsigned *peak,
                                          unsigned *enqueued, unsigned *sync_commits) {
    if (!s_preserve_mutex) s_preserve_mutex = SDL_CreateMutex();
    SDL_LockMutex(s_preserve_mutex);
    if (held) *held = s_preserve_held;
    if (peak) *peak = s_preserve_held_peak;
    if (enqueued) *enqueued = s_preserve_enqueued;
    if (sync_commits) *sync_commits = s_preserve_sync_commits;
    s_preserve_held_peak = s_preserve_held;
    s_preserve_enqueued = 0;
    s_preserve_sync_commits = 0;
    s_preserve_lost = 0;
    s_preserve_write_failures = 0;
    s_preserve_abandoned = 0;
    SDL_UnlockMutex(s_preserve_mutex);
}
#endif

/* FNV-1a of the just-written capture manifest. The manifest is a pure
 * function of the capture inputs (live overlay bytes + observed seed-PC
 * sets, both grow-only), so an unchanged hash means a compile request
 * would redo exactly the work of the previous one. */
static void autocap_write_job_free(AutocapWriteJob *job)
{
    if (!job) return;
    free(job->ram); free(job->dispatch_pc_bitmap);
    free(job->dispatch_seg_bitmap); free(job->exec_pc_bitmap);
    free(job->bitmap); free(job);
}

static int autocap_write_thread_main(void *opaque)
{
    AutocapWriteJob *job = (AutocapWriteJob *)opaque;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    job->manifest_sig = write_and_commit_snapshot(
        job->bitmap_words, job->bitmap, job->dispatch_pc_bitmap,
        job->dispatch_seg_bitmap, job->exec_pc_bitmap, job->ram, "autocap",
        job->sequence);
    SDL_AtomicSet(&s_autocap_write_state, 2);
    return 0;
}

static uint64_t autocap_write_retry_delay(unsigned attempts)
{
    uint64_t frames = (uint64_t)(attempts ? attempts : 1u) * 6u;
    return frames < AUTOCAP_WRITE_RETRY_MAX_FRAMES
         ? frames : AUTOCAP_WRITE_RETRY_MAX_FRAMES;
}

static void autocap_write_retry(AutocapWriteJob *job, uint64_t frame)
{
    job->attempts++;
    const uint64_t delay = autocap_write_retry_delay(job->attempts);
    job->retry_frame = frame + delay;
    job->retry_ms = autocap_now_ms() + delay * 1000u / 60u;
}

static void autocap_provider_request_clear(void)
{
    s_autocap_provider_sig_pending = 0;
    s_autocap_provider_retry_frame = 0;
    s_autocap_provider_attempts = 0;
}

/* A durable contribution and a successfully started compiler request are two
 * separate commits. Retain the former's signature until request() accepts it;
 * otherwise a transient CreateProcess/pipe/thread failure would mark identical
 * evidence futile and suppress compilation forever. Returns nonzero while a
 * provider request is still pending. */
static int autocap_provider_request_try(const CodeProvider *cp, uint64_t frame)
{
    if (!s_autocap_provider_sig_pending) return 0;
    /* A playback holds the guest at a recorded boundary until the compiler
     * has made the unit, so frames stop advancing there. A retry counted in
     * frames never came, and the playback stopped with the unit unmade. While
     * a playback paces the captures, retry at every poll; the provider's own
     * backoff, counted in milliseconds, still limits how often it starts. */
    if (frame < s_autocap_provider_retry_frame && !s_autocap_replay_paced) return 1;
    /* busy remains true through AC_DONE until the emulation thread has
     * committed every prepared DLL and completed the additive final rescan. */
    if (cp->busy && cp->busy()) return 1;
    int reg = overlay_loader_registered_count();
    if (s_autocap_provider_sig_pending == s_autocap_sig_at_req &&
        reg == s_autocap_reg_at_req) {
        s_autocap_futile++;
        if (s_autocap_backoff < AUTOCAP_BACKOFF_MAX)
            s_autocap_backoff <<= 1;
        s_autocap_next_ok = frame +
            (uint64_t)AUTOCAP_COOLDOWN_FRAMES * s_autocap_backoff;
        s_autocap_next_ok_ms = autocap_now_ms() +
            AUTOCAP_COOLDOWN_MS * (uint64_t)s_autocap_backoff;
        autocap_provider_request_clear();
        return 0;
    }
    const int unavailable = !cp->request ||
        (s_autocap_replay_paced && cp->available && !cp->available());
    if (!unavailable && cp->request()) {
        s_autocap_backoff = 1;
        s_autocap_sig_at_req = s_autocap_provider_sig_pending;
        s_autocap_reg_at_req = reg;
        s_autocap_triggers++;
        autocap_provider_request_clear();
        return 0;
    }
    s_autocap_provider_attempts++;
    if (s_autocap_replay_paced) {
        const uint32_t failures = cp->failure_count
            ? cp->failure_count() - s_autocap_provider_initial_failures
            : s_autocap_provider_attempts;
        if (unavailable || failures >= AUTOCAP_REPLAY_PROVIDER_FAILURES) {
            fprintf(stderr, "psxrecomp: replay overlay compiler could not start\n");
            autocap_provider_request_clear();
            return 0;
        }
    }
    s_autocap_provider_retry_frame = frame +
        autocap_write_retry_delay(s_autocap_provider_attempts);
    return 1;
}

static int autocap_write_launch(AutocapWriteJob *job)
{
    job->manifest_sig = 0;
    s_autocap_write_job = job;
    SDL_AtomicSet(&s_autocap_write_state, 1);
    s_autocap_write_thread = SDL_CreateThread(autocap_write_thread_main,
                                               "overlay-capture-write", job);
    if (!s_autocap_write_thread) {
        SDL_AtomicSet(&s_autocap_write_state, 0);
        return 0;
    }
    return 1;
}

/* Copy coherent inputs on the emulation thread, then perform base64 formatting,
 * file I/O, and verification reread on a worker. */
static AutocapWriteJob *capture_snapshot_create(uint32_t scope_lo,
                                                uint32_t scope_hi,
                                                int scoped)
{
    extern uint8_t *memory_get_ram_ptr(void);
    const size_t ram_size = psx_ram_live_bytes();
    uint32_t bw = dirty_ram_get_bitmap_word_count();
    AutocapWriteJob *job = (AutocapWriteJob *)calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->ram = (uint8_t *)malloc(ram_size);
    job->dispatch_pc_bitmap = (uint32_t *)malloc(
        sizeof(g_dirty_ram_dispatch_pc_bitmap));
    job->dispatch_seg_bitmap = (uint32_t *)malloc(
        sizeof(g_dirty_ram_dispatch_seg_bitmap));
    job->exec_pc_bitmap = (uint32_t *)malloc(sizeof(g_dirty_ram_exec_pc_bitmap));
    job->bitmap = (uint32_t *)malloc((size_t)bw * sizeof(uint32_t));
    if (!job->ram || !job->dispatch_pc_bitmap || !job->dispatch_seg_bitmap ||
        !job->exec_pc_bitmap || !job->bitmap) {
        autocap_write_job_free(job); return NULL;
    }
    memcpy(job->ram, memory_get_ram_ptr(), ram_size);
    memcpy(job->dispatch_pc_bitmap, g_dirty_ram_dispatch_pc_bitmap,
           sizeof(g_dirty_ram_dispatch_pc_bitmap));
    memcpy(job->dispatch_seg_bitmap, g_dirty_ram_dispatch_seg_bitmap,
           sizeof(g_dirty_ram_dispatch_seg_bitmap));
    memcpy(job->exec_pc_bitmap, g_dirty_ram_exec_pc_bitmap,
           sizeof(g_dirty_ram_exec_pc_bitmap));
    if (scoped) {
        /* DMA preservation includes a one-page executed halo so an ...FFC
         * control transfer and its ...000 delay slot remain coherent. */
        capture_executed_pages(job->bitmap, bw, job->exec_pc_bitmap,
                               scope_lo, scope_hi, 1);
    } else {
        for (uint32_t i = 0; i < bw; i++)
            job->bitmap[i] = dirty_ram_get_bitmap_word(i);
    }
    job->bitmap_words = bw;
    job->sequence = capture_next_sequence();
    return job;
}

static int autocap_write_start(void)
{
    /* Periodic convergence needs executed code, not every sticky dirty page in
     * RAM. Evidence-scoping keeps the background manifest proportional to live
     * coverage and avoids multi-megabyte rewrites every cooldown. */
    AutocapWriteJob *job = capture_snapshot_create(
        0, psx_ram_live_bytes(), 1);
    if (!job) return 0;
    if (!autocap_write_launch(job)) {
        s_autocap_write_job = NULL;
        autocap_write_job_free(job);
        return 0;
    }
    /* The queued job owns this coherent evidence epoch. Start the next one
     * immediately, so later manifests contain newly seen PCs and pages. */
    memset(g_dirty_ram_exec_pc_bitmap, 0, sizeof(g_dirty_ram_exec_pc_bitmap));
    dirty_ram_dispatch_evidence_clear(0u, DIRTY_RAM_EXEC_BITMAP_WORDS);
    memset(g_dirty_ram_exec_page_bitmap, 0, sizeof(g_dirty_ram_exec_page_bitmap));
    return 1;
}

void overlay_autocapture_watch(void (*observer)(void)) {
    s_autocap_observer = observer;
    SDL_AtomicSet(&s_latest_autocap_only,
                  observer || s_autocap_replay_paced == OVERLAY_REPLAY_PACED_AUTOCAPTURE);
}

void overlay_autocapture_replay_set(int paced) {
    if (paced && !s_autocap_replay_paced && s_autocap_write_job)
        s_autocap_write_job->attempts = 0;
    s_autocap_replay_paced = paced;
    SDL_AtomicSet(&s_latest_autocap_only,
                  s_autocap_observer || paced == OVERLAY_REPLAY_PACED_AUTOCAPTURE);
}

int overlay_autocapture_replay_capture(void) {
    return s_autocap_enabled && s_active && !s_autocap_write_job &&
           !s_autocap_provider_sig_pending && autocap_write_start();
}

int overlay_autocapture_replay_has_work(void) {
    extern uint64_t s_frame_count;
    return SDL_AtomicGet(&s_autocap_write_state) != 0 ||
           (s_autocap_write_job &&
            (s_autocap_replay_paced
                 ? s_autocap_write_job->attempts < AUTOCAP_REPLAY_WRITE_ATTEMPTS
                 : s_frame_count >= s_autocap_write_job->retry_frame)) ||
           (s_autocap_provider_sig_pending &&
            (s_autocap_replay_paced ||
             s_frame_count >= s_autocap_provider_retry_frame));
}

static int preserve_write_thread_main(void *opaque)
{
    (void)opaque;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    /* Failed writes in a row, over all entries. At shutdown five of them mean
     * that the store cannot be written: each entry still held then gets one
     * attempt and is given up, with no wait. Without this every entry had
     * five attempts with waits of its own, about 1.6 s each. */
    unsigned fail_streak = 0;
    for (;;) {
        SDL_LockMutex(s_preserve_mutex);
#ifdef PSX_OVERLAY_CAPTURE_TEST
        while ((!s_preserve_head || s_preserve_test_hold) && !s_preserve_stop)
            SDL_CondWait(s_preserve_cond, s_preserve_mutex);
#else
        while (!s_preserve_head && !s_preserve_stop)
            SDL_CondWait(s_preserve_cond, s_preserve_mutex);
#endif
        if (!s_preserve_head && s_preserve_stop) {
            SDL_UnlockMutex(s_preserve_mutex);
            break;
        }
        PreserveWriteJob *job = s_preserve_head;
        s_preserve_head = job->next;
        if (!s_preserve_head) s_preserve_tail = NULL;
        SDL_UnlockMutex(s_preserve_mutex);

        job->snapshot.manifest_sig = write_and_commit_snapshot(
            job->snapshot.bitmap_words, job->snapshot.bitmap,
            job->snapshot.dispatch_pc_bitmap,
            job->snapshot.dispatch_seg_bitmap, job->snapshot.exec_pc_bitmap,
            job->snapshot.ram, "preserve-outgoing", job->snapshot.sequence);
        if (!job->snapshot.manifest_sig) {
            job->attempts++;
            fail_streak++;
            SDL_LockMutex(s_preserve_mutex);
            s_preserve_write_failures++;
            int abandoning = s_preserve_stop && (job->attempts >= 5u || fail_streak >= 5u);
            if (!abandoning) {
                job->next = NULL;
                if (s_preserve_tail) s_preserve_tail->next = job;
                else s_preserve_head = job;
                s_preserve_tail = job;
            } else if (s_preserve_abandoned++ == 0) {
                fprintf(stderr,
                    "psxrecomp: ERROR: outgoing overlay snapshot could not be retained after shutdown retries "
                    "(further ones are counted, not printed)\n");
            }
            SDL_UnlockMutex(s_preserve_mutex);
            if (abandoning) {
                autocap_write_job_free(&job->snapshot);
                preserve_entry_released();
            } else {
                SDL_Delay(job->attempts < 4u ? job->attempts * 100u : 1000u);
            }
            continue;
        }
        fail_streak = 0;
        autocap_write_job_free(&job->snapshot);
        preserve_entry_released();
    }
    return 0;
}

static int preserve_writer_start(void)
{
    if (!s_preserve_mutex) s_preserve_mutex = SDL_CreateMutex();
    if (!s_preserve_cond) s_preserve_cond = SDL_CreateCond();
    if (!s_preserve_mutex || !s_preserve_cond) return 0;
    SDL_LockMutex(s_preserve_mutex);
    if (!s_preserve_thread) {
        s_preserve_stop = 0;
        s_preserve_thread = SDL_CreateThread(preserve_write_thread_main,
                                             "overlay-preserve-write", NULL);
    }
    int ok = s_preserve_thread != NULL;
    SDL_UnlockMutex(s_preserve_mutex);
    return ok;
}

/* Transfer ownership of an already-coherent immutable snapshot to the
 * retrying FIFO. This is also the shutdown path for a periodic job whose
 * formatter/fsync/history commit failed: the live evidence epoch has moved on,
 * so the old bytes must be retried as-is rather than reconstructed or dropped. */
static int preserve_snapshot_enqueue_owned(AutocapWriteJob *snapshot)
{
    PreserveWriteJob *job;
    if (!snapshot || !preserve_writer_start()) return 0;
    job = (PreserveWriteJob *)calloc(1, sizeof(*job));
    if (!job) return 0;
    job->snapshot = *snapshot;
    free(snapshot);

    SDL_LockMutex(s_preserve_mutex);
    if (s_preserve_tail) s_preserve_tail->next = job;
    else s_preserve_head = job;
    s_preserve_tail = job;
    s_preserve_enqueued++;
    if (++s_preserve_held > s_preserve_held_peak)
        s_preserve_held_peak = s_preserve_held;
    SDL_CondSignal(s_preserve_cond);
    SDL_UnlockMutex(s_preserve_mutex);
    return 1;
}

/* Capture coherent bytes before DMA mutates RAM, but keep base64, hashing,
 * copying, and fsync off the emulation thread. The one FIFO writer bounds I/O
 * concurrency; sequence-aware commits also order it against autocapture. */
static int preserve_snapshot_async(uint32_t scope_lo, uint32_t scope_hi)
{
    if (!preserve_writer_start()) return 0;
    AutocapWriteJob *snapshot = capture_snapshot_create(scope_lo, scope_hi, 1);
    if (!snapshot) return 0;
    if (preserve_snapshot_enqueue_owned(snapshot)) return 1;
    autocap_write_job_free(snapshot);
    return 0;
}

void overlay_autocapture_tick(void)
{
    extern uint64_t s_frame_count;
    const CodeProvider *cp = code_provider_active();

    if (SDL_AtomicGet(&s_autocap_write_state) == 2) {
        s_autocap_gate_hit = AUTOCAP_GATE_WRITE_JOIN;
        AutocapWriteJob *job = s_autocap_write_job;
        SDL_WaitThread(s_autocap_write_thread, NULL);
        s_autocap_write_thread = NULL;
        SDL_AtomicSet(&s_autocap_write_state, 0);
        uint64_t sig = job ? job->manifest_sig : 0;
        if (!sig && job) {
            /* The queued snapshot owns the cleared evidence epoch. Keep that
             * immutable job alive and retry it; never merge it back into or
             * clear the newer live epoch that has accumulated meanwhile. */
            autocap_write_retry(job, s_frame_count);
            return;
        }
        s_autocap_write_job = NULL;
        if (sig) {
            s_autocap_provider_sig_pending = sig;
            s_autocap_provider_retry_frame = s_frame_count;
            s_autocap_provider_attempts = 0;
            s_autocap_provider_initial_failures =
                cp->failure_count ? cp->failure_count() : 0;
            (void)autocap_provider_request_try(cp, s_frame_count);
        }
        autocap_write_job_free(job);
        return;
    }

    /* A transient formatter/fsync/history-commit or thread-create failure must
     * not discard an already-cleared evidence epoch. Retry the same immutable
     * snapshot at bounded cadence before considering another periodic fire. */
    if (s_autocap_write_job) {
        s_autocap_gate_hit = AUTOCAP_GATE_WRITE_RETRY;
        AutocapWriteJob *job = s_autocap_write_job;
        // A replay can hold this guest frame while the writer recovers.
        // Stop after repeated I/O failures so a read-only output directory
        // can report a replay error. The job remains available to ordinary
        // capture after the replay ends.
        const int retry_due = s_autocap_replay_paced
            ? job->attempts < AUTOCAP_REPLAY_WRITE_ATTEMPTS &&
                autocap_now_ms() >= job->retry_ms
            : s_frame_count >= job->retry_frame;
        if (SDL_AtomicGet(&s_autocap_write_state) == 0 &&
            retry_due &&
            !autocap_write_launch(job)) {
            autocap_write_retry(job, s_frame_count);
        }
        return;
    }

    if (s_autocap_provider_sig_pending) {
        s_autocap_gate_hit = AUTOCAP_GATE_PROVIDER;
        (void)autocap_provider_request_try(cp, s_frame_count);
        return;
    }

    if (!s_autocap_enabled || !s_active || s_autocap_replay_paced) {
        s_autocap_gate_hit = AUTOCAP_GATE_DISABLED;
        return;
    }
    if (SDL_AtomicGet(&s_autocap_write_state) != 0) {
        s_autocap_gate_hit = AUTOCAP_GATE_WRITE_BUSY;
        return;
    }
    const uint64_t now_ms = autocap_now_ms();
    if (s_frame_count - s_autocap_last_check < AUTOCAP_CHECK_FRAMES &&
        now_ms - s_autocap_last_check_ms < AUTOCAP_CHECK_MS) {
        s_autocap_gate_hit = AUTOCAP_GATE_INTERVAL;
        return;
    }
    s_autocap_last_check = s_frame_count;
    s_autocap_last_check_ms = now_ms;
    s_autocap_last_sample_ms = now_ms;
    s_autocap_gate_hit = AUTOCAP_GATE_NONE;

    uint64_t disp  = g_dirty_window_dispatches;
    uint64_t delta = disp - s_autocap_last_disp;
    s_autocap_last_disp  = disp;
    s_autocap_last_delta = delta;
    uint64_t insns = g_dirty_ram_insns_run;
    uint64_t insns_delta = insns - s_autocap_last_insns;
    s_autocap_last_insns = insns;
    s_autocap_last_insns_delta = insns_delta;

    if (delta < AUTOCAP_MIN_DISPATCHES && insns_delta < AUTOCAP_MIN_INSNS)
        return;
    if (turbo_loads_engaged()) return;             /* vblanks outrun wall time */
    if (cp->busy && cp->busy()) return;
    if (s_frame_count < s_autocap_next_ok && now_ms < s_autocap_next_ok_ms)
        return;                                    /* cooldown (x backoff)  */

    /* Snapshot coherent inputs for the player-shareable coverage manifest;
     * a worker writes/hashes it, then a later vblank applies futility/backoff.
     * Unchanged content with no new candidates backs off without a provider run. */
    s_autocap_last_fire = s_frame_count;
    s_autocap_next_ok = s_frame_count + AUTOCAP_COOLDOWN_FRAMES;
    s_autocap_next_ok_ms = now_ms + AUTOCAP_COOLDOWN_MS;
    if (autocap_write_start() && s_autocap_observer) s_autocap_observer();
}

void overlay_capture_wait_pending(void)
{
    if (s_autocap_write_job) {
        if (s_autocap_write_thread)
            SDL_WaitThread(s_autocap_write_thread, NULL);
        s_autocap_write_thread = NULL;
        AutocapWriteJob *job = s_autocap_write_job;
        s_autocap_write_job = NULL;
        SDL_AtomicSet(&s_autocap_write_state, 0);
        if (!job->manifest_sig && preserve_snapshot_enqueue_owned(job)) {
            /* Owned by the retrying FIFO, which is drained just below. */
        } else {
            if (!job->manifest_sig) {
                fprintf(stderr,
                    "psxrecomp: ERROR: periodic overlay snapshot could not be queued for shutdown retry\n");
            }
            autocap_write_job_free(job);
        }
    }
    if (s_preserve_thread) {
        SDL_LockMutex(s_preserve_mutex);
        s_preserve_stop = 1;
        SDL_CondSignal(s_preserve_cond);
        SDL_UnlockMutex(s_preserve_mutex);
        SDL_WaitThread(s_preserve_thread, NULL);
        s_preserve_thread = NULL;
    }
    if (s_preserve_enqueued || s_preserve_sync_commits || s_preserve_lost)
        fprintf(stdout,
            "psxrecomp: overlay capture: %u outgoing snapshots queued (most at once %u, cap %u), "
            "%u committed on the emulation thread at the cap, %u dropped and %u given up at quit "
            "because the store could not be written\n",
            s_preserve_enqueued, s_preserve_held_peak, s_preserve_cap,
            s_preserve_sync_commits, s_preserve_lost, s_preserve_abandoned);
}

void overlay_autocapture_shutdown(void)
{
    int state = SDL_AtomicGet(&s_autocap_write_state);
    if (state == 0) return;
    if (s_autocap_write_thread) {
        SDL_WaitThread(s_autocap_write_thread, NULL);
        s_autocap_write_thread = NULL;
    }
    /* The worker atomically published and durably archived the coherent
     * snapshot before setting state=2. No compiler request is needed while
     * shutting down; just release its copied inputs before the final snapshot. */
    autocap_write_job_free(s_autocap_write_job);
    s_autocap_write_job = NULL;
    SDL_AtomicSet(&s_autocap_write_state, 0);
}

uint32_t overlay_capture_get_region_crc(uint32_t region_start,
                                         uint32_t region_size)
{
    int j;
    uint8_t *image = (uint8_t *)calloc(region_size, 1);
    if (!image) return 0;

    for (j = 0; j < s_count; j++) {
        OvEntry *blk = &s_entries[j];
        if (!blk->bytes) continue;
        if (blk->load_addr < region_start ||
            blk->load_addr >= region_start + region_size) continue;
        uint32_t off     = blk->load_addr - region_start;
        uint32_t copy_sz = blk->size;
        if (off + copy_sz > region_size) copy_sz = region_size - off;
        memcpy(image + off, blk->bytes, copy_sz);
    }

    uint32_t crc = crc32_compute(image, region_size);
    free(image);
    return crc;
}
