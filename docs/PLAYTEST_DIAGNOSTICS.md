# Shared playtest diagnostics

The PSX recorder follows the bounded background writer used by SNES Mega Man X
and the current MediEvil II recorder. It records the controller state actually
latched in SIO at each guest VBlank: both button words, analog capability, four
stick bytes per pad and independent controller presence. A rotating digest reads
16 KiB of main RAM each VBlank, covering every word across 128 frames. It is a
sampled divergence check, not a full machine-state checksum.

Each accepted row is copied to a bounded queue. Formatting and disk writes run
on a worker that flushes every 100 ms, including while gameplay is paused.
Normal shutdown drains accepted rows. Queue overflow or a write error stops
capture explicitly; it never silently drops an input row or blocks gameplay
waiting for queue space. Session creation, starting-card/settings copies and
the once-per-process executable hash happen outside steady-state recording.

## Add a title

Call after creating the runtime target:

```cmake
include("${PSXRECOMP_ROOT}/runtime/playtest_diagnostics.cmake")
psxrecomp_add_playtest_diagnostics(psx-runtime
    GAME_ID "SLUS-00000" MOD_ID "game.diagnostics")
```

Create a title mod package with a recording feature whose plugin is
`game.diagnostics`, and a separate input-display feature whose plugin is
`game.diagnostics.osd`. The package controls their defaults. The helper also
covers an existing `psx-runtime-pgxp` sibling and stages `replay-diagnostics.py`
beside both executables. The bundled-release packager includes this script when
present, as does the setup-host kit packager; AppImage wrapping preserves it in
the payload. Input Display also works with Recording disabled, with no RAM
digest or file capture. Session changes and netplay suspension clear the display.
No texture feature,
texture package or other enhancement is enabled or exposed by this helper.

The implementation reads no title-specific addresses. `telemetry.csv` stores
host intervals, incident markers, CD activity, ordinary GP0 draw commands,
host presentation calls, effective video configuration and optional render-pass
state-verification counters. These are passive evidence and never feed replay
or simulation. Press L3 + R3 together
to mark an incident and queue a composed screenshot on supported presenters;
holding the chord produces one marker/request. Game-specific
gameplay samplers remain separate, validated adapters.

## Feedback bundle

Recording starts at the first guest VBlank and creates
`saves/diagnostics/session-<timestamp>` beneath the runtime data root. On normal
desktop installs this is beside the executable; AppImage launchers use their
writable data root. A blocked destination falls back to the OS temporary
directory under `psxrecomp-diagnostics/<game-id>/session-<timestamp>`.
`diagnostics-location.txt`, when writable beside the game, points to the actual
folder. The runtime log also reports its full path.

Quit normally after reproducing a problem and zip the complete session folder:

- `inputs.csv` and `session.json` describe inputs and execution/build identity.
- `telemetry.csv` contains timing, incident, CD and renderer evidence.
- `incident-<number>.png`, when the presenter completed an incident capture,
  shows composed output rather than only native VRAM. OpenGL and software
  support this path; headless and Vulkan currently reject it explicitly.
- `mods-state.toml`, optional `settings.toml`, and starting cards or explicit
  absence markers preserve the original choices and save data.
- A slot-state restore starts a new segment with its complete `baseline.pst`.
  Rewind segments without a reusable slot image are marked unreplayable.

Discs and BIOS images are never copied into feedback bundles. Starting cards
contain the player's saves. Recording and input display are independently
controlled by the title's mod features. Netplay frames are skipped; this
offline recorder does not claim to reproduce network sessions.

## Coverage and effective video state

`psxrecomp-telemetry-v2` preserves the original thirteen CSV columns and appends
the following evidence. Input-v2 and strict replay checks are unchanged.

- `gpu_draw_commands`: cumulative ordinary GP0 drawing commands observed by the
  GPU front end. This is command activity, not a count of correct rendered pixels.
- `host_presents` / `host_present_errors`: cumulative returned/failed calls
  from the actual host present API. OpenGL's direct and threaded paths, Vulkan,
  and software presentation publish these without requiring debug tools.
  `host_present_status_available` says whether the latest host present API
  supplied a result. SDL2 has no return status, so its activity counter only
  proves the call returned; zero errors under SDL2 is missing error coverage. Vulkan
  success includes `VK_SUBOPTIMAL_KHR`. Neither API proves monitor scanout.
  Counts include repeated images, pause/overlay presents and generated frames;
  do not label them unique game frames. GP0 commands and presents need not be 1:1.
- `video_renderer`: the active backend, after fallback (`0` none/headless,
  `1` software, `2` OpenGL, `3` Vulkan). `video_resolution_preset`,
  `video_aspect_num/den` and `video_widescreen` sample the emulation owner's
  settings after mod overrides, including changes during the session.
