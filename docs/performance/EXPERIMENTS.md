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
