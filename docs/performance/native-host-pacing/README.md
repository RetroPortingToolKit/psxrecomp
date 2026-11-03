# Shared host deadline pacing

The guest pacer previously used whole-millisecond sleeps followed by up to
almost 2 ms of spinning. Native host pacing shares a bounded microsecond timer
wait with interpolation pacing, leaving a 200 us margin and spinning only for
the final 300 us. It samples the actual monotonic clock after each wait, including
interruptions or overshoot. A corrupt distant deadline cannot request an
unbounded wait. The existing portable host timer supplies platform support.

`PSX_NATIVE_HOST_PACING` defaults ON for ENHANCED. OFF or REFERENCE keeps the
original loops. This choice owns host waiting, with the same guest deadlines,
12-period catch-up/reanchor policy, reset behavior, input order and required
progress. It changes no guest timing model or simulation identity. There is no
runtime switch. The native helper is excluded from the reference build.

## Built measurements

Production debug tools and IPO are OFF. Each row compares identical generated
title inputs, features, settings and binaries differing only in the host pacing
selection. All include the qualified shared-image package from PR #784 and the
same old rbengine; these are incremental H1 results. Rewind is OFF on both sides.
Each ABBA block stays on one capacity-reserved Ryzen 7 9800X3D/RTX 3080 Ti host.
Concurrent owner work is allowed; raw load and variation are retained.

| Matched workload | Original CPU ms/vblank | Native CPU ms/vblank | Interpretation |
| --- | ---: | ---: | --- |
| Ape gameplay, PGXP/9x, interpolation OFF, first block | 6.797 | 5.532 | 18.61% less CPU |
| Ape same configuration, confirmation | 6.423 | 5.286 | 17.70% less CPU |
| Tomba cold movie, PGXP OFF/5x | 5.150 | 3.882 | 24.63% less CPU, one block |
| Ape gameplay, PGXP/9x, interpolation 120 | 6.077 | 5.906 | CPU ranges overlap; neutral/inconclusive |

Ape's default-60 CPU and process-cycle ranges are below both controls in both
blocks; cycles fall 17.30%/19.12%. Tomba's native CPU/cycle ranges are below both
controls, with 24.65% fewer process cycles in one block. Its interval retains
147–148 color decode commands and 360 draws. Interpolation already used fine
waits: its CPU mean changes 2.82% with overlapping ranges and unequal host load;
the package is neutral/inconclusive there, without a claimed extra gain. Active
interpolation captures and roughly two generated swaps per guest frame remain.
Comparisons between interpolation OFF and ON are not credited as optimization.

All measured intervals have zero audio bridge underruns/overflow. Both games
remain paced near 60 guest vblanks/s; this is CPU headroom, without a claimed FPS
increase. Startup and instrumented lifecycle observations remain in raw records.

## Timing tradeoff and observation scope

The timer permits bounded host submission jitter. Ape confirmation's recorded
nonempty swap mean stays near 33.367 ms, with every recorded slot gap equal to
two. Native p99 is 33.750–33.815 ms versus 33.379–33.381 ms for controls; the
native maximum is 34.000 ms. The first block's 45.160 ms **pre-pacer guest-arrival**
outlier did not recur in confirmation. That observation is retained, not removed
as contamination. Tomba adjacent-swap p99 rises from about 16.70 ms to
17.00–17.04 ms. Its shared sparse movie/repeat-elision gaps include 10 guest
slots and roughly 167 ms on both choices; those are not newly introduced stalls.
Mean cadence is sustained with the observed sub-millisecond tail tradeoff.

The unchanged timestamp rings are read after CPU timing endpoints. Guest arrival
stamps precede the pacer. Swap stamps measure CPU submission/return, with repeated
images elided and only the last swap retained per guest slot in temporal mode.
They do not establish physical display timing, every 120 Hz present, or
input-to-photon latency. Both worker settings are false in the earlier Ape
blocks with no environment override. Later temporal/Tomba blocks explicitly
disable workers and sample their actual getter states as zero throughout;
there is no retrospective getter-state claim for the earlier blocks.

[Ape data](ape.json) and [Tomba data](tomba.json) retain the per-game package,
source/binary/execution identities, options, inputs, settings, raw CPU/cycles,
frame bounds, bridge counters, memory, host CPU/RAM/GPU telemetry, reservations
and timestamp samples. Small mixed changes within variation are inconclusive.
These two games do not establish a universal title/backend/host win.

## Validation and remaining handoff

Actual original/native deadline and frame-pacer tests cover expired/zero waits,
large counters, bounded distant deadlines, interrupted and overshot waits,
transient debt, sustained reanchor, reset and PAL cadence. The original pacer
regression test and five real CMake selection configurations pass. No new OS
dependency or assembly path is introduced; the portable timer remains shared.
[Lifecycle records](correctness.json) contain the separately instrumented
current-build gameplay/save/load/pause/resume checks. They are not performance
samples or proof of physical speaker quality.

The native QA build saves and reloads its own slot, jumps, enters the actual F8
rewind pause with a full history, holds the same guest frame counter, closes the
panel and advances another 120 vblanks in about 2.05–2.08 seconds. Both default-60
and temporal-120 startup selections are recorded. Rewind is enabled only for
these separate lifecycle checks. Instrumented save/load/pause produces bridge
underruns, retained as a caveat; ordinary production timing remains clean. The
initial stale debug-pause command, unavailable single-disc runtime menu and F7
menu's existing lack of TCP servicing are retained as harness/setup limitations.
The passing check uses the already-supported F8 loop; it changes no menu code.

Reproduce through the supported title CMake entry with the recorded inputs and
`PSX_NATIVE_HOST_PACING=OFF` versus `ON`, keeping all other settings fixed.
Warm Ape 120 vblanks after loading the same private logical fixture, then measure
900; cold-boot Tomba and measure frames 800..1520, retaining actual endpoint
overshoot. Use fresh disposable profiles, a matched physical host/configuration,
and ABBA order. Run correctness instrumentation separately from production timing.

Owner adoption and combined integration checks remain at the end of the
campaign. This evidence authorizes no merge or release.
