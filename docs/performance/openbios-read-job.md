# Native OpenBIOS read request

Enabled by default in ENHANCED builds. Configure
`-DPSX_EXECUTION_PROFILE=REFERENCE` (or `-DPSX_NATIVE_OPENBIOS_READ=OFF`)
to retain the original generated request. Oracle builds also retain the
generated firmware. Existing CMake caches explicitly set to OFF remain OFF;
set the option to ON to adopt this default in those build directories.
The audited OpenBIOS image is
`fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57`.
Other BIOS images are unchanged; an unaudited OpenBIOS body fails configuration.

The common blocking CD request takes destination, LBA and sector count in
a0/a1/a2, returns destination or -1 in v0, and preserves the callee-saved ABI.
It owns MSF conversion, seek/read submission, retry budgets, completion/error
checks and return. A0 seek/read/error services and B0 TestEvent still execute
through the guest vectors. IRQ handlers, event consumption, callbacks and
actual CD/DMA transfers retain their working implementations. The unverified
DeliverEvent HLE anchor is neither invented nor bypassed.

After a complete unsuccessful event check, the native request advances to the
next observable device event, bounded to 16K cycles for host maintenance.
Intermediate device/audio events still run. This changes polling/timing, not
the CD-speed or host-loading setting. A modified B0 vector, handler or TestEvent
table entry disables deadline skipping and keeps calling the guest handler.
No runtime implementation selector or title loader is introduced.

Continuation PCs, retry state and the original 64-byte guest frame are the
entire request state. No private serializer section, live HLE/LLE conversion or
cross-build state promise is required. Existing generated continuation wrappers
enter the native body; only the original definition is renamed, so direct
callers do not accidentally enter LLE. Both source and binding recipe identify
the selected execution family for snapshots and common netplay profiles.
PGXP callee-save provenance is published/restored through the existing hooks.

The differential fixture compiles the actual generated original request beside
the native implementation. It compares caller results, MSF, command arguments,
payload, callee-saved values and error/completion outcomes for success, seek/read
submission exhaustion, seek failure, read retry, EOF, retry exhaustion and
custom table/vector/handler changes, including a patch during the request.
It also destroys/reconstitutes CPU state between
every native dispatch. This fixture alone does not validate real IRQ delivery,
current-build serialization, gameplay, PGXP rendering or performance.

```
python runtime/tests/test_openbios_read_job.py --generated generated/OpenBIOS_full.c --output <scratch> --cc <C compiler>
```

Supply `--cxx <C++ compiler>` to compare live callee-save precision using the
real PGXP engine. CTest registers the contract test when generated OpenBIOS is
available, and a separate synthetic-source selection test when Ninja is present.

Qualification belongs to `beads-wuhs` and the optimization campaign
`beads-2wy8.1`. The contract checks and matched built-game results below support
the ENHANCED default. Combined regression and owner adoption checks remain
bundled at the end of the burndown before merge.

## Bounded measurements, 2026-10-28

[Raw samples and identities](openbios-read-results-20261028.json) retain two
ABBA blocks per game on one capacity-reserved Windows PC (Ryzen 7 9800X3D,
RTX 3080 Ti, driver 616.86, GCC 15.2.0, Release/O3, runtime IPO off). Timing
runs are uncapped. Each game keeps its own rendering/features, hidden full
OpenGL presenter, dummy output with audio emulation, normal CD timing multiplier
and identical cold input setup. These are startup/loading checkpoints, not
gameplay FPS measurements.

The raw record describes the default-OFF recipe used during measurement.
The later default-ON selection change leaves the native request unchanged;
fresh-cache default, explicit OFF, REFERENCE and non-OpenBIOS selection are
checked separately. Historical source and binary identities remain unchanged.

| Game and enabled features | Checkpoint | Mean CPU-time reduction, blocks 1 / 2 | Assessment |
| --- | --- | --- | --- |
| Tomba!, PGXP off, 1x internal scale | 360 vblanks, 124 draws, 284 CD transfers | 9.57% / 17.37% | Bounded startup benefit; block 1 sample ranges overlap, block 2 separates. All four paired host-cycle reductions are positive. |
| Ape Escape, PGXP on, 9x internal scale | 1560 vblanks, 5683 draws, 1253 CD transfers | -1.15% / 1.56% | Neutral/inconclusive within variation; no proven gain or reproducible regression. |

All 16 timed intervals match progress and features within each game, with zero
dummy-output underruns. Each native run completes 13 requests without errors.
Do not aggregate these different game workloads or claim that the original
diagnostic profile predicted these savings. Retained pre-span title inputs
exclude the separate native CPU-span/value and RAM-publication experiments;
native-overlay execution is not newly qualified here.

Current-build restore/resimulation: Tomba passes three windows per build with
zero warm/cold mismatches; both builds show the same auxiliary restore drift
`[1,1,0]/8`. Ape passes four windows per build with zero warm/cold mismatches or
drift. Both Ape builds then exit 1 in window 5's ambient prime with the identical
unknown dispatch `addr=ra=0x4C544954`, `a0=0x8013AE90`, `a1=0x1F801110`.
Liveness-aware reader retries and retained crash reports confirm a shared
baseline failure, tracked under `beads-eio.3.66`; its root cause remains open.
Correctness diagnostics were separately capped below their CPU reservation and
are not performance samples. No failure was masked or patched for this change.

Final combined regression/adoption, common netplay-profile qualification and
normal-audio owner checks remain for the end-of-burndown handoff. This request
does not close the remaining general firmware, card or transfer candidates.

## Remaining event-service work, 2026-10-29

The existing native blocking-read qualification above remains unchanged: Tomba
shows a bounded startup CPU benefit; Ape is neutral/inconclusive. Its default
selection, original 16 raw intervals and shared lifecycle findings are retained.

A [current-package selector](openbios-event-selector-20261029.json) sampled one
Ape cold start with native-read activity observed during sampling
(13 requests completed without errors). One of 5,103 host leaf samples landed
in DeliverEvent's generated body, about 0.0395% of sampled thread-cycle weight;
the other seven primary bodies and continuation thunks had no leaf hits.
The uncertain ancestor union was about 0.527% and is not additive CPU time.
The complete fresh-process B0 census proves activity, including TestEvent and
UnDeliverEvent; it does not count direct firmware calls or establish their cost.

The original full selector failed after capture because this production
DEBUG_OFF binary exposed no debug endpoint for the guest-cycle-profile export.
Callback context and the final full GPU snapshot are unavailable, not zero.
Independent review accepted only the retained host/census/read-activity/progress
and clean-close subset. Most host leaf samples were DLL/helper work, so these
figures are neither an exact family cost ceiling nor proof of zero cost.

This observed window does not fund a large eight-service kernel prototype.
It does not establish that general event/wait/callback work is complete, and it
does not replace the combined regression/common-profile/owner handoff above.
