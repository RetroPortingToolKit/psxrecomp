# Performance experiment ledger

Campaign: [OpenGL performance campaign](../PERFORMANCE_CAMPAIGN.md).
Live work status: central Beads `beads-jod2`.

Record unsuccessful, incomplete and rejected experiments as well as accepted
ones. Local game-derived artifacts are not release fixtures. Hashes below make
the retained local evidence identifiable without distributing a savestate/ROM.

## CRAB-DISCOVERY-001: verify a usable starting scene

Date: 2026-09-04 (session date). Verdict: exploratory evidence only; no candidate
optimization, A/B comparison, production claim or correctness qualification.

- Host: Windows, Ryzen 7 9800X3D, 8 cores / 16 logical processors. Graphics
  adapters reported by the OS: RTX 3080 Ti, AMD integrated graphics, Meta virtual
  monitor. Actual GL device and power state were not recorded in this capture.
- Historical binary source: `_wt-ape-crabperf-20260901/build-dbg`, CMake Release
  with `PSX_DEBUG_TOOLS=ON`. Existing report identifies build
  `v0.3.2-alpha-139-g1bf70960-dirty`. Current source HEAD is not proof of binary
  provenance. This executable was not built from the campaign branch.
- Executable SHA256:
  `3b27bda4417522eb1ab331e4d2b194828f1d38fb9bbb165d1a78eb2156142ba1`.
- State: `saves-dbg/scph1001/state_800A3660_slot01.pst`, version 5, BIOS wordsum
  `F67ECB99`, entry `800A3660`, codegen hash `A4319B6F`, ABI 21, codegen version 10.
  SHA256 `2dfc142e65eb32e179b67d352f510d15f6d97efd22b3b43a9eef24301f3ad5a9`.
- Executable, saves, assets, BIOS assets and mods were copied into the campaign
  worktree's `.local/crabby-baseline`. Writable cards/settings use that copy.
  CLI selected OpenGL, no launcher, debug port 4680. Settings selected 2x SSAA
  and antialiasing. `PSX_RUNTIME_PERF_DIAG=1`; no host CPU cap was applied.
- Restore receipt: `savestate_status` generation advanced from 0 to 1 with
  `pending=0,last_ok=1,last_op=load,last_slot=1`. Presented screenshot visibly
  confirms the sandcastle area on the beach facing the water.
- Original `ApeEscapeRecomp/saves/scph1001` slots 00/01 were v3 and slots
  06/07/08/09/11 were v2; they cannot be used as v5 checkpoints.

The passive 10-second sample is retained locally at
`.local/crabby-baseline/stall-10s.json`, SHA256
`448d6a77b08479e0cca9bf11ffa21af1b3c2408ba70b9a3f1a0a7696f4a10b28`.
Snapshot start timestamps differ by 10.038749 seconds; commands are sequential,
so these are not atomic snapshots of a fixed amount of guest work.

| Observation | Value | Interpretation limit |
| --- | --- | --- |
| Overlay native dispatch delta | 0 | Does not include statically compiled game execution |
| Overlay interpreter fallback delta | 33,702 | Dispatch events, not CPU-time share |
| Interpreted instruction delta | 425,410 | Some kernel fallback is expected |
| Interpreter block delta | 33,714 | No claim that reducing calls changes the primary bottleneck |
| Compilation runs/configured | 0 / 0 | Cache/compilation provenance differs from a prepared release campaign |
| Rolling phase samples: interpreter / static / GPU | 1.59% / 80.11% / 18.30% | Instrumented phase stamps, not an independent host CPU profiler |

This observation rejects the inference that zero overlay-native dispatches
alone prove interpretation dominates the slowdown. It does not reject overlay
optimization as a candidate. No controlled compiler/process/power-state audit
was performed during this exploratory sample.

Separately queried `frame_perf` reported approximately 16.68 ms total and
16.65 ms `emu_cpu_ms_avg`. The latter is calculated from total wall time minus
present wall time, not OS thread CPU time. GPU queries span command-stream
intervals; do not interpret these fields as independent exclusive CPU/GPU busy
time or add their values together. Run without timer-query profiling and obtain
host-thread CPU samples before choosing an optimization from these values.

