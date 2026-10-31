# Native CPU spans: measured checkpoint benefit, qualification incomplete

Generated code currently maintains instruction-level cache/interlock timing
around values already compiled to native operations. The experimental
`PSX_NATIVE_CPU_SPANS=ON` ENHANCED build replaces that bookkeeping with one
approximate charge per generated control-flow span and guarded native RAM reads.
PGXP, values/load delays, stores, device/MMIO and interrupt observations remain.
REFERENCE and OFF builds keep the original timing. The option defaults OFF.
See [the caller/build contract](../HLE_EXECUTION.md#experimental-generated-cpu-spans).

This package contains CPU spans only. The later native-value/precision-region
prototype is a separate experiment and has no accepted performance result.

## Matched production measurements

Source commit `66388558b39da51db5d158733141186e4b36fa49` for both choices;
Windows 11, Ryzen 7 9800X3D, GCC 15.2/O3, OpenGL. Paced, never-shown full
presenter with dummy audio, debug/profiler OFF. Each process starts from the
same per-game machine payload, warms 120 guest frames, then records 900 frames.
Order is control/native/native/control. Existing waits/geometry and enabled
features match within each game. Ape uses 9x/PGXP; Tomba uses 5x and its PGXP-off
variant. These are distinct per-game packages, not an equal-feature comparison
between games.

| Workload/block | Control CPU ms/frame | Native CPU ms/frame | Reduction | Disposition |
| --- | ---: | ---: | ---: | --- |
| Ape first-level checkpoint, 1 | 6.4944 | 5.0669 | 21.98% | Excluded: host-load difference 8.0178 points exceeds the predeclared 8-point limit |
| Ape first-level checkpoint, 2 | 6.7188 | 5.3094 | 20.98% | Comparable; only one accepted Ape block, repeatability open |
| Ape first-level checkpoint, 3 | 10.5087 | 8.8160 | 16.11% | Excluded: 94–98% host saturation |
| Tomba gameplay checkpoint, 1 | 4.2748 | 3.9324 | 8.01% | Comparable |
| Tomba gameplay checkpoint, 2 | 4.5656 | 3.8200 | 16.33% | Comparable |

For the comparable blocks, both native samples are below both controls.
Guest progress remains approximately 60 Hz; draws are approximately 204–205
per frame for Ape and 439 for Tomba. Some blocks observed unrelated compiler
processes; classification uses recorded actual load and is not an idle-host
claim. Both Tomba blocks support reduced CPU work, with variable magnitude.
These short checkpoint samples do not establish uncapped FPS or GPU gains.

[Machine-readable identities, policy, per-run results and variation](native-cpu-spans-20261028.json)
include all five blocks and reference [raw sampled counters](native-cpu-spans-20261028.samples.jsonl).
The policy predates collection. Its original hand-entered timestamp was corrected
to actual file creation time without changing criteria; both timestamps remain.
Full original receipts, configurations, source/generated hashes and private
fixtures are retained with campaign `beads-wuhs`; no ROM or game RAM is published.
Private seed metadata differs by build identity; machine payloads match.
Own-build save/load checks were performed independently.

## Correctness evidence and remaining gate

- C11/C++17 native/reference fixtures check RAM mirrors/expanded RAM, read
  observers, MMIO/guard fallback, device deadlines and scaled-clock carry.
  Real CMake builds check ENHANCED, REFERENCE and OFF selection, execution
  identity and rejection of missing/stale generated sources. Emitter tests
  retain original load/interlock statements and check span charges.
- Ape both timing choices pass bounded full-presenter input, actual jump and
  current-build save/load. Local rollback passes two 32-boundary windows with
  zero cold/warm mismatch and live/restore drift. Cold boots reach the rendered
  intro and execute audited overlay cache entries in both choices.
- Tomba native passes input/jump/current-build save/load. Both production
  choices repeatedly pass four 32-boundary cold and warm resim comparisons.
  Live-versus-restored RAM drift is still open: control `[32,0,32,0]`, native
  `[32,1,32,0]`. Original-binary boundary observations locate the sampled
  differences at `0x90AC–0x9130`; the candidate-only boundary differs in two words
  also present in baseline failures. That is attribution, not proof of a
  harmless/shared cause. A diagnostic relink had masked the symptom and is
  excluded from that conclusion. No digest exclusions or guest-memory patches
  were introduced.
- Warm dummy-output runs report zero underruns. Loaded cold boots disrupt
  output queues in both choices. Physical-speaker audio, native card transaction
  completion, additional Ape repeatability and appropriate two-peer/common
  simulation-profile qualification remain uncompleted.

Keep this PR draft and the feature default OFF. Do not merge or promote based
on CPU samples alone; resolve the remaining behavior/qualification findings and
obtain the applicable owner playtest clearance. No release or title-default
change is included.

[Continuation evidence](native-cpu-spans-continuation-20261029.md) narrows the
retained Tomba observation and records the finite, unqualified Ape confirmation
attempt. Historical measurements and policy above remain unchanged.
