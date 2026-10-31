# Native MDEC transfer jobs

MDEC already decodes complete native jobs and batches contiguous input. The
remaining output transfer can wake the common cycle/device scheduler for every
word. ENHANCED builds now publish a minimum of32words per internal MDEC service
event, or the remaining tail. This is one output FIFO, at most448guestclocks
(about13.2us) of delayed partial publication. A late catch-up still drains all
overdue words;32 is not a maximum transfer size.

The existing data loop retains live input reads, attributed RAM writes and their
observers, wrapping/decrement, readiness, cancellation, final completion and IRQ.
Other devices and DMA channels keep their deadlines. No decoder, pixel format,
precision, presentation or audio algorithm is removed. This does not claim an
entire movie can execute without guest-visible intermediate progress.

`PSX_NATIVE_MEDIA_JOBS` defaults ON in ENHANCED. Configure it OFF, or select
REFERENCE, to compile the maintained original `dma.c` instead. One translation
unit is linked; there is no runtime switch. Selection and shared source/contract
hashes enter execution identity. Simulation profiles must agree before netplay;
these measurements do not qualify a two-peer or cross-host session.

## Built result

The machine-readable [Tomba samples](tomba.json) retain both matched ABBA blocks,
source/title/binary identities, compiler options, settings, actual feature and
progress counters, per-sample host CPU/RAM/GPU/process-I/O data and reservation
receipts. Chart inputs are the raw CPU time and actual vblank counts. Each block
compares arithmetic means of the two runs per choice.

| Tomba native movie interval | Original CPU ms/vblank | Native CPU ms/vblank | Reduction |
| --- | ---: | ---: | ---: |
| ABBA1 | 7.127 | 5.938 | 16.68% |
| ABBA2 | 6.121 | 5.431 | 11.26% |

The observed ranges separate within each block, and process-cycle measurements
agree directionally. Between-block host variation is substantial. The paced
interval stayed near60vblanks/second; this is reduced CPU work, not higher FPS.
Poll endpoints cover720–725vblanks,147–148decode commands and360draws per run.
The interval includes a movie transition, rather than uninterrupted decode
throughput. Both choices use PGXPOFF,5x OpenGL and dummy audio in a hidden normal
presenter. The copied settings file requests vsync0, but the live requested
selection is1 in both builds; do not infer that all copied settings applied.

## Correctness and limits

The actual production DMA/MDEC fixture runs original/native atO0/O2. It exercises
15/24bit output, tails, live input that changes actual decoded pixels, wrapping,
decrement, FIFO stalls, cancel/restart, channel disable/re-enable, late catch-up,
unrelated deadlines, per-write provenance and partially active input/output
snapshot restore. The real CMake selector test checks default/ON/OFF/REFERENCE
selection, one linked body and distinct identities.

Both exact production executables also pass five8boundary warm rollback windows,
with zero cold mismatches and continued movie progress. They exhibit the same
live-versus-restore auxiliary drift `[1,1,0,2,2]`; this remains a shared caveat.
Retained real-game snapshot blobs contain drained MDEC state. Active partial
transfer restore is established by the actual-module fixture, not those blobs.

The eight ordinary timed intervals record zero bridge underruns/overflow drops.
Substantial startup overflow remains, and instrumented rollback produces underrun
counts in both choices. None of this establishes normal speaker quality. Owner
visual/audio adoption and combined regression handoff remain deferred to the end
of the campaign. No whole-game, all-title or merge acceptance is claimed.

Reproduction uses the supported title build with identical generated inputs,
`PSX_EXECUTION_PROFILE=ENHANCED`, `PSX_RUNTIME_IPO=OFF`, debug tools OFF, changing
only `PSX_NATIVE_MEDIA_JOBS=OFF/ON`. Use an isolated disposable profile per launch,
the recorded disc revision/config, a hidden presenter and dummy audio. Keep each
ABBA block on one capacity-reserved compatible host and retain actual bounds and
host load. Correctness/selfcheck runs are separate from timing.