## HOST-BUDGET-001: verify the throttle denominator

Date: 2026-09-04 (session date). Verdict: launcher calibration, not an accepted
runtime optimization or a substitute for low-end hardware.

On the same 16-logical-CPU host, a single Python process busy-looped until
`time.process_time()` advanced by 2.5 seconds. Every run used affinity mask
`0x1`. The game was stopped. The sequence below was a calibration sequence,
not an interleaved optimization campaign. Reports live in
`.local/host-calibration/{unrestricted,core50,core25}.json`.

| Requested budget | Applied Job CpuRate (out of 10,000) | Wall seconds | Process CPU seconds |
| --- | --- | --- | --- |
| Unrestricted | none | 2.580994 | 2.500000 |
| 50% of one logical CPU | 313 | 4.679147 | 2.531250 |
| 25% of one logical CPU | 157 | 9.515031 | 2.500000 |

`--cpu-percent` is a percentage of the whole host's CPU scheduling budget.
`--core-percent` normalizes to one logical CPU equivalent independently of
affinity; it limits the whole launched job, including helper threads/processes.
This does not selectively slow the emulation thread, lower CPU frequency, or
simulate an older GPU/cache/memory subsystem. Reports retain requested and
applied rates, including integer rounding.

An earlier 0.5-CPU-second witness did not demonstrate effective throttling even
at a 3% host budget. Longer witnesses did, so sub-second smoke timing is not a
valid cap calibration. Repeat calibration on each host and use sustained game
windows rather than assuming the scheduler's short-term behavior is linear.

## ROUTE-SMOKE-001: live runner protocol validation

Date: 2026-09-04 (session date). Verdict: successful diagnostic smoke only.
No optimization candidate, target-provenance manifest, or acceptance comparison.

Used the isolated executable/state from CRAB-DISCOVERY-001, OpenGL, no host cap,
`PSX_RUNTIME_PERF_DIAG=1`, and explicitly `PSX_GL_PERF=0`. A local shortened
route held neutral input for 120 ticks. The runner confirmed completed slot-1
restore (generation 1, `last_ok=1`) and route completion (`active=false`, one
step consumed, `remaining=0`). Raw evidence:
`.local/crabby-baseline/runner-smoke.json`.

- Route wall time: approximately 2.141 seconds, including setup/status overhead.
- Counter observation window: approximately 2.672 seconds and 161 frame ticks,
  including sequential snapshots/control traffic, not exactly 120 guest ticks.
- GL frame timing correctly reported unavailable with timer queries disabled.
- No native overlay compilation was configured; cache provenance remains
  unsuitable for a prepared-release acceptance baseline.

The process exited after the smoke. Original saves/cards were not modified.
The committed 7,200-tick route is a longer stationary workload manifest; this
smoke does not validate its full-duration timing or the analog approach route.
See [tool usage and remaining gates](TOOLS.md).

## MEMORY-TRACE-001: remove empty production write-trace calls

Dates: 2026-09-04 to 2026-09-05. Verdict: retain as a narrowly scoped dead-work
simplification. Local write benchmark improves; gameplay gain is unproven.
This does not complete the campaign or establish the cause of the beach slowdown.

### Change and correctness evidence

Guard the six RAM/scratchpad byte, halfword and word calls to
`debug_server_trace_write_check` with the existing `PSX_NO_DEBUG_TOOLS` switch.
That function already returns immediately in production. This also removes its
old-value argument loads. Debug builds keep the original calls. No functional
memory observer, store, cycle charge, MMIO path, runtime setting, ABI or state
layout changes. Separate `-O2 -fno-lto` object inspection found the baseline's
undefined trace symbol and confirmed its absence from the candidate object.

`runtime/tests/test_memory_debug_trace_guard.py` compiles actual `memory.c`
against real runtime headers, with external device/observer stubs. It checks
all six writes through four RAM mirrors and scratchpad aliases, expected bytes
and adjacent sentinels, observer/store counts, and production/debug trace-call
witnesses. Optional `--baseline-memory` also checks the old production source.
The compact state hash covers touched bytes and selected counters, not complete
guest/device state. Structural checks preserve functional RAM observer order.
An earlier synthetic store model and then a fake-header fixture were rejected
during review; neither is used by the committed test.

