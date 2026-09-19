# Cycle-isolation micro-benchmark test ROM (ruler #2)

A purpose-built, in-tree, version-controlled cycle test program — the
**integration-test ecosystem** for the faithful R3000A cost model
(FAITHFUL_TIMING_PLAN.md §3c, ACCURACY_BURNDOWN.md axis 2). Complements ruler #1
(the BIOS-kernel function ruler) by isolating ONE cost component per loop, which
organic BIOS/game code can't (it mixes components in a single basic block).

This is game-independent: it validates the shared `psx_instr_base_cycles` +
memory-path model that EVERY recompiled PSX title uses, not just the current
bring-up vehicle.

## What it is

`gen_testrom.py` emits `cycle_testrom.exe`, a tiny PS-X EXE of counted loops.
Each loop's top is a basic-block leader (branch target), so a cyc_watch
single-anchor consecutive-hit Δ = exactly one iteration's cycle cost — the same
measurement on both backends.

Loops (each adds ONLY its op(s) over the shared loop overhead):
- `baseline`   — loop overhead only (addiu; bne; nop)
- `alu`        — + one addu (pure execute)
- `load`       — + one lw from main RAM (memory read wait-state)
- `load2`      — + two lw (linearity check)
- `div`        — + divu, mflo (div latency + stall-on-read, worst case)
- `div_spaced` — + divu, 2 filler addu, mflo (stall partly absorbed)
- `mult`       — + multu, mflo (mult latency)
- `gte_rtps`   — + RTPS cmd, mfc2 (GTE per-command stall; cost 15)
- `gte_nclip`  — + NCLIP cmd, mfc2 (GTE per-command stall; cost 8)

**Isolation by baseline subtraction:** (component_per_iter − baseline_per_iter)
= the component's cost; loop overhead AND instruction fetch (both fully
cache-resident after warm-up) cancel.

## Two-backend measurement

Same EXE, both backends, compare per-iteration Δ:
- **native** (psx-runtime cost model): recompiled by `psxrecomp-game`; the game
  emitter emits `debug_server_cyc_observe` at every block leader (debug builds),
  so each anchor is observable. (Native delivery harness: see "Native side" TODO.)
- **Beetle** (HW oracle): mednafen sideloads the raw PS-EXE (it auto-detects the
  "PS-X EXE" header). cyc_watch the same anchors.

Each component is then transcribed/calibrated into `psx_instr_base_cycles`
(execute latencies: mult/div, GTE) or the memory-path wait-state (`memory.c`),
Δ-gated against Beetle on these loops + FMV-no-regression, one at a time.

## Boot disc (how the EXE reaches both backends)

The real BIOS only boots a disc that carries the PlayStation license region; a
license-less disc drops to the BIOS shell (Memory-Card/CD-Player menu). So the
test EXE ships on a synthetic disc built with the in-tree `tools/mkpsxiso`:

```bash
cd tools/cycle_testrom
python gen_testrom.py cycle_testrom.exe                    # EXE + .anchors.json
../../recompiler/build-t2/psxrecomp-game.exe --config game.toml   # recompile (native)

# ONE-TIME: extract the license region from a disc YOU OWN (local only, never
# redistributed). dumpsxiso writes license_data.dat from the disc system area:
DUMP=../mkpsxiso/mkpsxiso-2.20-win64/dumpsxiso.exe
"$DUMP" -x /tmp/own_disc -s /tmp/own_disc/x.xml "<path to a PS1 disc you own>.cue"
cp /tmp/own_disc/license_data.dat disc/license_data.dat    # gitignored, stays local

# build the disc (embeds license + SYSTEM.CNF + cyctest.exe):
cd disc && ../../mkpsxiso/mkpsxiso-2.20-win64/mkpsxiso.exe -y cyctest.xml
```

`disc/license_data.dat` and the built `disc/*.bin`/`*.cue` are **gitignored** —
copyrighted Sony data, local only. Everything else (gen, xml, SYSTEM.CNF) is
tracked, so each developer reproduces the disc from a disc they own.

## Measure

```bash
# both backends boot the same disc/cyctest.cue (psx-cyctest = the native test-runtime, port 4600):
#   beetle: psx-beetle <bios> --disc disc/cyctest.cue --port 4382
#   native: psx-cyctest --no-launcher --game game.toml --bios <bios> --disc disc/cyctest.cue
python measure.py --port 4382      # Beetle oracle: per-iter + per-component delta
python measure.py --port 4600      # native cost model (compiled backend)
```

