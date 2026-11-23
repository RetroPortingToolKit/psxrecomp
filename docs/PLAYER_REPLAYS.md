# Playable replays

**Mods → Playable Replays (Single Player)** saves gameplay as `.psxrpl`
files in the game's save folder, under `replays`. It is enabled by default,
remembers your choice, and applies on the next launch. Quit normally to finish
the file. Recording observes normal controller input. In-game saves still
write to the player's memory cards, and background compilation continues.

This is separate from **Input Recording**, where a title provides that control.
Input Recording collects support-session diagnostics; Playable Replays makes a
file that the game can play back.

The format records Player 1's buttons and DualShock sticks. A connected,
neutral Player 2 pad is allowed. Player 2 activity, host controller mode/layout
changes, special peripherals, or restoring a state/rewinding stop the recording
with a message. The completed prefix remains playable; a recording with no
completed frames produces no file. A load or stop between frames finishes at
the last verified checkpoint, up to one second earlier. Ordinary controls and
state loading continue to work. Netplay is unsupported.

To play a file with the same game and disc:

```text
Game.exe --replay path/to/session.psxrpl --replay-verdict path/to/result.json
```

Both anchored and power-on playback temporarily use the recorded settings and
isolated copies of the recorded memory cards. The player's settings and ordinary
cards are restored when playback ends. An unfinished card write cannot carry
over to the restored cards. Player 1 input after it has been neutral takes over.
Power-on recordings must be played from a fresh launch; anchored recordings
start from their embedded, build-specific execution state. Older states may be
rejected. This does not affect native memory-card compatibility.
Incomplete prerelease anchors without recorded card images are refused safely.

The verdict checks guest-state digests and the final RAM/cycle checkpoint.
Rendering-only differences are reported separately. Recordings remember when
the game requested background compilation and when native code became
available. Power-on playback can ask the local compiler to reproduce the
capture schedule. Units available before the first recorded capture must
already be in the player's cache. Anchored playback requires the recorded
native units in the cache because its save state omits earlier compiler history.
