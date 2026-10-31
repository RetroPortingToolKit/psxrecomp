# Reusable raw snapshot capture

Raw rewind snapshots previously allocated a fresh12MiB initial writer buffer,
staged module payloads, and freed/replaced each history allocation. The native
package sizes state after canonical device/SPU catch-up and writes raw sections
directly into reusable storage. rbengine serializes into one unpublished spare,
then swaps it into history only on success. An unsuccessful capture preserves
all visible history. Replaced or evicted storage becomes the next spare.

`PSX_NATIVE_CAPTURE_JOBS` defaults ON for ENHANCED. OFF or REFERENCE selects
the maintained original writer/store. Both use the same current-build caller
contract; selection and source/contract hashes enter execution identity. No
runtime switch or old-state migration is required. OFF/REFERENCE can still build
with the old rbengine API; native ring capture requires reusable APIv1. No-ring
builds import no ring dependency. Portable/big-endian or unaligned VRAM paths
retain staging where necessary. Compressed disk saves keep their existing path.

The ring retains allocation high-water marks, bounded by depth+one buffers,
until clear/destroy. This is raw storage reuse, without compression or deltas.
It does not merge captures taken at different logical times, remove GPU readback,
change capture scheduling, or eliminate current-build restore requirements.

## Built measurements

One matched production ABBA per game, on the recorded Ryzen7 9800X3D/RTX3080Ti
host. Debug tools and IPO are OFF. Active rewind is configured interval1/depth50;
Tomba keeps the existing FMVcadence4. A full sampled history and advancing capture
index establish active capture, not an independently counted successful-capture
total. Each block uses identical title/generated inputs, settings/features and
physical host. The package explicitly includes the old/new rbengine dependency.

| Workload | Original CPU ms/vblank | Native CPU ms/vblank | Mean private commit saved |
| --- | ---: | ---: | ---: |
| Ape gameplay, PGXPON/9x/temporal120 | 11.611 | 11.568 | 417.03MiB |
| Tomba movie, PGXPOFF/5x | 6.881 | 6.474 | 411.83MiB |

Ape CPU/cycle ranges overlap: neutral/inconclusive (0.37%/0.16% mean reductions).
Tomba's two native samples are below both controls (5.91% CPU,3.22% cycles),
supporting one-block evidence. The primary demonstrated result is roughly
412–417MiB lower **Windows private memory commitment** with these full histories.
Page faults/vblank fell99.35%/94.14%. Working sets overlap; no physical-RAM/RSS
or FPS gain is claimed. No-rewind or other-depth benefits are unmeasured.

[Ape raw samples](ape.json) and [Tomba raw samples](tomba.json) retain CPU time,
actual vblank bounds, process memory/faults, host CPU/RAM/GPU telemetry, source,
binary and execution identities, build options, settings, features and lease
receipts. Both paced near60Hz. Normal measured intervals have zero bridge
underruns/overflow; startup overflow remains. Copied Tomba settings request
vsync0 but live requested vsync is1, as recorded; do not assume every setting
applied. Concurrent owner work was permitted and variation is retained.

## Correctness and remaining gate

The real serializer/loader and adapter fixtures exercise current-build restore,
direct/staged VRAM, dirty mirror, 2/8MiB RAM, dynamic post-catch-up layout, odd
arenas, growth failures preserving history, overflow, overwrite/drop/clear and
warm allocation reuse. Original/native O0/O2 wire digests agree. Build-selector
fixtures cover default/ON/OFF/REFERENCE and no-ring cases. The real netplay
adapter fixture also builds/runs REFERENCE against the actual old rbengine API.

Actual native Ape QA performed jumping, its own disk save/load, F8 opening,
host-pad selection/accept of the oldest full-ring entry, earlier guest-cycle and
capture-tick restoration, resumed gameplay and history refill. Both exact Tomba
production builds passed five8boundary warm rollback selfchecks with zero cold
mismatches; identical live-restore auxiliary drift `[1,1,0,2,2]` remains a shared
caveat. This selfcheck exercises serializer rollback; the separate actual-module
fixture covers the netplay ring adapter. [Correctness observations](correctness.json)
retain the bounded scope. Instrumented pauses/rollback produce audio underruns;
these checks do not establish speaker quality or a two-peer/cross-host session.

Owner adoption, combined regression coverage and merge approval remain queued
at the end of the campaign. No merge or release is authorized by this evidence.

Reproduce with each title's supported CMake entry and the recorded generated
inputs, options and disc configuration. Use fresh disposable profiles, the same
recorded active features and paired host/settings. Compare OFF+olddep against
ON+reusabledep in ABBA order, warm Ape120vblanks then measure900; cold-boot Tomba
and measure800..1520. Keep actual endpoint overshoot and host samples. Correctness
and rollback instrumentation run separately from production timing.
