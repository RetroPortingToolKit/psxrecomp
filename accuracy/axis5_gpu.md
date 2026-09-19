# Axis 5 — GPU Accuracy Findings & Fix Plan

Cross-reference of the psxrecomp GPU implementation against the in-tree Beetle/
Mednafen-PSX oracle (`beetle-psx/mednafen/psx/gpu*.cpp`, the byte-exact SW
renderer) and nocash psx-spx. READ-ONLY research; no source modified.

Oracle note: in Beetle the **software renderer is the hardware oracle**; the
`rsx_intf_*` GL/Vulkan paths are separate approximations and are NOT the
comparison target. At `upscale_shift==0`, no PGXP, `line_render_mode==0`,
Beetle SW = pure mednafen-PSX behavior.

Our files:
- `runtime/src/gpu/gpu.c` — GPU register/command front end (GPUSTAT, GP0/GP1
  decode, VRAM transfers, display config, widescreen machinery).
- `runtime/src/gpu/gpu_sw_renderer.c` — software rasterizer (the default/oracle-
  comparable backend; also drives 24bpp display decode for the GL path).
- `runtime/src/gpu/gpu_gl_renderer.c` — OpenGL backend (FBO RGBA8 hi-res +
  R16UI native mirror + stencil mask).

---

## 1. What our implementation does

### 1.1 Front end — `gpu.c`
- **GPUSTAT** assembled bit-for-bit (`gpu_read_gpustat`, gpu.c:1216-1290):
  bits 0-3 texpage X, 4 texpage Y, 5-6 abr, 7-8 texmode, 9 dither, 10 dfe,
  11 mask-set, 12 mask-check, 13 interlace field, 14 reverse, 15 tex-disable,
  16-22 display mode, 23 display-off, 24 IRQ1, 25 DMA-req, 26 cmd-ready,
  27 VRAM-send-ready, 28 DMA-block-ready, 29-30 DMA dir, 31 LCF.
- **GP0 command set** fully decoded (gpu.c:1585-2620): mono/shaded/textured
  tri+quad (0x20-0x3F), mono/shaded line + polyline (0x40-0x5F), variable
  rect / 1x1 dot / 8x8 / 16x16 sprites (0x60-0x7F), FBCopy/FBWrite/FBRead
  (0x80-0xDF), and env regs E1-E6. Word counts in `gp0_command_word_count`
  (gpu.c:2285).
- **GP1** reset/display-config commands; **GPUREAD** packs two 16-bit texels
  per word from the active VRAM→CPU transfer (gpu.c:1294-1329).
- Quads are split into two triangles with winding **(0,1,2)+(2,1,3)**
  (gpu.c:1610-1611, 1663-1665). Textured quads detect an axis-aligned
  rectangle and route to the faster rect rasterizer (gpu.c:1755-1778).
- Large body of **widescreen** machinery (squash / native-wide / MMX6 tile
  reveal) layered on top; identity at 4:3 by design.

### 1.2 Software rasterizer — `gpu_sw_renderer.c`
- 1024×512 `uint16_t` VRAM (15-bit + mask bit). Optional SSAA mirror `g_hr`
  and a native-wide compositor surface.
- **Semi-transparency**: all 4 modes in `blend_pixels` (sw:211-246). Textured
  prims blend only when texel bit15 set; untextured always blend when the
  command's semi flag is set (`put_opaque`/`put_textured`, sw:253-313).
- **Mask bit**: set (force bit15) + check (skip if dest bit15) in the pixel
  writers and in `sw_copy_rect` (sw:260,286,654-657). Fill ignores mask.
- **Texture window**: psx-spx AND/OR formula (sw:324-327). CLUT 4/8/15-bpp
  fetch (`texel_fetch`, sw:319-357). Texel `0x0000` = transparent (skipped).
- **Triangles**: scanline rasterizer with **float** edge + attribute
  interpolation (`raster_flat_triangle` sw:682, `raster_gouraud_triangle`
  sw:746, textured sw:864, shaded-textured sw:989). Color interp in 5-bit
  space. Optional bilinear (`texel_fetch_bilinear`, opacity-weighted, Beetle-
  style) — opt-in, non-hardware.
- **Lines**: Bresenham (sw:1382, 1403), both endpoints plotted.
- **Display**: 15-bit→ARGB with 5→8 bit replication `(c<<3)|(c>>2)`
  (`rgb555_to_argb`, sw:1503).

### 1.3 GL backend — `gpu_gl_renderer.c`
- Authoritative color = RGBA8 hi-res FBO; native R16UI mirror holds raw 1555
  for texture sampling + CPU readback; 8-bit stencil mirrors bit15.
