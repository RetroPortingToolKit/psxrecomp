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
