# Completed presentation images

`PSX_SHARED_PRESENT_IMAGES=ON` selects shared completed-image ownership in
ENHANCED builds. REFERENCE/oracle targets and explicit OFF retain the original
native hold capture. This is a presentation-only choice; it changes no simulated
state or common netplay profile and requires no new host capability.

The temporal renderer already captures a stable native display band. Its freshly
completed image can also serve hold-last, avoiding a second capture of the same
band. Hold stores a read descriptor containing the texture and its own dimensions.
Its private drawable capture texture remains separate, so composing a native
hold with gamma, filtering, aspect and letterboxing cannot overwrite history.
Ordinary composed/pre-OSD drawable captures retain their existing path.

`interp_capture` is the sole history writer. Its two callers immediately publish
the completed hold descriptor on the same GL context/thread. A subsequent
rotation/reallocation is followed by the same publication before any hold
consumer; metadata-only reset retains the old completed image until then.
Context teardown invalidates the descriptor. No extra fence, worker or live
implementation switch is introduced.

Reused FLIP history is not proof that the current source image is identical.
Those intervals retain the original native hold copy. Fresh captures alone are
shared. The existing letterbox-size capture optimization is unchanged.

The real hidden OpenGL fixture compares shared and original copied pre-OSD
compositions exactly across resize, full/native versus downscaled captures,
filtering and forced 4:3. It checks reset lifetime, image dimensions, private
drawable writes, unchanged history, duplicate-FLIP fallback and context teardown.
Real CMake compile/run checks cover fresh default, explicit ON/OFF and reference
target selection. Missing real GL is a skip, not a pass.

Measurements at code commit `fac3396322aa41cbf6eff30edee5929e05af521e`
are retained in [the per-game dataset](shared-present-images-20261029.json),
under `beads-wuhs`, campaign `beads-2wy8.1`, burndown G4. Matched builds use
the same guest state, config and existing temporal120 feature on each side;
only the build-time image implementation differs. The Windows/NVIDIA runs
use hidden full OpenGL, dummy audio, uncapped CPU execution and capacity
reservations. These are Release builds with QA debug interfaces enabled.

| Game and feature configuration | Original capture scope | Shared capture scope | Result |
| --- | ---: | ---: | --- |
| Ape Escape, PGXP ON9x, ABBA block1 | 0.206970 ms | 0.076650 ms | 62.97% lower |
| Ape Escape, PGXP ON9x, ABBA block2 | 0.208278 ms | 0.077882 ms | 62.61% lower |
| Tomba, PGXP OFF5x, ABBA block1 | 0.155538 ms | 0.142207 ms | Inconclusive: overlapping ranges and different GPU clocks |

The Ape result is about **0.13 ms less GPU capture elapsed time per guest
frame**, with fresh captures approximately once per frame and no skipped or
unavailable timestamp samples. Block2 records 3456x2160 captures: sharing
removes a logical 29,859,840-byte RGBA8 copy per fresh capture, not a measured
physical VRAM-bus byte count. The timestamp scope ends before pacing; this
does not establish an FPS increase or GPU busy-time reduction of the same size.

Instrumented blocks raised CPU concerns: Ape process-cycle means increased
about1.1%, and Tomba about4.2%. Those observations remain in the dataset.
One additional matched ABBA per game disabled timestamp queries, using the
same binaries and feature settings:

| Game | Original CPU ms/guestframe | Shared CPU ms/guestframe | Qualification |
| --- | ---: | ---: | --- |
| Ape Escape | 9.790616 | 9.758992 | Overlapping ranges; neutral/inconclusive |
| Tomba | 6.151474 | 6.105400 | Overlapping ranges; neutral/inconclusive |

The adverse process-cycle direction did not persist; cadence comparisons are
mixed or overlapping, and fresh/draw/guest-cycle progress remains matched.
There is **no proven CPU gain**. Variable host exposure is retained and does
not prove that query instrumentation caused the earlier adverse observations.
The planned second instrumented Tomba block was replaced with this focused
no-query check to resolve its concrete CPU concern.

Both implementation choices have bounded current-build game save/load and
scripted-input checks in addition to the real GL lifetime fixture. Dummy audio
does not establish speaker quality. Ordinary composed, movie and stereo
captures retain their existing paths; no saving is claimed for them. Owner
adoption and combined current-base regression checks remain bundled at the
end of the campaign. These results do not authorize merging.

Diagnostic builds can opt into `PSX_GL_CAPTURE_PERF=1`. Four buffered timestamp
pairs bracket capture plus hold publication, ending before interval pacing.
Results are harvested only when available; busy slots are retained and new
scopes skipped. Missing entrypoints provide no passing result. The private
diagnostic counters retain completed samples, total GPU nanoseconds and skipped
or unavailable scopes for external A/B collection. This scope avoids treating
the paced whole-present timer as GPU busy work. Release builds omit it.