- `video_snapshot_valid`: whether a coherent passive backend snapshot was
  available. `composed_*` fields describe the last composed image's backend,
  actual internal scale, native source dimensions, drawable dimensions and
  fitted viewport dimensions. Dimensions unavailable on a path are zero.
  A threaded renderer can compose ahead of the swap counter; these fields do
  not identify the exact image a particular swap displayed. FMV/menus may use
  a narrower viewport than the configured widescreen mode.
- `render_verify_enabled`, existing `render_verify_checks/mismatch`, and
  `render_verify_ever_executed`: enablement, cumulative checks/failures and
  whether any optional extra render-pass state-preservation check has run.
  These do not monitor ordinary rendering or texture correctness. An enabled
  check that never executes supplies no coverage. To measure a recording
  interval, compare the counter increase, not just a preexisting nonzero value.
- `visual_verify_available` / `visual_verify_checks`: currently zero because
  this recorder has no independent pixel oracle. This is explicit missing
  coverage, never a successful texture test. Screenshots/clips and matched
  original-game comparisons remain necessary for visible defects.
- `incident_capture_request`: `-1` means no request on this row, `0` rejected,
  and `1` queued on the existing composed-output capture path. The next
  presented image is written as `incident-<number>.png` if capture succeeds;
  an accepted request alone is not a completed screenshot. The analyzer lists
  accepted/rejected requests separately from image files present in the bundle.
  Capturing on an explicit incident may cost a readback/PNG write; ordinary
  frames do not request pixel capture. A pending diagnostic request can be
  replaced by a later explicit screenshot request before it is fulfilled.

The backend observer does not read pixels, query GL errors, synchronize the
render thread or change guest state. Its snapshot makes at most three attempts
and reports unavailable if a coherent publication cannot be sampled.

Analyze either v1 or v2 without launching a game:

```text
python replay-diagnostics.py <session-dir> --analyze
```

The JSON report separates activity, optional state checks and visual coverage.
Old v1 bundles explicitly report absent ordinary-renderer/effective-mode data.
Missing probes, zero executed checks, sequence discontinuities and counter
resets are visible; zero mismatches alone is never presented as a pass. A torn
final row is ignored, while a malformed complete row is rejected.

A diagnostic recording is helpful for exact input/state reproduction, but is
not a prerequisite for investigating a visual report. Reproduce known menu,
attract-demo, transition and resize cases directly with a fresh private save
directory and muted automated output. Capture the composed presentation, since
raw/native VRAM screenshots can omit the very widescreen/HD transformation at
issue. Preserve location, camera direction, effective mode and before/after
images. Escalate to an incident-marked recording when the trigger is unclear
or depends on intermittent timing; do not classify an untried case as
improbable merely because the reporter supplied no log.

## Replay

Use Python 3.8+ (install `tomli` before Python 3.11) with the recording's original
native executable, disc and game configuration. The tool copies the executable, runtime dependencies and
starting state into a newly created private directory. Personal settings/cards
and the original installation are untouched.

```text
python replay-diagnostics.py <session-dir> --exe <original-executable> \
    --disc <disc.cue> --game <game.toml> --output <new-private-directory>
```

The source copy is `psxrecomp/tools/replay_diagnostics.py`. For an AppImage,
extract the original image, pass the native `usr/bin/<game>` executable and
the payload's `game.toml`. Direct AppImage execution is rejected because its
launcher can seed personal data. Automated playback uses dummy SDL audio
output while retaining emulation; ordinary interactive play keeps normal audio.

Strict playback requires the same input format, execution identity, portable
mod-plan identity and build identity. The build identity includes the game's
serial/version, game/framework revisions and SHA-256 of the running executable.
The binary hash also distinguishes source archives, dirty builds, compiler
changes and signed packages. A baseline segment must restore exactly the saved
image before inputs are consumed. Playback checks every accepted pad field and
sampled RAM digest, stops at the first observed divergence, and reports success
only after all complete rows have been checked. A torn final row is ignored.

Developer builds additionally accept `--input-only` to explore a route across
changes. This checks accepted inputs but skips build/mod/RAM equivalence and
reports route completion separately. Player builds reject it. Legacy v1 routes
require an explicit `--legacy-connected` assumption because they lacked pad
presence. These routes are never counted as verified replay.

## Standalone validation

No game, BIOS, SDL window or speakers are needed:

```text
cmake -S runtime/tests/playtest_diagnostics -B build-diagnostics
cmake --build build-diagnostics --parallel 2
ctest --test-dir build-diagnostics --output-on-failure
```

The fixtures cover queue saturation/owned rows/order/wall-time flushing,
record/replay alignment, digital and analog controller removal, incident
debouncing, first divergence, identity mismatches, save-state baselines, readonly
output fallback, release-build input-only rejection and isolated replay setup.
They establish the shared engine-boundary contract. Actual title/disc playtests
are still needed to qualify each game's original-build replay.
The video tests additionally cover concurrent passive snapshots, composition
versus completed presentation, failed presents, live scale/aspect changes,
ordinary activity with no optional checks, legacy coverage reporting and the
analysis-only CLI.
