# Frame rate: presentation above the guest's cadence

A game-owned mod can present a PS1 title at a higher rate than it renders
(60, 120, 240 Hz, the display's refresh, ...) without changing how fast the
game runs. Guest VBlanks, timers, CD, SPU and game logic stay at their stock
cadence; only what the OpenGL presenter shows between guest VBlanks changes.
Never use `psx_mod_set_native_vblank_rate` for this: it speeds up the whole
machine.

## Enabling it (mod_plugins.h)

From a trusted plugin's activation callback:

```c
psx_mod_set_frame_interpolation_source(PSX_MOD_FRAME_SOURCE_FLIP);   /* optional */
psx_mod_set_frame_interpolation_blend(PSX_MOD_FRAME_INTERPOLATION_LINEAR);
psx_mod_set_frame_interpolation(120);   /* 0 = follow the display refresh */
```

`psx_mod_set_frame_interpolation` accepts 0 or 60..1000, selects the OpenGL
renderer and turns vsync off (the presenter paces itself with a sleep+spin
schedule on the emulation thread). Blend and source are reset to their
defaults at every session start (first boot and the lobby rematch), so a
netplay session or a later offline one never inherits them.

The presenter uses the high-resolution host timer for microsecond waits,
leaving only a short final spin for scheduling jitter. Whole-millisecond
waits previously spent up to almost two milliseconds spinning per output,
which could starve game execution on a CPU-constrained host at high refresh.
The presentation deadlines and guest cadence remain unchanged.

## Blend modes

| Mode | Output between two source frames |
|---|---|
| `LINEAR` (default) | crossfade `mix(prev, cur, a)` |
| `MOTION_ADAPTIVE` | crossfade small changes, switch large changes at a = 0.5 |
| `HOLD` | no crossfade: repeat the newest frame (for titles whose plugin supplies in-between images with render passes) |

Blending never invents motion: moving objects ghost, and the image is one
source frame behind the newest one. True in-between frames need the game's
own draw code: see [RENDER_PASSES.md](RENDER_PASSES.md).

## Blend source: every VBlank or real flips

The presenter plans one output schedule per guest VBlank present.

- `PSX_MOD_FRAME_SOURCE_VBLANK` (default, and the historical behaviour):
  every guest VBlank is a new source frame. Right for games that flip every
  VBlank.
- `PSX_MOD_FRAME_SOURCE_FLIP`: a VBlank is a new source frame only when the
  game really flipped -- the displayed VRAM origin moved, or the displayed
  rect was redrawn. The presenter tracks the flip period P (VBlanks between
  flips, clamped 1..4) and gives VBlank k after a flip the blend window
  [k/P, (k+1)/P], so one crossfade spans the whole game frame. A 30 Hz game
  (P = 2) would otherwise blend for one VBlank and hold for the next. A frame
  that arrives late holds at a = 1; one that arrives early restarts from the
  frame that was fully shown.

`gl_interp` (TCP) reports `source`, `flip_period`, `captures` (new frames)
and `duplicates` (VBlanks that re-presented the same frame); a 30 Hz game in
FLIP mode shows about 30 captures and 30 duplicates per second.

## Limits

- OpenGL only; a command-line `--renderer` override after activation drops
  it. Netplay sessions run without mods, so without interpolation.
- FMV (MDEC / 24-bit) frames suspend it, as does rewind.
- Vsync is off: on a fixed-refresh panel pick the display refresh rate.

Tests: `frame_interpolation_schedule_test` (runtime ctest) covers the phase
windows, the flip tracker, the new-frame decision and output counts per rate
at 30 Hz. `frame_interpolation_flip_source` (source guard, recompiler ctest)
pins the wiring no unit test reaches: the session-start hand-off of the
source to the renderer, and `interp_capture`'s FLIP branch (origin and redraw
into the decision, an early return on a duplicate).
