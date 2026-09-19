# OpenGL Renderer — Handoff

Branch: `feat/gl-renderer` (both psxrecomp and TombaRecomp).
Last commits this session: `0631538`..`45cc5c3` (psxrecomp).
Status: **v2 architecture, opt-in, rendering cleanly on every screen
tested.** Software renderer is still the committed default; OpenGL is
selected per-game via `[video] renderer = "opengl"`. TombaRecomp
`game.toml` has LOCAL (uncommitted) overrides (`renderer = "opengl"`,
`texture_filtering = "bilinear"`, `supersampling = 2`) for testing — do
NOT commit those; committed defaults are `software` / `nearest`.

Goal: a hardware (OpenGL) renderer as a *selectable option* alongside
software, enabling higher internal-resolution scaling without the software
renderer's per-pixel CPU cost. Make it default-with-software-fallback only
after broad visual validation.

---

## Architecture (v2 — "GPU-authoritative VRAM")

- **Facade + vtable.** `gpu.c` calls `gr_*` (gpu_render.h/.c); a
  `GpuRenderBackend` vtable dispatches to software (`sw_*`) or OpenGL
  (`glb_*`). `gr_set_backend()` picks at startup from the config.
- **GL backend** in `runtime/src/gpu/gpu_gl_renderer.c`
  (+ `runtime/include/gpu_gl_renderer.h`). The single authoritative VRAM is
  the **hr FBO** (`s_hr_tex`, RGBA8, 1024·S × 512·S, + depth24/stencil8);
  internal scale S comes from `[video] supersampling` (GL-side; the SW
  hi-res mirror is forced off under GL).
- **Mask bit exact:** FBO alpha carries bit15; the stencil buffer mirrors it
  (bit0). `check_mask` = stencil test EQUAL 0; writing 1 under check uses
  GL_INVERT. Textured prims/copies/uploads draw in two passes split by the
  texel STP bit so each pass writes one known stencil value.
- **Coherency model** (all state in gpu_gl_renderer.c):
  - `s_up_pending`: CPU→VRAM writes land in the CPU array immediately and
    accumulate a pending rect; `flush_cpu_upload()` quad-blits the rect into
    the hr FBO (and subimages the raw mirror directly) before the next GPU
    op, present, or readback.
  - `s_pack_dirty`: hr-FBO content not yet re-encoded into the native R16UI
    raw mirror (`s_raw_tex`) — the texture-sampling source and readback
    surface. `pack_flush()` runs a scissored PACK pass over the union before
    any textured draw that samples a dirty page/CLUT.
  - `s_gpu_dirty`: CPU array stale; `ensure_cpu()` = flush + pack + full
    readback. Used by GPUREAD, A0 mask-check reads, vram peek, screenshots,
    24-bit present.
- **Texture sampling (Beetle-PSX model, structure studied from
  beetle-psx-libretro `rsx/`):**
  - Per-primitive **uv sampling bounds** (`u_limits`), clamped in the
    fragment shader when no texture window is active. Filtered neighbours
    and S>1 interpolation overshoot can never read outside the prim's own
    texture rect. Rect prims pass exact bounds ([u0, u1-1] forward); 
    triangles compute min/max with the exclusive max edge backed off for
    axis-aligned (2D) uv mappings; uv ranges crossing a 256 wrap boundary
    widen to the full page.
  - **Bilinear** (`texture_filtering = "bilinear"`): nearest texel is the
    base (cutout + STP authority), neighbours toward the sub-texel offset,
    per-texel opacity weights with renormalised colour, discard iff
    opacity < 0.5. The SW renderer's bilinear uses the same formulation
    (GL/SW parity); SW nearest is untouched.
  - **Sample-grid alignment:** all draw shaders shift positions by
    `u_shift = 0.5/S − 1/64` so GL's center-sample grid matches the PS1
    integer DDA (fixes striped/squished fonts). Beetle instead uses a
    `+0.001` uv epsilon and truncation — coarser; we keep u_shift.
- **GPU ops:** fills (scissored clear, wrap-split 4 ways), copy_rect
  (hr→scratch blit + masked quad back), all triangle/rect/line prims,
  batched CPU→VRAM uploads. Blending via glBlendFuncSeparate — alpha (mask
  bit) is never blended.
- **Present:** one deterministic path per depth. 15-bit always blits the
  display rect from the hr FBO (4:3 letterboxed); 24-bit FMV syncs and
  presents the CPU conversion (letterboxed). The old per-frame FBO-vs-CPU
  alternation (menu jitter) is structurally gone.
- **Init is all-or-nothing:** any shader/FBO failure → clean fallback to
  pure software.

## Verified (2026-06-12 session)

