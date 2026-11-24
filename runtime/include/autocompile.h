/* autocompile.h — background overlay compile for variant-capture automation
 * (step 2.8). Spawns the configured compile command (compile_overlays.py
 * invocation from game.toml [runtime] overlay_autocompile_cmd) after an
 * automatic capture, collects its output into an in-memory ring (no log
 * files — CLAUDE.md §3; read it via the autocompile_status TCP command),
 * and on success has the emu thread rescan the overlay-DLL cache so the new
 * variant goes native in-session, without a restart. */
#ifndef PSXRECOMP_AUTOCOMPILE_H
#define PSXRECOMP_AUTOCOMPILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Store the command line + working directory. Empty cmd disables. */
void autocompile_configure(const char *cmd, const char *cwd);
int  autocompile_configured(void);

/* Pin the canonical overlay cache dir + captures file the LOADER actually uses
 * (<exe>/cache and <exe>/overlay_captures.json — computed once in main.cpp).
 * autocompile injects these into every spawned compile via the environment
 * (PSX_OVERLAY_CACHE_DIR / PSX_OVERLAY_CAPTURES), so the WRITE cache and the
 * READ captures can never diverge from where the loader reads — for every game,
 * dev or prod, gcc or tcc. compile_overlays.py / coverage_vault.py honor these
 * over any CLI --out-dir/--captures. This is the single source of truth for the
 * cache location: no per-game config, no drift. */
void autocompile_set_cache_paths(const char *cache_dir, const char *captures);

/* Is the staged overlay toolchain the one THIS runtime was built with?
 *
 * The shard cache directory carries the codegen hash (gcc/<abi>/cg<N>_<hash>_...). A toolchain
 * staged from an older emitter writes its shards under its own hash, a directory this runtime
 * never reads: every compile succeeds, nothing is ever loaded, every overlay stays interpreted
 * and the same regions are recompiled for the whole session (LoD playtest 2026-10-12: 27
 * compile runs, 0 shards loaded). Reads <toolchain_include_dir>/overlay_codegen_hash.h and
 * compares its PSX_OVERLAY_CODEGEN_HASH with `runtime_hash`.
 * Returns 1 when usable or unknown (no/unparseable header, or runtime_hash == 0); returns 0 and
 * records a degraded reason (autocompile_status "degraded_reason") on a mismatch. The caller
 * must then NOT configure the bundled toolchain command. */
int autocompile_check_toolchain_hash(const char *toolchain_include_dir, uint32_t runtime_hash);

/* Probe whether a C compiler is actually reachable on PATH (gcc/cc/clang) — the
 * REAL "developer machine" signal, distinct from autocompile_configured() which
 * only reports that a command STRING is set (the shipped game.toml always sets
 * one). overlay_backend_resolve uses this so `auto` picks gcc only when the
 * toolchain can really build a shard, else tcc (toolchain-less production).
 * Memoized; safe to call repeatedly. */
int  autocompile_toolchain_available(void);

/* 1 while a compile process is running. */
int  autocompile_busy(void);

/* Owner-thread startup/teardown gate. Sealing rejects any child, worker or
 * queued publication; callers must join/discard with shutdown first. */
int autocompile_session_set_interpreted(int sealed);
int autocompile_session_interpreted_sealed(void);

/* Kick a compile if configured and idle. Returns 1 if started. */
int  autocompile_request(void);

/* Actual start and completion failures. Read on the emulation thread. */
uint32_t autocompile_failure_count(void);

/* Emu-thread tick: applies a finished compile (cache rescan on success).
 * Must be called from the same thread that owns the overlay loader. */
void autocompile_poll_main(void);

/* Process-shutdown teardown, emu thread only: stops the publication pipeline,
 * kills the compiler process tree (job object), and JOINS the watcher and
 * preparer threads — waiting out any in-flight LoadLibrary rather than
 * abandoning a thread inside the Windows loader lock (which can deadlock
 * ExitProcess). Safe to call in any state, including mid-run; idempotent. */
void autocompile_shutdown(void);

/* JSON status blob for the debug server: state, run/fail counters, last
 * exit code, and the output tail. Returns bytes written. */
int  autocompile_status_json(char *out, int cap);

/* The overlay compile results of this start, as one JSON object for the run
 * report (psx_last_run_report.json, key "overlay_compile"). A product has no
 * debug server, so this is how a start says that units failed to compile and
 * ran interpreted. Fields:
 *
 *   configured, state            is a compile command set; idle/running/done
 *   consistent                   0 when the copy was taken without the lock
 *   runs, runs_failed            driver runs started; failures: a run that ended
 *                                badly or a driver that could not be started,
 *                                so runs_failed can be larger than runs
 *   runs_with_result             runs that printed their PSX_SHARD_RESULT line
 *   runs_stopped_at_quit         runs that a normal quit stopped; a result line
 *                                such a run had printed is counted
 *   units_compiled, units_failed, units_skipped
 *                                those result lines, summed over the start
 *   units_attempted              units_compiled + units_failed
 *   fail_lines                   "SHARD FAIL" lines seen, counted as they
 *                                arrive: it includes a run that the end of
 *                                the start cut short, which has no result line
 *   failure_classes              up to failure_classes_max entries:
 *                                class, count, first (the first SHARD FAIL
 *                                line of the class), first_error (the
 *                                compiler's first error line for it, or "")
 *   fail_lines_in_unnamed_classes  failures in classes past that bound
 *   output_tail                  the last output_tail_max bytes of the latest
 *                                run's output
 *
 * Each text is cut at failure_text_max bytes. A unit that fails in two runs is
 * counted twice. Returns the bytes written; the object is always complete
 * ("{}" when cap is too small). It never waits for a lock: it is called from
 * the crash handler. */
int  autocompile_report_json(char *out, int cap);

/* Why overlay autocompile cannot work, or NULL if nothing is known to be wrong.
 *
 * When this is non-NULL, no shard will ever be compiled and overlay execution
 * stays in the interpreter — the run is valid but its performance is
 * meaningless. Callers that report timings should say so rather than publish a
 * number measured in that state.
 *
 * This exists because the equivalent warnings are written to stdout and the
 * shipped runtime links -mwindows, so they reach nobody. `autocompile_status`
 * carries this out over the TCP debug server as "degraded"/"degraded_reason". */
const char *autocompile_degraded_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_AUTOCOMPILE_H */
