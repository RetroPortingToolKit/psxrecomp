# Post-decompression data shards (memoized pure-function replay)

Branch: `spike/tomba-load-shards` (framework) + `spike/load-shards` (TombaRecomp).
Status: **spike in progress** — feasibility experiment.

## The idea

We are a RECOMP, not a DECOMP: we cannot AOT-transform game data the way a
decompilation project can (an .o2r/asset-archive pipeline needs source-level
knowledge of every asset). But we CAN do what we already do for *code*
overlays: **capture on first encounter, replay ever after**. Native code
shards memoize "what does this RAM-resident code compile to"; data shards
memoize "what does this pure function compute".

Target case: Tomba's NOW-LOADING decompression. First run: the decompressor
executes faithfully (native/interp) and we record its effect. Every later
run with byte-identical inputs: skip the execution, apply the recorded
effect. The shard store persists on disk (o2r-like), so a player's second
session has fast loads everywhere they've been.

## Prior art this builds on (and why it dodges the parked spikes)

- [instant-loads round 1](INSTANT_LOADS_PARKED.md): pumping the loader's
  cooperative yields starves the streamer → hardlock. **We don't touch the
  yield structure at all.**
- [instant-loads round 2](INSTANT_LOADS_SPIKE2.md): loads are
  emulation-throughput-bound under turbo; "instant requires making the WORK
  fast". **This is exactly that** — the work becomes a memcmp + memcpy.
- CD adaptive-instant (parked): delivering sectors early desyncs guest
  timing. **We deliver nothing early** — see cycle-faithful replay below.
- Kernel bless (fw 8f23892): byte-verify-then-trust is the established
  soundness pattern. Data shards are the same pattern applied to data
  transforms: **verify the read-set bytes, then trust the recorded
  write-set.**

## Soundness model

A call is memoizable iff it is a deterministic transform of RAM inputs to
RAM outputs. Capture proves this per-shard; replay verifies it per-hit.

**Capture (first encounter, or shard-miss):** execute the call in the
dirty-RAM interpreter with a load/store trace armed (non-exception context
only), from function entry until control returns to the entry `ra` at the
entry `sp` depth. Record:

- **read-set**: every address the call reads *before* writing it
  (read-before-write = the true input), with the bytes observed. Ordered,
  merged into ranges.
- **write-set**: final bytes of every address written (post-state ranges).
- **reg-out**: `v0/v1` (+ anything the ABI lets it define).
- **cycle-cost**: non-exception guest cycles consumed by the call.
- **purity verdict**: any MMIO/scratchpad-IO read, COP0 access, syscall,
  exception-return into the traced frame, or unresolved control transfer
  out of the frame → **not memoizable**, shard poisoned (negative-cached
  for the session, never re-captured this run).

**Replay (subsequent encounter):** look up shards by
`(func_addr, a0..a3, read_set_hash_prefilter)`; byte-compare the full
recorded read-set against live RAM (kernel-bless-style — the compare IS the
proof); on match: apply write-set, set reg-out, **advance the recorded
cycle-cost through the normal psx_advance_cycles/IRQ machinery in slices**,
return to `ra`. On mismatch: fall through to real execution (optionally
capture a new variant — the store is additive per input, like overlay scene
variants).

## Cycle-faithful by default

Replay credits the *recorded* guest cycles through the existing cycle
advance + interrupt-check path. Guest-visible timing is identical to real
execution: the loader thread still spends the same guest time per queue
step, still yields identically, CD cadence untouched, VBlank/IRQ phase
preserved to the slice. All the wedge classes that killed the earlier
spikes were guest-*timing* perturbations; this mechanism perturbs none.

The win is host wall time under `turbo_loads`: the pig window fast-forwards
guest time at host speed, and the dominant host cost inside the window (if
profiling confirms: decompression execution) becomes ~zero. An "instant"
mode (credit ~0 cycles) is a later, opt-in enhancement — NOT the default,
and not needed for the primary win.

Known fidelity delta (documented, accepted for the spike): an IRQ handler
that fires mid-credit observes the write-set already complete (memory state
"from the future" within the call window). Handlers reading an in-flight
decompression buffer mid-call would misbehave — no known case; capture can
optionally record IRQ arrivals to flag suspicious shards.

## Mechanism

1. **Gen-time entry hook** (same emission point as round-1 loader_pump):
   functions listed in `game.toml [data_shards] funcs = [0x...]` get
   `if (psx_datashard_enter(cpu, 0xADDR)) return;` emitted at entry.
   Config-listed for the spike; auto-detection (hot, pure, hook-worthy) is
   a later phase. Rolls the cg-hash → regen + reshard (worktree owns its
   whole chain).
2. **Runtime store** (`runtime/src/cdrom/data_shards.c`): in-memory index +
   on-disk persistence under the unified cache dir
   (`cache/<game-id>/datashards/`), one file per shard + a manifest.
   Loaded lazily; saved on capture. Guarded by the same codegen-tag stamp
   discipline as code shards (a shard records the emitter tag it was
   captured under only for bookkeeping — shards are codegen-independent by
   construction, they describe *guest* semantics).
3. **Capture executor**: the dirty-RAM interpreter run-to-return with a
   tracing callback. Trace only non-exception context. Abort (poison) on
   purity violations.
4. **Observability**: TCP `data_shards` — hits, misses, captures, poisons,
   bytes replayed, cycles credited, per-func table. Always-on counters;
   ring of recent events.

## Exclusions and spike environment (2026-07-10, user directives)

- **FMVs are excluded from data-sharding.** MDEC playback is a timed
  stream (XA audio interlock, per-frame DMA), not a pure transform —
  memoizing it is semantically wrong. User-reported FMV lag turned out to
  be the **OpenGL renderer** (~0.70x pace during FMV, audio warble;
  software renderer plays the same FMV at ~0.91x+). GL FMV perf is a
  separate investigation — this spike runs on the **software renderer**.
- Software renderer at Tomba's shipped 2x SSAA is rasterizer-bound
  in-game (measured 0.51x, 60.8% of wall in GP0) — the spike's
  settings.toml pins `renderer=software, supersampling=1`.

## Feasibility gates (measure first)

Gate 1 — **profile the pig window** (phase_profile + phot + dirty_ram_stats
across a real area transition on a healthy overlay cache): what fraction of
wall time is decompressor execution? If the window is CD-cadence/idle
dominated, data shards buy little → report honestly, park.

Gate 2 — **purity**: does the Tomba decompressor trace clean (no MMIO, no
syscalls, bounded frame)? If it reads CD/DMA registers directly, the
memoization boundary must move (e.g. per-asset unpack leaf, not the
streaming wrapper).

Gate 3 — **UX delta**: wall-clock NOW-LOADING duration, cold (capture run)
vs warm (replay run), same transition. The number the user feels.

## Open questions

- Hook auto-selection (profile-guided) vs config list — config list for the
  spike.
- Cross-session invalidation: shards keyed by full read-set bytes are
  self-validating; disc/game version changes wash out naturally.
- Shard size policy: decompressed area assets are O(100KB–1MB) each;
  eviction/compaction if the store grows unreasonably (compress shard
  payloads with zstd/deflate on disk — ironic, but host-side decompress is
  free relative to interp).
