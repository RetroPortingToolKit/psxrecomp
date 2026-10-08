# Smooth motion (frame generation, `[video] frame_generation`)

Opt-in, OpenGL with the render thread only, off by default
(`PSX_FRAME_GEN=0/1` overrides for one run). Off, nothing in this document
runs and the renderer is the render thread of docs/RENDER_THREAD.md.

```toml
[video]
render_thread = true
frame_generation = true
# How in-between frames are made: "redraw" (default) or "reprojection".
frame_generation_method = "redraw"
```

`frame_generation_method` (`PSX_FRAME_GEN_METHOD=redraw|reprojection`
overrides it for one run):

- **`redraw`** (the default for every title): each in-between frame draws the
  recorded list again from an in-between camera (How, below).
- **`reprojection`** (opt-in per title, in its `game.toml`): each in-between
  frame warps the newer real frame's finished image (Reprojection, below). It
  is cheaper (about a millisecond of GPU per frame) but its object rules were
  tuned on R4 and are assumptions about how a game draws; on other titles it
  has shown flicker (THPS2), so no title gets it unless it opts in.

With Smooth motion off, or in `redraw`, nothing of reprojection runs or is
allocated: no snapshots, depth image, warp programs, HUD matching, per-frame
`getenv` or uniform lookups. Switching to `redraw` (or Smooth motion off)
frees what reprojection held.

## Rule

Real frames at the guest's rate come first and resolution is the only
quality lever for them (dynamic resolution). Frames above that are a bonus,
drawn by the render thread **from recorded data only**: no guest code runs
again, nothing is snapshotted or rolled back, and only when the render thread
has measured time to spare.

## How

- **Lists.** The render thread keeps a copy of every replayed state and
  drawing record (not transfers, copies, presents, steps). Display buffers are
  learned from presents; a list is one game frame's drawing into one buffer and
  closes when drawing moves to another buffer, or when the game flips to the
  buffer being drawn. (R4 starts drawing the next frame a VBlank before it
  flips, so a flip alone is not a frame boundary.)
- **Cadence.** A flip (a present of a different display address) is a new
  game frame; a present of the same buffer is the same frame again. R4
  updates every other VBlank, so generation interpolates between flips (30 Hz
  game frames), not between VBlanks.
- **Sources.** While generation is on, every RTPS/RTPT output is noted
  (gte.cpp, `gte_fg_source_*`) with an identity (the issuing function `ra`
  and the model-space vertex), the camera-space position the GTE divided and
  H, indexed by the packed screen word. The same point projected again in a
  frame (a corner shared by separate meshes, another function) is one vertex
  (the smaller identity); up to two different points on one pixel are kept,
  and gpu.c picks the one projected by the same function as the triangle's
  other vertices, else the one nearest their depth. gpu.c looks each polygon vertex up by
  its packet word and records the sources ahead of the triangle
  (`RTH_FG_SRC`, never drawn). CPU-built vertices have none. Guest-visible
  GTE results are untouched.
- **Camera** (`frame_gen.c`, `fg_cam_fit`). No primitive is paired by draw
  order. Per view (draw area), vertices of the two frames with the same
  identity are paired in camera space (instances: the clearly nearest) and a
  rigid motion is fitted (RANSAC + least squares, inliers within 1 screen px
  at their depth). The static world agrees with it; paired vertices that
  disagree are objects (cars, the followed car). Verdict: no projections, a
  view with few pairs or under half inliers, or a turn/travel no camera makes
  between two frames rejects the generated frame (the real frame shows).
