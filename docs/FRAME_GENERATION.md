# Smooth motion (frame generation, `[video] frame_generation`)

Opt-in, OpenGL with the render thread only, off by default
(`PSX_FRAME_GEN=0/1` overrides for one run). Off, nothing in this document
runs and the renderer is the render thread of docs/RENDER_THREAD.md.

```toml
[video]
render_thread = true
frame_generation = true
```

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
  frame composed at t = 1 / t = 0 equals the newer / older real frame.
