# WO-6 / WO-7 and BIOS #343 / #346 integration

Tracked by `beads-eio.3.140`. Integration branch:
`integrate/wo6-bios343-346`. Contributor heads `c2f9c3d5` (#343) and
`baca0a8a` (#346) are retained as ancestors, not copied or squashed.
Initial baseline: `736c002a`; refreshed baseline: `b4ea4c37`.

## Changes and conclusions

- WO-6: generate an unconditional immutable physical-word index for resident
  dispatch tables spanning less than 2 MiB. Wide/ambiguous tables retain the
  original binary search. Keep CPS returns, live-code validation, interrupt
  checks and mod hooks. This accelerates address resolution; it does not turn
  overlay calls into nested C calls or promise fewer dispatcher activations.
  Tomba's 18,130-entry index is 265,706 bytes; MMX6's 18,858-entry index is
  255,918 bytes. An isolated benchmark of the actual emitted lookup with
  simplified table records and 4,096 repeated mixed-hit queries measured
  roughly 54-61 ns/query for binary search and 1.2-1.6 ns/query for the index.
  This is **not** a measurement of total dispatch overhead or game speedup.
- WO-7: the native hot-owner sampler counts owner activations, including CPS
  continuation returns. Function-entry tracing counts invocations. Its
  direct-mapped bucket resets on a collision, so it can undercount but does not
  accumulate another owner's calls. Clarify the telemetry label and API docs;
  no counter algorithm change or confirmed counter corruption. The supplied
  brief's 3.2 microseconds divides total guest work by activations and therefore
  does not isolate dispatch cost. Its branch at `0x80191BC0` with immediate
  `FFFB` targets `0x80191BB0`, not `0x80191BB4`.
- #343: setup detection now requires a configured dispatch/full pair and its
  matching backend descriptor, and uses the configured framework location.
  Preserve expected-retail discovery for `OpenBIOS;SCPH1001` and alternate
  retail stems. Add the CLI test file registered but missing from the PR.
- #346: guard patched BIOS ranges before instruction charges and terminators,
  including entry through interior labels. Compare the whole declared range
  and hand off at the requested RAM PC. Reject native emission when the range
  begins across a live branch/load-delay boundary. Do not hand back from the
  interpreter with a pending load. Clear range metadata on memory reset.
  No patch ranges were widened and no guest patch effects were synthesized.

## Validation

Windows native Clang 22, Release recompiler / RelWithDebInfo runtimes; fresh
game and BIOS generation, isolated builds, overlay caches and memory cards.
Real title-owned mods were built/staged; no original game output or save was
modified. Live runs use LLE BIOS boot/calls, headless framebuffer readback,
debug diagnostics and native GCC/Clang overlay compilation. UI, Vulkan,
netplay, rewind and PGXP are disabled in this regression configuration.

Recompiler CTest: 65 enabled tests pass, including the executable generated
lookup check (aliases, holes, alignment, bounds, stale-code rejection and
interrupt/call behavior). Runtime CTest: 78 executed tests pass; one explicit
skip and two disabled. Recompiler has three disabled tests; its disabled
overlay-pair test was run directly and passed all five executable scenarios.
The #343 host probe was also run with real recomp-ui headers and native CC:
seven positive/negative cases pass. Upstream relocation/AOT tests: 4 + 24 pass.

The test-harness repairs include two Windows text/byte fixtures, a stale
assertion that demanded unsafe continuation-range clipping, a cache-tag
fixture now using the canonical serializer, and a Windows stack allowance for
the mod-runtime test's real 1 MiB disc-hashing scratch buffer. None changes
production guest behavior.

Both linked BIOS images were regenerated. OpenBIOS emits 651 functions with
zero tagged interpreter fallbacks and four existing unsupported-instruction
skips; retail emits 1,309 with its one existing load-delay fallback and one
existing unsupported-instruction skip.
Retail SHA-256:
`71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3`.

Initial matrix: all eight combinations of Tomba/MMX6, OpenBIOS/SCPH-1001 and
baseline/integration completed at least 11,000 frames. Screenshots show real
FMV and MMX6's gameplay demo; integration has native overlay/kernel execution,
zero remaining kernel-body mismatches, and completed 128-byte card reads.
The first baseline Tomba screenshot exceeded a five-second TCP deadline;
its rerun passed. The first integration MMX6 run spent most of its budget in
host startup and reached only 1,414 frames; a longer-budget retry passed.
The baseline also exhibited slow host startup. Do not present either failed
attempt as a successful run or interpret cold-start times as dispatch cost.

The rebuilt eight-way matrix against `b4ea4c37` passed, with clean process
exits and 32 completed 128-byte card transactions per run:

| Game | BIOS | Baseline frames | Integration frames | Kernel mismatches, baseline -> integration |
| --- | --- | ---: | ---: | ---: |
| Tomba | OpenBIOS | 11,005 | 11,015 | 22 -> 0 |
| Tomba | SCPH-1001 | 11,009 | 11,008 | 11 -> 0 |
| MMX6 | OpenBIOS | 11,002 | 11,013 | 7 -> 0 |
| MMX6 | SCPH-1001 | 11,014 | 11,015 | 11 -> 0 |

Each integration run has native overlays plus native kernel execution and
11-33 patch-skip events. Observed warm wall times were 82.5/86.2 seconds for
Tomba/OpenBIOS, 71.6/74.2 for Tomba/retail, 70.2/69.3 for MMX6/OpenBIOS and
70.6/70.8 for MMX6/retail (integration/baseline). These runs share a busy host
and are not controlled FPS benchmarks. Retail Tomba's frame-1,500 checkpoint
falls in a display-disabled boot interval on both builds; subsequent
checkpoints show the same FMV. Local raw reports/screenshots are under
`F:/Projects/psxrecomp/_validation-wo6-bios/{baseline,integration}/{tomba,mmx6}/results/`;
`final-openbios` and `final-SCPH1001` identify the refreshed runs.

An additional OpenBIOS/MMX6 run on each side captured the demo fade-out at
roughly 20-frame intervals through frame 11,150. Both show the same fade,
brief display-disable interval, and return to the 512-wide Capcom screen.
This explains the darker initial integration screenshot sampled 11 frames
later than baseline; it is not missing background rendering.

Integration PR: [#348](https://github.com/RetroPortingToolKit/psxrecomp/pull/348).

## Scope of confidence

This is regression evidence for the tested boot/FMVs/attract paths, generated
dispatch contracts, BIOS patch boundaries and card protocol, not a full
playthrough, hardware differential proof, audible-audio assessment, UI test,
save/load round trip, cross-platform certification or Parasite Eve FPS result.
Tomba 2 and alternate retail BIOS images were not part of this matrix.