- **Drawing** (`fg_cam_place`). One real frame is redrawn from an
  in-between camera: world vertices re-projected through the fraction of the
  fitted motion, object vertices lerped in camera space, CPU-built vertices
  of placed triangles moved with their neighbours, everything else (HUD, 2D,
  sprites, the mirror's own view if unprojected) unchanged. Moving forward
  the older frame is redrawn (seen from further on it spreads past the
  screen edges); otherwise the newer. Drawing starts on the newer real
  frame's image and skips the clears before the first draw, and strips along
  a view's edges the picture moved away from are copied from that image, so
  what the in-between camera uncovers shows the newer frame instead of a
  hole. Every placed vertex at one screen position moves alike (a weld: the same
  corner reaches the GPU from several projections), and a vertex without a
  projection lying on an edge between placed vertices moves as that point of
  the edge (closes the T-junctions the game's polygon splits leave). A game
  frame whose in-between camera would pass through geometry (a vertex behind
  it: tunnel ceilings, overhead bridges) is not generated: without near-plane
  clipping its image would be wrong. The raw texture
  mirror is not packed from the generated surfaces; all bookkeeping the draws
  touch is restored, so the real stream is unaffected.
- **Schedule.** With `n` in-between frames per game frame, at the flip the
  real frame is composed and kept (its buffer may be drawn over before it is
  shown), frames `t = k/(n+1)` are presented `flip/(n+1)` apart, then the
  kept real frame. Timed work runs from the render-thread core's `tick`
  hook between records and while idle.
- **Latency.** Interpolation shows a real frame after the in-between frames
  that lead to it: up to `n/(n+1)` of a game frame later than without
  generation (R4 at 30 Hz on 120 Hz: 25 ms at n = 3, 17 ms at n = 1).
  Extrapolation (no delay, guessed motion) is not offered.
- **Plan.** `n = fg_plan(...)`: slots = round(flip interval × refresh)
  (R4 on 120 Hz: 4), limited to what fits in 85 % of the interval after the
  real frames' render-thread CPU time and one swap, at the measured cost of a
  generated frame (CPU, GPU via `GL_TIME_ELAPSED`, plus a swap). On macOS a
  real frame's `TIME_ELAPSED` span reads close to the whole interval (it
  counts the GPU waiting for records), so it is not used here; GPU overload
  shows as backpressure. The measured frame cost dynamic resolution uses
  excludes generation (its query is paused, segments summed). With the
  present thread (`[video] present_thread`, docs/RENDER_THREAD.md) a swap
  here is the hand-off to it, and its cost is the time spent waiting for a
  free slot, not the compositor's round trip.
- **Breaker.** Off for 3 s (doubling to 24 s on repeats within 10 s) after
  the queue backed up (the guest waited), the render thread fell two frames
  behind, or a guest frame took over 1.5 intervals. While dynamic resolution
  is over budget or stepping down, generation pauses (1 s, no escalation).
  One frame behind just shows the waiting real frame at once.
- **Sync points** show the waiting real frame and restart the lists at the
  next flip.

## Reprojection (`frame_generation_method = "reprojection"`)

- **Source.** At each flip the newer list's finished image is copied
  (`rp_snapshot_list`): the game draws on into its buffers while the
  in-between frames are shown. The camera is fitted for the newer frame
  (`fg_cam_fit`, a view that fails leaves its vertices unchanged; a third of
  the pairs is enough, since whatever moves on its own is redrawn).
- **Depth image** (once per game frame, 2 texels per native pixel): the
  newer list's recorded triangles in painter order, each pixel one of world
  (every corner placed by the camera: its 1/z and view), object, keep, or HUD
  needle.
- **Warp** (per in-between frame, one draw): a grid with a vertex per native
  pixel; a world vertex is unprojected with its depth, moved by its view's
  in-between camera (`fg_view_affine`) and projected again (nearest wins);
  cells over a depth edge, a view boundary or a non-world pixel are not
  drawn, so the snapshot underneath shows there (disocclusion is filled with
  the newer frame). Keep pixels (2D, HUD, the mirror) are then put back from
  the snapshot; needle pixels from the older snapshot. The warp's depth is
  cleared before anything else is drawn (with PGXP depth the cars would
  otherwise test `LEQUAL` against it).
- **Objects.** Cars are drawn again from the recorded list at their own
  in-between position; HUD needles are moved as `fg_hud_lerp` moves them.
- **Breaker.** Each method has its own breaker settings, set again when the
  method changes: reprojection holds 0.1 s (at most 1 s; redraw 0.5 s to
  8 s). Dynamic resolution over budget does not pause reprojection (its
  frames cost little; a step down still pauses it 0.05 s), and a late guest
  frame trips it only when its in-between frames cost over 3 ms of CPU. The
  generated-frame cost estimate moves at most 4x per sample.
- **Dynamic resolution** budgets the real frames as their cost against their
  share of the frame (redraw: the share of the period).

### Object rules (R4-tuned assumptions)

These rules decide what the warp moves and what is drawn again. They were
tuned on R4; a title that opts in gets them as they are:

- **Cars** are triangles the fit calls objects (`FG_PLACE_OBJECT`) whose
  corners really moved against the world (more than 4 % of their depth from
  where the camera alone puts them; near road subdivided past the fit's
  tolerance is world), **textured** (an untextured gradient is backdrop), and
  **small** (at most 96 x 64 native px). Anything else the fit moves on its
  own (a sky dome) is backdrop the warp keeps where it is.
- **Backdrop.** A view's triangles drawn before its first world triangle are
  backdrop and are not drawn again.
- **HUD needles** are 2D triangles at most 48 px on a side that moved at most
  24 px per corner between the two frames and drew the same thing
  (`fg_hud_match`); they are drawn at their in-between position over the
  older frame's dial.
- **HUD protection.** 2D pieces up to 160 x 120 px (rects and triangles) of
  the newer and the older list are keep pixels: the newer frame's pixels are
  shown there, never warped. Big 2D (a backdrop drawn first) is not.
- **Small views** (a view whose area is under 15 % of the biggest, R4's
  mirror) are keep: shown as the newer frame drew them.
- **Split screen.** Each view is fitted on its own; one view's failed fit
  leaves the others reprojected.

Diagnostics (read once when the programs are built): `PSX_RP_DEBUG=1` shows
the warp grid, `=2` the pixel kinds; `PSX_RP_NOPREV=1` draws needles over the
newer frame's dial. `frame_gen` reports `reproject`, `reprojected`,
`reproject_fallback`, `rp_ms` and `rp_zn`.

## Limits

- Only GTE-projected triangles move; sprites, lines and fills are the
  redrawn frame's. Areas uncovered by the in-between camera show the newer
  frame. Vertices without a projection that are neither on a placed edge nor
  shared take their triangle's mean motion (`guessed` counts those over a
  1 px spread; 0 in R4 races once shared corners resolve).