### Build provenance

Framework baseline is `8ec6498d323a7abd04a3058afcbb5e36e62a8ec3`; candidate differs
only by the six guards in `runtime/src/memory.c`. Source SHA256:

- Baseline: `693be507fa53fded3e8e276f4ac0f7293d3879f3d53a8044fa8c383b4e097b99`.
- Candidate: `4fdb391d4f74da1ef2d31ce7cd61a0e0b958abab8304da37d83a2c1bb0e85253`.

Both title executables were rebuilt, using the same copied Ape Escape generated
code and assets from `_wt-ape-crabperf-20260901`. Provenance trees/build logs are
retained under `.local/ape-build-provenance`. CMake Release uses MinGW GCC/G++
15.2.0 from `C:/msys64/mingw64/bin`, `-O3 -DNDEBUG`, debug tools OFF, static runtime ON,
SDL3, recomp UI and rewind ON, netplay OFF, and block cycles ON. Vulkan was
enabled in the build but was not the selected/tested renderer. The first build
failed with the wrong windres shim; successful builds explicitly selected
`C:/msys64/mingw64/bin/windres.exe`.

- A executable SHA256: `f0c893a5743d8dfddcde75fe275d525c99c66294c5333fdf48b797de0b21096e`.
- B executable SHA256: `6fd00dc4ed8a7e9cf66f1bdd64ebaafd376d867dd5687c126bcd56f8e03dbf12`.
- A successful build-log SHA256: `c9a042d2ccab65d248a9b91b0b74d1106fc80fe74f611ea5eccfb27f350ad252`.
- B successful build-log SHA256: `3d5326d5fa5de696cb02fd3c021a2336b92362c04fcfc3f00a560a79ec34ad8e`.

The generated BIOS warned that it was stale against the current emitter
fingerprint. It was deliberately identical in A/B to retain compatibility with
the available v5 checkpoint. No native overlay compilation/loading was
configured. These are controlled local integration artifacts, **not** clean
release builds or qualification of regenerated code and shipped overlay caches.

### Local write benchmark

Actual memory implementation, external device stubs, and a genuinely empty
trace function in a separate translation unit; GCC C11 `-O2 -fno-lto`, production
macro enabled. Each run executes 50 million loops of six writes. Both artifacts
warm up with one million loops before measured A1/B1/A2/B2/A3/B3 order. Host is
the Ryzen 7 9800X3D, High Performance power plan, affinity `0x1`, no CPU cap.
Primary metric is launch-to-completion wall seconds, lower is better.

| Pair | A seconds | B seconds |
| --- | --- | --- |
| 1 | 1.4162425 | 1.1504406 |
| 2 | 1.4181166 | 1.1467513 |
| 3 | 1.4099258 | 1.1160858 |
| Minimum | 1.4099258 | 1.1160858 |
| Median | 1.4162425 | 1.1467513 |

Median improvement: 19.03%; minimum improvement: 20.84%. All six runs produced
identical selected state/counters: `state=dcc19de5`, 300 million stores,
150 million parity calls, 150 million card checks, zero trace calls. This
workload emphasizes write-call overhead and does not predict gameplay gain.
The compiler-process monitor reported no observed compilers; thermal/frequency
telemetry was not collected.

### Crabby Beach OpenGL comparison

Same v5 state as CRAB-DISCOVERY-001, staged copies of saves/cards/settings.
OpenGL context reported NVIDIA 3.3.0 driver 610.74, 2x SSAA and antialiasing,
165 Hz display, wall-clock 59.94 Hz pacing, driver vsync OFF. Both uncapped
smokes sustained approximately 60 guest Hz with no reported audio underruns.
A screenshot outside the acceptance runs confirmed the water-facing beach
scene. Smoke timings are excluded: they are not three pairs and B's screenshot
overlapped its smoke window.

