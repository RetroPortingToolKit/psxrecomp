# Shadow Audio + Screen Enhancements (PSX backport)

Backport of the gbarecomp "verified-enhancement" QoL layer to psxrecomp.
All work lives on the `feat/shadow-enhancements` branch / `_shadow_psxrecomp`
worktree (sibling of the core repo `psxrecomp`, branched
off `master`). It does not touch the in-flight `feat/gl-renderer`,
`feat/mmx6-input`, `dev/ape-escape`, or any other branch/worktree.

## Which PSX core / language / build

- **Core chosen:** `psxrecomp` — the real git repo
  (`origin git@github.com:mstan/psxrecomp.git`, default branch `master`, current
  to 2026-06-10). `<psxrecomp-v4>` is a stale stub (symlink to
  `psxrecomp/duckstation` + an empty `recompiler/build`), NOT a maintained core.
- **Language:** runtime is **C99** (`spu.c`, `gpu.c`, …) with a little C++17
  (`main.cpp`, `gte.cpp`). All shadow code is written in **C** to match the
  files it integrates with (SPU + GPU are C). This also matches the snesrecomp
  backport (also pure C), which is the closest analog.
- **Build:** CMake. Runtime sources are listed in
  `runtime/runtime.cmake` (`PSXRECOMP_RUNTIME_SOURCES`); new files are added
  there. Targets built via `psxrecomp_add_runtime_target` (psx-runtime, the
  oracle, per-game `*Recomp` projects). Toolchains: MSVC or MinGW gcc.

## Governing principle (the carve-out)