- **Semi-transparency**: all 4 modes via fixed-function blend
  (`apply_psx_blend`, gl:697-716; mode 2 = `GL_FUNC_REVERSE_SUBTRACT`).
  Correct STP-bit gating via two-pass discard for textured prims.
- **Mask bit**: implemented via stencil + alpha channel (set + check).
- **Texture**: in-shader CLUT against VRAM usampler; texture window exact;
  texel-0 transparent honored; nearest default + opt-in bilinear.
- **CPU readback / GPUREAD**: PACK shader → R16UI → glReadPixels; 15-bit exact.
- **24bpp display**: delegated to the SW decode path.

---

## 2. Specific discrepancies vs Beetle + psx-spx

Ordered roughly by player-visible impact. Citations: `gpu_*.c:line` (ours)
vs `beetle-psx/mednafen/psx/<file>:line` (oracle).

### D1 — Dithering is completely absent (both renderers) — and the enable bit is decoded but dropped
- **Oracle**: 4×4 signed dither matrix `{{-4,0,-3,1},{2,-2,3,-1},{-3,1,-4,0},
  {3,-1,2,-2}}` (gpu.cpp:79-85), applied (then `>>3` + clamp to 5-bit) to
  Gouraud polygons, modulated-textured polygons, and lines whenever
  `DitherEnabled` (`dtd` = GP0 E1 bit 9) (gpu.cpp:644-665; gpu_polygon.cpp:
  184-211; gpu_line.cpp:145-154). Sprites & raw copies are never dithered.
- **Ours**: `dither_enabled` is parsed from GP0(E1) (gpu.c:2071) and surfaced
  in GPUSTAT bit 9 (gpu.c:1244) **but is never forwarded to either renderer.**
  `gpu_sw_renderer.c` has no dither term anywhere; Gouraud/modulated colors
  are truncated `>>3` with no matrix add. The GL backend likewise has no
  dither (confirmed; documented "the software path doesn't dither either").
- **Effect**: visible color banding on Gouraud-shaded surfaces and gradients
  (skies, lighting ramps, shadows) versus hardware; every dithered frame
  byte-diffs against the oracle. This is the single largest systematic SW/GL
  vs hardware divergence.

### D2 — SW triangle rasterizer uses float interpolation + inclusive spans, not the PSX fixed-point half-open fill rule
- **Oracle**: 64-bit fixed-point edges (`MakePolyXFP` adds `(1<<32) - (1<<11)`
  — the sub-pixel bias that yields the PSX top-left rule; gpu_polygon.cpp:
  24-43). X span is **half-open** `[x_start, x_bound)` (the `do { } while(--w>0)`
  loop, gpu_polygon.cpp:165-218), right clip inclusive of ClipX1
  (`(x+w) > clipx1+1`, :143-144). Y spans are top-inclusive/bottom-exclusive
  (`while(yi < yb)`, :460). Zero-height tri dropped (:272). Oversize reject:
  `|Δx|>=1024 || |Δy|>=512` drops the primitive (:704-728).
- **Ours**: `raster_*_triangle` sort by Y, then interpolate edges and
  attributes in **float** and fill scanlines with **inclusive** bounds
  `for (x = sx; x <= ex; x++)` (sw:719, 822, 935, 1079); Y loop is inclusive
  `for (y = y0; y <= y2; y++)` (sw:693 etc.). No top-left bias, no half-open
  right/bottom edge, no oversize rejection (only a `area2==0` degenerate
  guard, sw:953).
- **Effect**: shared triangle edges are filled by **both** triangles → seam
  double-draw (visible with semi-transparency: doubled-darkened/brightened
  edges) and ±1px coverage drift versus hardware on every polygon edge. The
  inclusive right/bottom edge is the classic "one pixel too wide" PSX
  rasterizer error. Float interpolation also rounds differently than the
  fixed-point DDA, shifting interior texel/color sampling.

### D3 — Gouraud / textured attribute interpolation is per-scanline float, not the PSX divided-difference fixed-point DDA
- **Oracle**: attributes carried as fixed-point with a `+0.5` seed bias at the
  core vertex (`COORD_MF_INT(v.r) + (1<<(COORD_FBS-1))`, gpu_polygon.cpp:
  311-313) and stepped by integer `AddIDeltas_DX`; read out `>>24`
  (:167-169). Texture U/V identical scheme (:294-295).
- **Ours**: `float` barycentric-ish interpolation along edges then along the
  span (sw:795-833 gouraud, sw:937-942 textured). UV truncated `(int)fu & 0xFF`
  (sw:942). No `+0.5` seed, no fixed-point step.