IMPORTANT: launch psx-cyctest via PowerShell `Start-Process` (a bash `&` launch
fails to boot — pc=0). cyc_watch / freeze_check sampling must be done AT STEADY
STATE: query AFTER the BIOS has booted to the EXE and the loops are running
(`measure.py`'s own wait handles this) — an early sample reports warm-up values
(Beetle's boot window) or, for the interp path, dirty_ram_insns=0 because the
EXE entry hasn't been reached yet.

## Interp-path measurement (PSX_FORCE_INTERP)

Both backends share one cost model (`psx_cyc_*`, psx_cyc.h), but `measure.py
--port 4600` above measures the COMPILED backend. To measure the dirty-RAM
INTERPRETER on the same loops (isolated interp-vs-Beetle Δ — not by-construction),
launch psx-cyctest with `PSX_FORCE_INTERP=1`:

```powershell
$env:PSX_FORCE_INTERP='1'; Start-Process psx-cyctest.exe -ArgumentList ...
```

`PSX_FORCE_INTERP=1` makes `dirty_ram_is_dirty()` (runtime/src/memory/memory.c) report all
RAM above the kernel window as dirty, so the dispatcher routes the test ROM through
the dirty-RAM interpreter (the SAME path overlays take) instead of the compiled
image — no emitter/dispatch change. Confirm it engaged via freeze_check
`dirty_ram_insns` climbing (hundreds of millions during a measure run).

**Interp == Beetle EXACT on all 12 components (2026-06-27):** baseline/alu/load/
load2/load_use/div/div_spaced/mult/gte_rtps/gte_nclip/gte_read_use/ld_div all match
the oracle, so the interpreter's per-instruction interlock model is MEASURED equal
to the compiled backend and to Beetle, not merely shared-by-construction.

## I-cache fetch (icache_miss loop + PSX_ICACHE)

The `icache_miss` loop forces an I-cache miss every iteration: its top and a victim
block 0x1000 bytes away map to the SAME direct-mapped line (4 KB / 256-line cache,
index = addr bits 4-11) with different tags, so each fetch evicts the other. (Every
other loop is small enough to be all-hits after warm-up — fetch cost 0.)

The faithful R3000A I-cache fetch model lives in `runtime/src/cpu/psx_icache.c` (HIT +0,
KSEG1/uncached +4, cached miss +3 + refill from the missing word to the line end —
transcribed from Beetle ReadInstruction). It is **opt-in via `PSX_ICACHE=1`** (default
OFF) until BOTH backends charge it: charging it only in the interp while the compiled
path does not would make the two backends disagree on fetch cost in mixed execution.
Measure the interp path with both envs:

```powershell
$env:PSX_FORCE_INTERP='1'; $env:PSX_ICACHE='1'; Start-Process psx-cyctest.exe -ArgumentList ...
```

**icache_miss == Beetle EXACT (2026-06-27): native-interp +14 == Beetle +14** (per-iter
17). The hit path (the other 12 loops) is unchanged at +0 fetch. So the I-cache
hit AND refill-miss costs are MEASURED equal to the oracle on the interp path.
STAGE 2 (pending): charge the same model in the compiled emitters (per cache-line
leader) and flip PSX_ICACHE on by default; validate ruler #1's cold first-hit spike
(Beetle 84/77 vs steady 56) on the compiled path.

## Beetle ORACLE results (2026-06-26 — the HW cost targets)

Per-iteration cycle delta, and component cost (minus baseline=3):

| loop        | per-iter | component | reads as |
|-------------|----------|-----------|----------|
| baseline    | 3        | —         | addiu+bne+nop, base 1 each |
| alu         | 4        | +1        | one addu (pure execute) |
| load        | 8        | +5        | one main-RAM lw (result unused → no absorb) |
| load2       | 14       | +11       | 2 lw: 5 + 6 (2nd load's +1 = ReadFudge) |
| div         | 41       | +38       | divu+mflo base 2 + ~36 div stall |
| div_spaced  | 41       | +38       | divu+2addu+mflo — fillers ABSORBED by stall |
| mult        | 18       | +15       | multu+mflo base 2 + ~13 mult stall |
| gte_rtps    | 18       | +15       | RTPS cmd + mfc2 stall (gte.cpp RTPS=15) |
| gte_nclip   | 11       | +8        | NCLIP cmd + mfc2 stall (gte.cpp NCLIP=8) |

**GTE per-command stall — VALIDATED EXACT 2026-06-27:** native (compiled path)
gte_rtps +15 == Beetle +15, gte_nclip +8 == Beetle +8. Modeled via
`psx_gte_set`/`psx_gte_stall` (cpu->gte_ts_done), cost table in `psx_cycles.c`
(verified from gte.cpp op returns). All other components unchanged (load2 +10 vs
Beetle +11 = the remaining ReadFudge gap).

Key targets for the native cost model: **div stall ~36, mult stall ~13** (native
charges 0). Load ~5/6 (native memory.c flat +6 — close). **`div_spaced`==`div`
proves the stall must be modeled as a completion-timestamp + stall-on-mflo-read
(fillers absorb it), NOT a flat charge on the divu.**

## Status / TODO

- [x] Generator + hand-encoded MIPS isolation loops; encoding validated by a
      clean recompile (divu→`gpr[10]/gpr[11]`, etc.).
- [x] Universal per-block-leader `debug_server_cyc_observe` in BOTH emitters
      (BIOS `full_function_emitter.cpp` + game `code_generator.cpp`).
- [ ] Native delivery: a test-runtime target (link the generated C) that reaches
      `entry_pc` and free-runs the loops while the debug server serves cyc_watch.
      The boot→entry handoff (boot_state.c snapshot is disc-keyed) is the one
      piece to solve — options: a minimal boot disc, or a direct-entry harness.
- [ ] Beetle delivery: extend `beetle_main.cpp` to sideload a `.exe` content path.
- [ ] `compare.py`: arm each anchor on both backends, report per-component Δ vs
      the analytic expectation.