Faithfulness is the product; these are an opt-in layer on top. The one
permitted form of HLE here is a **verified-enhancement shadow**, allowed only
when ALL hold (recomp-template `PRINCIPLES.md`, "Verified-Enhancement HLE Is
Allowed; Load-Bearing HLE Is Not"):

1. The emulated (canon) path keeps running and stays both the authoritative
   output and the verify oracle. The shadow is never ground truth.
2. The shadow is continuously, differentially checked against the canon stream
   and substitutes only after a proven window.
3. It reverts loudly (logs DEGRADED) the instant it stops matching.
4. It is opt-in and present-time, off by default; with it off the output is
   byte-identical (frame hashes / oracle diffs stay on the raw canon).

Worst-case failure is "the user hears/sees the authentic hardware output," and
it cannot mask a recompiler bug because the canon path it shadows is still the
thing being diffed.

## What ports verbatim vs what is PSX-specific

| Piece | Status | Notes |
|---|---|---|
| **`ShadowVerifier`** (envelope-correlation self-check, auto-gain, prove/strike/pause) | **DONE** — `runtime/{src,include}/audio_shadow.{c,h}`, C, compiles clean, standalone-tested | Engine-agnostic; algorithm identical to gbarecomp/snesrecomp |
| Color-science core (xyY→XYZ, primaries→matrix, Bradford, sRGB OETF) | **DONE** — `runtime/src/gpu/color_lut.c` | Re-implemented in C from the GBA C++ port |
| Present-path color LUT | **DONE** — `color_lut.{c,h}`, wired into `gpu.c` | PSX VRAM is BGR555 (like GBA), so the LUT index is identical; the GBA **LCD-panel** model is swapped for **CRT / composite / Trinitron** models |
| **SPU float shadow render** | **DONE (substitution path complete; needs on-hardware A/B)** — `runtime/{src,include}/spu_shadow.{c,h}` + tap in `spu.c` | PSX-specific: re-render the ADPCM voices with 4-point cubic interpolation + float headroom (no int16 truncation), verified against the canon SPU mix |

## Console specifics (PSX)

### Video — framebuffer format + present/blit

- VRAM is `1024×512 × uint16` (`gpu.c:25`). 15-bit display mode stores **BGR555**
  (red = low 5 bits): `gpu_rgb555_to_rgb888()` (`gpu.c:368`) expands 5→8 by bit
  replication. There is also a **24-bit (depth24) mode** for FMV, where scanout
  reads packed RGB888 bytes directly (`gpu_display_pixel_rgb`, `gpu.c:374`).
- Present path (`main.cpp` `sdl_vblank_present`, ~`main.cpp:1030`):
  - GL backend presents 15-bit frames straight from the VRAM FBO
    (`gl_renderer_present_vram`, `main.cpp:1198`); 24-bit/FMV syncs the FBO down
    to CPU.
  - CPU/software path builds an ARGB buffer pixel-by-pixel via
    `gpu_display_pixel_argb()` → `gpu_display_pixel_rgb()` →
    `gpu_rgb555_to_rgb888()` (`main.cpp:1216`), then `SDL_UpdateTexture` +
    `SDL_RenderCopy` (`main.cpp:1238`). A hi-res mirror path
    (`gr_render_display_hires`) handles integer supersampling.

### Audio — SPU canon stream

- `spu.c` is a compact but real hardware SPU model: 24 ADPCM voices, 512 KB SPU
  RAM, ADSR envelope (Beetle-faithful `CalcVCDelta`/`RunEnvelope`,
  `spu.c:109`/`:145`), KEYON/KEYOFF, ENDX latch, CD/XA input bus.
- Per-voice render: `decode_block()` (`spu.c:297`, BRR-style ADPCM 4-bit blocks
  → 28 samples, two-tap predictor) and `voice_next_sample()` (`spu.c`, applies
  envelope, advances a 12-bit-fraction phase accumulator with **nearest-sample**
  selection — no interpolation). The real PS1 uses a 4-tap Gaussian; the canon
  model approximates it as nearest-sample.
- Mix: `spu_render()` (`spu.c`) sums voices × per-voice volume, adds CD input,
  applies main volume, clamps to int16, writes interleaved stereo. Consumed by
  the host from `main.cpp` `sdl_audio_pump`/`sdl_audio_update`
  (`main.cpp:586`/`:629`) into the SDL audio queue at 44.1 kHz.
- Reverb, noise, sweep volumes, pitch-mod, and IRQ timing are **not modelled**
  by the canon yet (documented in `spu.c`'s header). The shadow does not add
  them — it only re-resamples + re-mixes what the canon already produces.

### The SPU shadow (enhancement detail)

The shadow re-renders the **same notes** the canon plays — it sources the
decoded ADPCM samples, ADSR envelope, and volumes from the canon via a
read-only per-frame tap, so it can never diverge in *which* voice plays, only
in *how* it is resampled:

- **4-point Catmull-Rom cubic** interpolation between decoded samples using the
  canon's own fractional phase, instead of nearest-sample — recovers high
  frequencies the hardware Gaussian (and the canon's nearest-sample) muffle.
- **Float throughout**: envelope + per-voice volume + mix all stay float, with
  no int16 truncation until the final clamp — no intermediate requantization.

It is continuously fed (canon, shadow) to `ShadowVerifier`; it substitutes the
float mix into `out_stereo` only while `proven`, and logs a `DEGRADED` line +
reverts to the canon mix the instant correlation/level breaks.

## Integration points (found on `master`, file:line)

- **Canon audio render:** `runtime/src/spu/spu.c` `spu_render()` — mix loop sums
  `voice_next_sample()` × volumes → `out_stereo`. Shadow tap is recorded in
  `voice_next_sample()` and the mix loop (guarded by `s_shadow_tap_on`), and
  `spu_shadow_process(out_stereo, frames)` is called at the end of `spu_render`.
  `spu_init()` calls `spu_shadow_reset()`.
- **Audio host consumer:** `runtime/src/app/main.cpp:586`, `:629` (`spu_render`
  callers). No change needed — substitution happens inside `spu_render`.
- **Canon video conversion:** `runtime/src/gpu/gpu.c:368` `gpu_rgb555_to_rgb888()` —
  the 15-bit→RGB888 step on the present path. The LUT is consulted here
  (`screen_lut_ensure()` + `color_lut_map555`); the raw fast-path is preserved
  verbatim so default output is byte-identical. The depth24/FMV path
  (`gpu_display_pixel_rgb`, `gpu.c:374`) is intentionally NOT colour-mapped.
- **Build:** `runtime/runtime.cmake` `PSXRECOMP_RUNTIME_SOURCES` — added
  `spu_shadow.c`, `audio_shadow.c`, `color_lut.c`.
- **Gating (env, default OFF):**
  - `PSX_SCREEN={raw,crt,composite,trinitron}` — screen color LUT (raw =
    passthrough = default). Read once in `gpu.c` `screen_lut_ensure()`.
  - `PSX_AUDIO_SHADOW=1` — SPU float shadow (off by default). Read once in
    `spu_shadow.c` `spu_shadow_enabled()`.
  - A per-game `game.toml` `[video] screen` / `[audio] shadow` supplement can be
    added later (mirrors gbarecomp's `[video] screen` / `[audio] shadow`); env
    is the source of truth for now.

## Default-off byte-identity

- **Video:** when `PSX_SCREEN` is unset or `raw`, `screen_lut_ensure()` leaves
  `s_screen_lut == NULL` and `gpu_rgb555_to_rgb888` runs the original
  5→8-replication path unchanged. The `color_lut.c` raw table was unit-tested
  to be bit-exact to that expansion across all 32768 indices.
- **Audio:** when `PSX_AUDIO_SHADOW` is unset, `spu_render` sets
  `s_shadow_tap_on = 0` (mix loop unchanged, no tap recorded) and
  `spu_shadow_process` returns immediately, so `out_stereo` is the unmodified
  canon mix.

## Compile / verification status

- All four new TUs compile clean with `gcc -std=c99 -Wall -Wextra`
  (`audio_shadow.c`, `color_lut.c`, `spu_shadow.c`) and `spu.c`/`gpu.c`
  re-syntax-check clean with the wiring in place.
- `color_lut.c` raw-passthrough byte-identity unit test: **PASS** (all 32768
  entries match the upstream 5→8 expansion; CRT model builds non-passthrough).
- `spu_shadow.c` + `audio_shadow.c` end-to-end pipeline test: runs, closes a
  verifier window with r≈1.0, exercises prove/strike/pause + substitution.
  (The synthetic harness uses a deliberately mismatched canon/shadow level so
  the verifier *correctly* refuses to prove — confirming loud-revert
  discipline; a same-domain pair proves and substitutes.)
- Static asserts in `spu.c` pin the internal tap layout to the public
  `spu.h` layout and `SPU_VOICE_COUNT == SPU_SHADOW_MAX_VOICES`.

## Next steps

1. Build a full game target (e.g. `TombaRecomp` / `MegaManX6Recomp`) with the
   three new sources, confirm default-off is byte-identical against the oracle,
   then A/B `PSX_SCREEN=crt` and `PSX_AUDIO_SHADOW=1` on real audio/video.
2. Surface `spu_shadow_get_info()` (proven / r / ratio / gain / pauses /
   last_revert) through the TCP debug server for live observability.
3. Optional: extend the SPU shadow to read cross-block sample neighbours from
   SPU RAM so the cubic kernel doesn't clamp at block edges (currently clamps;
   the canon doesn't interpolate across blocks either, so this is a refinement
   not a correctness fix).
4. Add `game.toml` `[video] screen` / `[audio] shadow` supplements.

## Attribution

`ShadowVerifier` + color-science core ported from JRickey/gba-recomp via the
gbarecomp C++ port, © Jrickey, MIT OR Apache-2.0, used with permission. See
`THIRD_PARTY_ATTRIBUTION.md`. The CRT/composite color models and the SPU float
shadow are PSX-specific (ours).