- **Effect**: color/texel values drift by up to a few LSB across a polygon vs
  hardware; combined with D1 (no dither) gradients look both banded and
  slightly mis-sloped. Texture sampling can pick a neighboring texel at span
  edges. Lower impact than D1/D2 but contributes to per-pixel diffs.

### D4 — SW Gouraud blend math is at 5-bit (correct direction) but mode-0 average lacks the oracle's low-bit truncation semantics; GL blends at 8-bit
- **Oracle mode 0** (B/2+F/2): `((F + B) - ((F^B)&0x0421)) >> 1` — strips the
  per-channel low bit before halving = floor, no rounding (gpu_common.h:28-31).
  Modes 1/2/3 use branch-free saturating 15-bit math (gpu_common.h:34-69),
  mode 3 = `(F>>2)&0x1CE7` for F/4.
- **Ours (SW)**: `blend_pixels` does straightforward per-channel `(b+f)>>1`,
  `min(31)`, `max(0)`, `b+(f>>2)` (sw:211-246). For mode 0 `(b+f)>>1` equals
  the oracle's floor only when the discarded low bits don't change the channel
  sum carry — generally equal at 5-bit per channel, **but** our halving is on
  the summed 5-bit channels directly (matches), so SW mode-0 is effectively
  the same value. Modes 1/2/3 match. Net: SW blend is close-to-exact.
- **Ours (GL)**: blends in RGBA8 with `CONSTANT_ALPHA` 0.5/0.25 then
  re-quantizes `>>3` at PACK — sub-LSB differences from hardware's 5-bit
  halving (gl audit). Minor but non-zero.
- **Effect**: SW essentially faithful; GL has occasional ±1/31 channel error
  on blended pixels. Low priority relative to D1/D2.

### D5 — Mono-quad / textured-quad triangle split winding may flip shared-edge fill vs hardware scan order
- **Oracle**: draws the quad as two tris but with its own core-vertex / Y-sort
  per tri and half-open edges, so the shared diagonal is covered exactly once.
- **Ours**: split `(0,1,2)` + `(2,1,3)` (gpu.c:1610-1611). Combined with the
  inclusive-span bug (D2) the shared edge is drawn by both halves. For
  Gouraud quads the diagonal interpolation can also differ because each half
  re-sorts vertices independently in float.
- **Effect**: visible diagonal seam on semi-transparent quads; couples with D2.
  Fixing D2 (half-open spans) largely resolves this.

### D6 — Sprite (rect) flip + texture-window interaction not modeled
- **Oracle**: sprites honor GP0(E1) bits 0x3000 SpriteFlip — FlipX sets
  `u_inc=-1` and forces `u|=1`; FlipY `v_inc=-1` (gpu_sprite.cpp:29-40), with
  a documented low-bit-of-u quirk. Sprites clip inclusively (`ClipX1+1`,
  half-open loop) and are never dithered (use the zero dither cell).
- **Ours**: textured rects/sprites step U/V forward only (sw:1207-1233,
  `u + col/s`); no flip handling, and texture-window is applied via
  `texel_fetch` (OK) but the sprite path computes its own uv limits for
  filtering. The 8x8/16x16 sprite handlers always step forward.
- **Effect**: games using hardware sprite-flip render mirrored sprites
  incorrectly (wrong texels / shifted by the `u|=1` quirk). Many 2D titles
  flip sprites for character facing; this is a real correctness gap. Lower
  frequency than D1/D2 but visible where used.

### D7 — Rectangle/sprite size & coordinate clamping differs slightly
- **Oracle**: variable-rect size `w = cb & 0x3FF`, `h = (cb>>16)&0x1FF`;
  coords sign-extended to 11 bits then offset added (gpu_sprite.cpp:147-173).
- **Ours mono rect**: `w = cmd & 0xFFFF` then clamp to 1023 / 511
  (gpu.c:1898-1901) — should mask `&0x3FF`/`&0x1FF` like the textured-rect
  path (gpu.c:1919-1920) and the oracle, not clamp a full 16-bit value.
- **Effect**: edge case; a rect width in 0x400-0xFFFF would be clamped to 1023
  instead of wrapped/masked. Rare. Low priority.

### D8 — GPUSTAT bit 25 (DMA-request) and bit 26/28 (ready) are constant, not FIFO-derived
- **Oracle**: bit 26 = `InCmd==NONE && DrawTimeAvail>=0 && FIFO empty`; bit 28 =
  `CalcFIFOReadyBit()`; bit 25 depends on the actual FIFO state and DMA dir
  (gpu.cpp:522-540, 1278-1324).