Measured A/B uses `PSX_LOAD_SLOT=1`, `PSX_RUNTIME_PERF_DIAG=1`, `PSX_GL_PERF=0`,
`PSX_BENCH_WINDOW=120:420`. Primary metric is the runtime's exact 300-frame
`BENCH wall_ms`, not launcher lifespan. Every run uses a 25%-of-one-logical-core
Job budget (raw CpuRate 157/10000) and affinity `0xFFFF`, limiting the entire
job. Every process is intentionally stopped after 35 seconds. Timed windows
complete before timeout. This scheduling cap is not low-end hardware emulation.

| Pair | A wall ms | B wall ms |
| --- | --- | --- |
| 1 | 8458.662 | 8588.874 |
| 2 | 8312.503 | 8274.390 |
| 3 | 8341.652 | 7946.127 |
| Minimum | 8312.503 | 7946.127 |
| Median | 8341.652 | 8274.390 |

Median improvement: **0.81%, within observed noise**; minimum improvement:
4.41%. No convincing scene-level gain. Each window reports 180,114 interpreted
instructions, 15,564 fallback dispatches, and zero overlay loads, invalidations,
or captures. Those matching counters are not full-state parity. Under the
severe budget both builds slow down and underflow host audio. No compilers or
monitor errors were observed; configuration/mod hashes remained unchanged.
No screenshots were taken during these six runs. No runs were discarded.

### Retained evidence and remaining gates

All following paths are relative to the worktree and are intentionally ignored:

| Artifact | SHA256 |
| --- | --- |
| `.local/memory-trace-bench/campaign.json` | `fd1944a1fc3a8ea750e37ceda9fb12367d9bb85adc8f12d01fdc28df3a3b06ad` |
| `.local/game-memory-ab/campaign.json` | `9d0c70ae9b9d221bc601e24cf7563dcdc61f84fecbae97806e7a7d068d8b5e1d` |
| `.local/run_memory_bench.py` | `90f6fec928f33f3d5e6395d31ce25d7f86ba16359d79851dd8903ef9ef48057d` |
| `.local/run_game_ab.py` | `e3e9fd310600c3dbde14d4b2b4fff741cee2a063c14bbfc9ffe7f8416f71bff9` |

