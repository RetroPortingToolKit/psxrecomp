# Native CPU spans: continuation evidence

This adds bounded findings to the [original measurements](native-cpu-spans-20261028.md).
It does not change the historical samples, acceptance policy, default-OFF
selection or experimental qualification status.

## Retained Tomba restore observations

All 32 original 2 MiB RAM captures were rehashed, without relinking or launching
the game again. The captured PCB/thread-array metadata and the pinned
[OpenBIOS thread ABI](https://github.com/pcsx-redux/pcsx-redux/blob/55fbf0468345acfbe5512628a3be5c0cfe3f22e7/src/mips/common/kernel/threads.h)
identify every sampled differing word as volatile register context in allocated,
inactive Thread 1. Its saved return PC, stack, return address and callee-saved
registers match. The candidate-only two-word difference is AT/A2; both are
defined before first use on the captured controller path, and full RAM converges
at the next original sample. Independent review verified the captures, ABI and
actual generated instruction sequence.

That resolves the previously unclassified candidate-extra live-in concern.
It does not explain every other volatile difference or establish full-state
equality, a harmless general cause or two-peer compatibility. The shared
strict-digest/current-build caveat is tracked separately as `beads-eio.3.424`.
No bytes were masked and no product memory writes were introduced.

## Finite Ape confirmation attempt

The unchanged original package was tested on one Windows host with a hard
0.25 physical-core-equivalent Job cap applying to the observer, game and
telemetry children. Both choices retain PGXP/9x OpenGL, batch mode 0,
interpolation OFF and an empty rewind ring. This constrained cohort is separate
from the original unrestricted measurements.

The complete control/native/native/control block observed 20.26% lower CPU per
guest frame and 19.76% lower process cycles per frame. Every run fails both
predeclared host limits: mean load was 74.42–82.27%, and the fraction of sampled
intervals over 90% was 6.67–23.26%. These results are supporting only and do not
close repeatability qualification. A reverse block completed one native run,
then stopped when the next control observer's required GPU query timed out.
The incomplete block has no A/B result. All five complete launches and the
failed control launch closed their owned games with exit code 0.

[Machine-readable per-game inputs](native-cpu-spans-continuation-20261029.json)
retain identities, per-launch metrics, exclusions and the failed-cohort scope.
Original raw samples, GPU telemetry, settings and failure/close receipts remain
with campaign `beads-wuhs`. No unrestricted FPS, physical-speaker or GPU benefit
is claimed. Native in-game card completion, common-profile qualification and
final owner adoption remain open.


## Replacement result and native card roundtrip (2026-10-30)

One complete native/control/control/native replacement block passed the same
host limits: mean host load24.58–36.51%, zero sampled intervals over90%, and
choice mean load difference6.65points. Ape CPU per guest frame was22.16% lower
and process cycles per frame23.03% lower, with non-overlapping per-choice ranges.
Actual PGXP/9x OpenGL, batch0, interpolationOFF, rewind0 and gameplay remain
fixed. Required GPU telemetry is present and all four owned games exited0.
Only the GPU query deadline changed from10 to30seconds after the interrupted
attempt; no original limit or failed result changed.

This replicates the favorable direction in a separate hard0.25-core profile.
The original comparable20.98% unrestricted result remains separately scoped;
these are not two repetitions under one identical cap. The failed high-load
ABBA and incomplete reverse attempts above remain excluded. No unrestricted
FPS or GPU-speed gain follows from this result.

The unchanged E3 native QA binary also completed the original game-menu route
Save File1 -> Completed -> Continue game No -> title -> Load Game -> File1 ->
Yes -> rendered gameplay, followed by304additional frames. A valid allocated
128KiB memory card persisted; the second blank card stayed unchanged, flush
results were0, and the exact game exited0. The current-build snapshot was used
only to stage the initial UI before Save. Its generation remained1 throughout
the later Save/Load route; no further snapshot load/save occurred.

This is a same-process native menu roundtrip, with software presentation and
dummy host audio. It does not establish fresh-boot/card-reinsert loading or
arbitrary progress restoration. Bounded protocol tails do not prove complete
fresh data I/O. All queued proof operations finished before the generic outer
inspection deadline; the raw deadline status is preserved. Prior unpaced-menu
and process-suspension harness failures are retained without product credit.

[Per-launch metrics and card observations](native-cpu-spans-qualification-20261030.json)
retain exact identities, exclusions, limits and private raw-evidence hashes.
Independent review passes the scoped checkpoint results and same-process native menu roundtrip. Common-profile qualification and normal window/audio owner adoption remain for the combined campaign gate.