- **Ours**: bit 26 always 1, bit 28 always 1, bit 25 = 1 for dir 1/2/3
  unconditionally (gpu.c:1266-1281). We process commands instantly so there is
  no FIFO occupancy to report.
- **Effect**: code that polls GPUSTAT for FIFO-full/busy before pushing more
  words never stalls — usually benign (we are "infinitely fast"), but games
  that gate timing on bit 26/28 transitions see no transition. Tied to the
  broader timing-axis work; not a pixel-accuracy issue.

### D9 — LCF / vblank pacing via a poll-count fallback, not cycle-derived
- **Ours**: `gpu_read_gpustat` flips LCF and raises VBLANK after 1000 polls
  (gpu.c:1228-1236, `GPUSTAT_POLL_VBLANK_THRESHOLD`). This is an approximation
  to break BIOS spin loops, separate from the cycle-paced vblank.
- **Effect**: interlace-field bit 31 and vblank timing are not hardware-faithful
  (a heuristic). Belongs to the timing axis (FAITHFUL_TIMING_PLAN.md), flagged
  here for completeness.

### D10 — Display path: 5→8 bit expansion differs from oracle scanout
- **Oracle scanout** (15bpp): `(c & 0x1F) << 3` — low bits zero, NO replication
  (gpu.cpp:1373-1383). 24bpp packs 3 bytes across two words (:1351-1369).
- **Ours**: `rgb555_to_argb` replicates `(c5<<3)|(c5>>2)` (sw:1503-1509).
- **Effect**: our screen is marginally brighter in the low bits than the
  oracle's exact scanout. This is a *present-time* cosmetic choice (arguably
  better-looking) but means present-time RGB diffs vs the oracle's framebuffer
  output are non-zero. VRAM contents themselves are unaffected. Low priority;
  document as an intentional present-time deviation or gate behind the existing
  `PSX_SCREEN` LUT.

### D11 — No oversize-primitive rejection
- **Oracle**: drops any tri/line whose vertex span exceeds 1024×512
  (gpu_polygon.cpp:704-728; gpu_line.cpp:222-226).
- **Ours**: no such guard; a malformed/huge primitive rasterizes fully (slow,
  and can scribble across VRAM where hardware would have dropped it).
- **Effect**: rare; mainly a robustness/timing divergence. Low priority.

### D12 — Textured-polygon texpage attribute did not update GPU draw-mode state — FIXED 2026-07-05
- **Oracle**: every textured polygon's texpage word is copied into the GPU
  draw-mode state before drawing — `SetTPage(CB[4 + ((cc>>4)&1)] >> 16)` runs
  for all GP0 0x20-0x3F with the texture bit (gpu.cpp:1070-1072). The poly's
  own word (not the last E1) selects its semi-transparency mode, and later
  rect/sprite prims (no texpage word) consume the state the poly left behind.
  Poly words cannot alter dither/draw-to-display (E1-only bits 9-10) nor
  texture-disable (bit 11 latches only under GP1(09h) TexDisableAllowChange).
- **Ours (pre-fix)**: textured polys fetched texels from their own tpage but
  blended with the stale E1-set global `semi_transparency`, and never updated
  `texpage_x/y/colors` for subsequent rects.
- **Effect**: additive glows (mode 1 in the prim word) blended with whatever
  mode the last E1 set — Ape Escape's headlamp halos / siren flares / firework
  bursts rendered as dark blobs or black starbursts, intermittently (depends
  on OT bucket order, hence camera/distance). Proven on Ape attract frame
  60052: nine op-0x2E quads at the artifact coords carried prim mode 1 vs E1
  mode 0. Fixed in `gpu.c set_tpage_from_poly()`, called by all four textured
  poly handlers; all backends inherit via `gr_set_semi_transparency`.

### Items that are FAITHFUL (no action)
- Semi-transparency 4-mode selection + STP-bit gating (SW and GL).
- Mask set/check including in copies (SW and GL).
- Texture window AND/OR formula; CLUT 4/8/15-bpp addressing; texel-0
  transparency.
- Color modulation ×2-around-0x80 rule.
- GP0/GP1 command coverage and word counts match the oracle table.
- GL VRAM readback is 15-bit exact.

---

## 3. Prioritized fix list (player-visible impact first)