JSON retains per-run evidence and artifact/configuration hashes; game/state
assets are not distributed. The focused compiled regression passes, as do the
FMV quiet and dirty-text admission/continuation guard tests. The combined
campaign/host-launcher/stall-report suite passes all 63 tests, and CMake's
registration guard finds all 120 runtime/recompiler test files registered.
These are not a full runtime CTest pass or cross-platform validation. Two unrelated
structural tests also fail on the immutable baseline; details are in
[the audit](RUNTIME_AUDIT.md#validation-gaps).

Retain the change only as removal of already-empty production work, under the
contract's simplification allowance, not as a 5% gameplay optimization. Full
checkpoint parity, multi-title/low-end hardware, moving/transition routes,
long-window frame consistency, clean-install and cross-platform release gates
remain open. Broader GPU/audio history and GL costs remain separate candidates;
the flat gameplay result does not justify focusing the campaign on memory.

## PACER-COST-001: attribute idle pacing CPU cost

Date: 2026-09-05. Verdict: baseline attribution only, no pacing change or
gameplay speed claim. Compile actual `frame_pacing.c` and `host_time.c` against
SDL3 with GCC 15.2.0 `-O2 -DPSX_SDL3`. The driver calls `frame_pacer_wait` 600
times at 59.94 Hz with no guest work, using `GetThreadTimes` for calling-thread
CPU time and SDL's performance counter for wall time. The first call anchors
without waiting, so 599 periods take approximately 9.9933 seconds.

| Run | Wall seconds | Thread CPU seconds |
| --- | --- | --- |
| 1 | 9.993298 | 0.812500 |
| 2 | 9.993297 | 0.859375 |
| 3 | 9.993297 | 0.765625 |

This is approximately 7.7-8.6% of one logical CPU spent on idle pacing on the
Ryzen 7 9800X3D, High Performance plan, unrestricted scheduling and inherited
affinity. The existing sleep decision stops sleeping below two milliseconds,
then spins until the deadline. This local cost makes the spin tail worth
measuring in gameplay; it does not establish an actual pacer bug or explain
exclusive wall-time phase attribution under a Job cap. No candidate was tested.
Agents held compilation during these runs; no continuous compiler-process or
thermal monitor was attached to this exploratory probe.

Local retained driver `.local/pacer-cost.c`, SHA256
`d98305437fb4e8c86769f31893e43bf63ade8568d761f522e0842817a92ac105`;
executable `.local/pacer-cost.exe`, SHA256
`7aa80698ca5d3a831d0d651601b3c5b2e8bc3665e2eec82f5e426c308eae7ddb`;
raw output `.local/pacer-cost-results.txt`, SHA256
`749fc5185fa512e3a00390f0022fa8939993763ca3b9f6e123ba76ff6654fea1`.

## HISTORY-002: isolated GPU and audio history candidates

Date: 2026-09-05. Correctness milestone; controlled resource/time comparison
pending. No gameplay gain accepted from the correctness runs below.

### Candidate boundaries

- `GP0-HISTORY-001`: compile the GP0 command-history allocation, recording and
  diagnostic guest-stack scan only when debug tools are enabled. Production
  history accessors remain linkable but report zero capacity/count and empty
  dumps. Keep source tracking, opcode counters, WS census, functional overhang
  detection, and all command execution unchanged. Removes a 104 MiB allocation
  request; debug builds retain always-on history.
- `AUDIO-PCM-001`: compile out four PCM history arrays and their writes in
  `PSX_NO_DEBUG_TOOLS` builds. Preserve every tap's head, nonzero/audible/peak
  statistics, rates, event records and pump/underrun/mute counters. Production
  WAV dump returns the existing error result without opening a file; debug
  WAV history is unchanged. Actual GCC object BSS is 83,886,272 bytes with debug
  history and 16,777,408 without it: exactly 64 MiB less backing capacity.

Both use the existing build switch, not a new runtime selector or lazy allocator.
Capacity is not equivalent to resident memory or a measured speed improvement.
Packed production audio tap counters may share cache lines between callback and
emulation threads; require the real bridge-audio path in performance checks.

### Focused checks

The GPU test compiles actual `gpu.c` and real headers with external renderer
stubs. It compares exact serialized GPU snapshots and the entire VRAM at eleven
checkpoints across environment, fill, upload, copy, polyline and NOP commands.
It also checks debug history count, copy source/stack pointer and polyline
header, and production unavailability. This tests parser/state behavior, not
the rendering correctness of a stubbed backend. Known pre-existing polyline
source attribution was not changed or silently redefined by the test.

The audio test compiles actual `audio_trace.c` in both modes. All four taps'
observed statistics/events compare equally, including a new event after ring
wrap, signed extremes, audible thresholds, rates and resets. Debug WAV headers
and sample bytes are checked for all taps and for wrap/clamped eviction;
production must not create the requested WAV. Review rejected the first test's
hard-coded output comparison and replaced it with real observations.

Both focused tests passed root execution. The 63 campaign/host/stall-report
tests passed, as did the memory-trace regression. CMake's registration guard
finds 122 test files registered. This is not a full cross-platform CTest pass.

### Isolated build provenance

B is the previous memory-guard baseline, C adds only the GP0 gate, and D adds
only the audio gate. All use identical local-only endpoint memory/restore/
renderer/audio witnesses and an optional checkpoint probe. Original B from
MEMORY-TRACE-001 is preserved. Source trees, exact options/commands/logs and
hashes are in `.local/ape-build-provenance/builds-bcd-manifest.json`, SHA256
`176ed3d54939fab3498ced5d1e3dd38566a9ef76e7e4cb23f5514a8d9a740530`.

| Variant | Executable SHA256 |
| --- | --- |
| B metrics | `f1310b19eb3633d81e01717f36ab60f4d6cd4d53aa3838adf0e614d10bbf4914` |
| C GP0 | `74063d9dadef152d023c0dead18eabde854773c4399f7f52f49713fb67315045` |
| D PCM | `e9f525296dfdb0be2a50596a1198bb9735be0371d21afa627cc6e08dfe05bd39` |

Release GCC 15.2.0, SDL3, static runtime, debug tools OFF, rewind ON, netplay OFF,
same donor generated game/BIOS and saved scene as MEMORY-TRACE-001. The stale
generated-BIOS warning persists. A default all-target build failed in an
unrelated donor title test on a long Windows dependency-file path; explicit
`psx-runtime` builds succeeded. No release qualification is claimed.

### Real OpenGL checkpoint comparison

Separate uncapped 20-second processes restore the same slot-1 v5 beach state.
All variants report active OpenGL (`gl_active=1`, `gr_backend=1`), completed
restore (`generation=1,pending=0,last_ok=1,last_op=load,last_slot=1`), and active
non-legacy bridge output with advancing host-tap frames. The local-only probe
writes raw boot-state blobs at frames 120, 121 and 420, plus CPU timing sidecars.
For **both C and D versus B, every blob and sidecar is byte-identical**.

| Frame | Raw guest snapshot SHA256 (all B/C/D) |
| --- | --- |
| 120 | `505f7d94c5c7e518e099669ed1623296d9210abc30eab4bc907956e91ff652c6` |
| 121 | `1e0b19c3a913733aba9bb403b1e01a1c9d309cc919e3accd9c52f3d341d3429b` |
| 420 | `55ebbe6a36ac2674952a4672575dfb619547ffec0a059f9a277bb955b9d074e2` |

Each blob is 3,685,569 bytes. The existing save format covers CPU registers,
RAM, scratchpad, VRAM, SPU RAM and serialized device/IRQ/clock/cache/dirty state.
The 411-byte sidecars add multiply/divide and GTE completion timestamps, all 33
read-absorb bytes and load-timing scalars. CPU function pointers, host audio
queues, SDL/pacer/presentation histories and diagnostics are excluded. This is
one scene's checkpoint evidence, not exhaustive state/restore or multi-title
qualification. Full campaign correctness gates remain open.

Evidence: `.local/history-correctness-002/report.json`, SHA256
`7feb56b0f0119688e460ff46b027d3aaf9549021e996a935830e26a6154b21f2`;
driver `.local/run_history_checks.py`, SHA256
`f6f29b505225fc0ff8688d9a86921be60590a81288980c5fd7d4c8b56961d0f4`.
Raw game-derived snapshots remain local and uncommitted.

The first attempt, `.local/history-correctness-001`, failed closed because the
staged checkpoint hook was absent despite an earlier build report. Explicit
source and binary-string checks plus forced rebuilds corrected the artifact.
That failed attempt is retained, not counted as successful state comparison.

Checkpoint saves can publish deferred cycles and force GL readback. These runs
also overlapped unrelated builds, so **none of their timing or memory values is
acceptance evidence**. The timed runner explicitly unsets checkpoint capture.
The preregistered primary metric is exact-frame-420 process PrivateUsage; the
secondary metric is frames-120:420 wall time, with three interleaved pairs for
each independent candidate and a validated 25%-one-core Job cap. It requires
stable input/artifact hashes, active OpenGL/audio and completed restore, and
aborts on compiler contamination or missing/nonfinite metrics.

### Timing gate outcome and disposition

The benchmark agent monitored the host for ten minutes without finding 30
consecutive seconds free of compiler/benchmark activity. Concurrent SNES builds
included Mega Man X and Super Mario Kart. No timed candidate process was
launched and `.local/game-gp0-memory-ab` was not created. Unrelated work was
not stopped. The power plan remained High performance before and after;
thermal state was not measured.

The prepared twelve-run driver is `.local/run_game_gpu_memory_ab.py`, SHA256
`ce01b28c627dd5a8d0abe3217a49e2fd1d576fc7128153de502487f69a96f64b`.
It includes `cc.exe` in compiler-contamination detection. Resume the paired
campaign in a quiet host window before making retention or speedup claims.

Disposition: committed candidate implementation in the isolated performance
worktree, **pending performance acceptance**, not release qualification. The
104 MiB GP0 allocation request and 64 MiB PCM static-storage capacity removals
are established; resident-memory savings and gameplay speedups are not.
Focused parity tests and real OpenGL checkpoints passed, but the broader
correctness matrix and the quiet-host comparisons remain open.