- A texture the frame drew earlier into its own displayed buffer is sampled
  as the raw mirror holds it when the in-between frame is drawn.
- Single-buffered games never flip, so nothing is generated.
- Windowed high-resolution mode and depth24 frames are not generated.
- HD texture replacements or texture dumping hold the render thread on the
  synchronous path, so Smooth motion does not generate frames while either
  is active. Disabling both permits the thread to resume; saved preferences
  are unchanged.

## Debug

`{"cmd":"frame_gen"}` (not a sync point): generated / real presents, flips,
dups, plan (`last_n`, `slots`), costs (`real_ms`, `gen_ms`, `swap_ms`), last
camera fit (`views`, `pairs`, `inliers`, `place_*`, `cam_angle_deg`,
`cam_shift`), verdict (`verdict_ok`, `rejected`, `reject_why`), breaker,
ceiling, last hold, total swaps.

`PSX_PRESENT_SHOT_GENERATED=1` (diagnostic): a staged `present_shot` is
taken at the next generated frame instead of the next present, to inspect
in-between images in game. `PSX_PRESENT_SHOT_BURST=N`: a staged
`present_shot` takes the next N composed frames (written after the burst as
`<path>_NN_{r|g}_f<flip>.png`, in composition order: a real frame precedes
the in-between frames that lead up to it).

## Tests

- `frame_gen_test`: the camera fit on a synthetic grandstand of one
  repeated texture, redrawn in another order with a row culled and one added
  (the case the old draw-order matcher paired wrongly: 225 of 226 pairs) -
  phase 0 / 0.5 / 1 land where the older / halfway / newer camera sees each
  vertex; a followed car, a HUD triangle, a CPU-built vertex; the verdict
  (no projections, a cut, an impossible turn, inconsistent motion); the plan,
  breaker, cost, pace, ceiling.
- `render_thread_test`: the tick hook runs on the render thread with the
  context, never early, and wakes an idle thread.
- `gl_frame_gen_test` (real GL, `gpu;opengl;hardware`): a double-buffered
  30 Hz scene, VRAM and native-wide presents, flipping right after drawing
  and a VBlank late: the distinct real presented images are identical with
  generation off and on, generated frames are presented, and an in-between
  frame composed at t = 1 / t = 0 equals the newer / older real frame. In
  redraw (the default) the fixture checks that nothing of reprojection ran
  or was allocated and prints the phase 0.5 image's digest (`p05=`), which
  matches master's. With `FG_METHOD=reprojection` the warp's pixels are
  checked: with a parallax pan (a projected backdrop twice as far as the
  triangles) phase 0.5 matches the redraw of the same pair (under 5 % of the
  pixels differ, under half of what the unwarped newer frame differs), every
  pixel is a real frame's or the redraw's (no holes), the HUD is exactly the
  newer frame's, the real frames are those of generation off, and with PGXP
  depth the warp leaves no depth behind for the cars.