**P1 — Implement dithering (D1).** Add the 4×4 signed matrix and the
`dtd`-gated apply (Gouraud + modulated-textured + lines; not sprites/copies)
to BOTH renderers. Plumb `dither_enabled` from `gp0_exec_draw_mode` into
`gr_set_*` (it is already decoded; just unused). SW: add `matrix[y&3][x&3]`
before the `>>3` truncation in the gouraud/textured span writers. GL: add the
matrix in the fragment shader before the 5-bit pack. Highest visual payoff;
removes the largest systematic oracle diff.

**P2 — Fix SW rasterizer fill convention (D2 + D5 + D3).** Replace the float
scanline rasterizer with the PSX fixed-point edge DDA: half-open X spans
(`x < x_bound`), top-inclusive/bottom-exclusive Y, the `MakePolyXFP`
`-(1<<11)` top-left bias, and the `+0.5` attribute seed. This removes
shared-edge double-draw (visible on semi-transparent quads/triangles),
the +1px right/bottom over-fill, and brings interior sampling in line with the
oracle. This is the core "faithful rasterizer" item — biggest correctness win
after dithering. (GL coverage stays ±1px by nature of the HW rasterizer;
accept as documented, or move to a compute/SW-on-GPU rasterizer later.)

**P3 — Sprite flip + exact sprite stepping (D6).** Honor GP0(E1) FlipX/FlipY
(`u_inc=±1`, the `u|=1` FlipX quirk, `v_inc=±1`) in the SW sprite/rect path and
GL. Needed for correct 2D character facing in sprite-heavy titles.

**P4 — Oversize-primitive rejection (D11).** Add the `|Δx|>=1024 || |Δy|>=512`
drop in the SW (and GL) primitive entry points to match the oracle and avoid
runaway VRAM writes.

**P5 — GPUSTAT FIFO/ready bits + LCF pacing (D8, D9).** Derive bits 25/26/28
and LCF/vblank from the cycle/FIFO model rather than constants/poll-count.
Cross-listed with the timing axis (FAITHFUL_TIMING_PLAN.md); do as part of that
front, not in isolation.

**P6 — Cleanups (D7, D4-GL, D10).** Mask mono-rect size to `0x3FF`/`0x1FF`
(D7). Do GL mode-0/3 halving at 5-bit to drop the sub-LSB blend error (D4).
Decide D10 (5→8 expansion): either match the oracle's `<<3` scanout for
diffability or keep replication as an explicit present-time option.

---

## 4. Validation method

Two independent processes share the JSON debug protocol (CLAUDE.md §16):
`psx-runtime` (native, TCP **4500** on this wt/tomba2 worktree per build-t2;
4370 on master) and `psx-beetle` (oracle, TCP **4382**/4380). Validate per the
ring-buffer-first rule — query always-on state, never arm-then-capture.

1. **VRAM byte-diff (primary).** Drive both processes to the same scene
   (same input script, same frame count). Pull VRAM from each via
   `vram_transfer_out` / `emu_read_ram` (or `glb_vram_diff` /
   `gl_renderer_fbo_peek` on the GL side) and byte-compare the 1024×512×2
   region. A faithful renderer is byte-identical to the Beetle SW oracle in
   the drawn region (mask bit included). Expect D1 (dither) and D2 (edges) to
   dominate the initial diff map; success = those regions collapse to zero
   after P1/P2. Diff per-primitive by single-stepping the GP0 stream if a
   whole-frame diff is too noisy.

2. **Targeted GPU test ROMs.** Run hardware GPU conformance ROMs on both:
   - Dithering on/off gradient tests (validates D1 matrix values + dtd gate).
   - Triangle/quad edge & fill-rule tests (validates D2 half-open + top-left).
   - Semi-transparency 4-mode swatches over a known backdrop (validates blend
     math, especially mode-0 floor and mode-2 clamp).
   - Mask-bit set/check tests (draw, set mask, overdraw, verify rejection).
   - Texture-window wrap + CLUT 4/8/15-bpp sampling grids.
   - Sprite FlipX/FlipY tests (validates D6).
   Byte-diff the resulting VRAM against Beetle for each ROM.

3. **Per-primitive oracle agreement harness.** Extend the existing GP0 capture
   (shaded-quad capture already exists, gpu.c:1632) into a general "replay one
   GP0 command into a scratch VRAM region on both backends and diff" tool, so
   each rule (edge, dither, blend, mask, flip) is pinned by a minimal
   regression case rather than a whole-scene screenshot. Per Rule 15, fix the
   tool if a diff can't be localized — do not infer correctness from two
   renderers agreeing.

4. **Regression breadth.** After each fix, re-run the diff harness across
   Tomba, Tomba2, MMX6 (and the BIOS shell) since the renderer is shared
   framework code; a faithful-rasterizer change must be validated on every
   title, not just the one under test.