- BIOS → intro FMV (24-bit) → title → memcard slot screen → save-block list
  all render **clean** under GL at S=1 and S=2 with bilinear on:
  - Title vertical line at VRAM x=640 — GONE.
  - Memory-card screens' cross seams (tile edges at 704/864/256/368) — GONE.
  - Menu text fully readable under bilinear.
- Root cause of all the black lines: the v1 bilinear base texel was
  `floor(uv − 0.5)` — one texel OUTSIDE the prim on top/left edges. With
  u0=v0=0 tiles (card wallpaper, title bg page seam) the neighbour wrapped
  to empty VRAM → texel 0 → whole edge column/row discarded. They were
  never coherency holes; both the FBO and (post-sync) CPU VRAM agreed.
- gl_vram_diff at a quiesced (static-screen) moment: **0 mismatches**.
- Audio fade (below) built and boots clean; user feel-test still pending.

## Diagnostics (debug TCP 4470, dev builds)

- `gl_fbo_peek x= y= w= h=` — read GPU-side VRAM rect (via pack) without
  writing CPU VRAM; returns pending/pack rects + gpu_dirty.
- `gl_vram_diff` — full-VRAM FBO-vs-CPU mismatch count/bbox/samples. Forces
  a full pack first so it reads FBO truth even if the raw-mirror invariant
  broke. **Run it BEFORE any screenshot** — screenshots ensure_cpu (CPU :=
  FBO) and make the diff trivially clean.
- `gl_coh_ring n= [frame_min=]` — always-on coherency event ring: flushes,
  fills, copy src/dst, draw bboxes, packs, readbacks, presents, probe
  perturbations, each with rect + frame. The event AFTER a "flush" names
  the op that triggered it. Keep n ≤ ~2000 per query (TCP send budget).
- Caveats: peeks/diffs themselves flush pending uploads mid-frame (they're
  recorded in the ring as peek/diff events). Animated screens legitimately
  diverge mid-frame (gpu_dirty=1); only quiesced screens give clean diffs.

## Open / deferred

1. **User visual validation** of the whole pass (incl. fullscreen letterbox
   + audio fade feel) — everything above was screenshot-verified only.
2. Gameplay smoke under GL this build (last session verified gameplay
   renders; this session's shader changes were validated on title/menu
   screens which exercise the same textured paths).
3. Mirrored-texture mappings: bilinear/limits keep the full inclusive range
   for mirrored rects (no exclusive-edge backoff); ±1 texel at
   exact-integer uv remains the documented 1/64-bias tradeoff. Beetle
   handles mirrors with a +1 uv offset (`off_u/off_v` in
   gpu_polygon_sub.cpp) — adopt if a mirrored sprite ever shows it.
4. Dirty-sub-rect readbacks (ensure_cpu is a full 1 MB read), dither (SW
   lacks it too), GL_LINES vs Bresenham edge cases.
5. S=4 unvalidated (S=2 verified this session).

## Audio fade (shipped with this branch, main.cpp)

`turbo_loads` mutes sequenced load BGM (tempo is frame-bound; can't
decouple). `sdl_audio_update()` ramps gain over 40 ms at mute/unmute edges
with an 8-frame unmute hangover to debounce burst gaps. The fade tail is
sized to the ramp and clamped to `sdl_audio_buf` (the first version
overflowed the 2048-frame buffer and corrupted adjacent statics — wedged
the boot before display enable).

## Useful commands / paths

- Build: `PATH=/c/msys64/mingw64/bin:$PATH cmake --build build-stable --target psx-runtime`
  (from `TombaRecomp/`; GL source is in `psxrecomp/runtime/`).
- Run: `./build-stable/psx-runtime.exe --game game.toml` (BIOS path baked).
- Debug: `python ../psxrecomp/tools/debug_client.py --port 4470 <cmd>`;
  `screenshot_file path=...png` (CPU-side render after FBO sync — native
  res, shows FBO truth; the window shows the S× blit).
- Input injection: `press buttons=N frames=M` is consumed faster than some
  screens poll — prefer `set_input buttons=0xNNNN`, sleep ~1–2 s, then
  `clear_input`. Circle (0x2000) = confirm in Tomba menus; Start (0x0008)
  skips the intro FMV.
- A/B vs software: `sed -i 's/renderer          = "opengl"/renderer          = "software"/' game.toml`.
- Beetle PSX GL reference (GPL — structure only, never copy code):
  `psxrecomp/beetle-psx/rsx/rsx_lib_gl.cpp`, `rsx/shaders_gl/*.glsl.h`,
  `mednafen/psx/gpu_polygon_sub.cpp` (uv limits, may_be_2d, mirror offsets).
- GP0 ring decode: `python TombaRecomp/_shots/decode_frame.py <frame>`.
- Attract demo auto-plays on cold boot (AP 97400 = canned demo).
