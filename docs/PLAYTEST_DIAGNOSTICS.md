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
host intervals, incident markers, CD activity and render-pass counters. These
are passive evidence and never feed replay or simulation. Press L3 + R3 together
to mark an incident; holding the chord produces one marker. Game-specific
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
- `mods-state.toml`, optional `settings.toml`, and starting cards or explicit
  absence markers preserve the original choices and save data.
- A slot-state restore starts a new segment with its complete `baseline.pst`.
  Rewind segments without a reusable slot image are marked unreplayable.

Discs and BIOS images are never copied into feedback bundles. Starting cards
contain the player's saves. Recording and input display are independently
controlled by the title's mod features. Netplay frames are skipped; this
offline recorder does not claim to reproduce network sessions.

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
