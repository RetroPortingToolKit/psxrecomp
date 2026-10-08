# Render thread (`[video] render_thread`)

Opt-in, OpenGL only, off by default. With it off nothing in this document
runs: `gl_backend_get()` returns the existing table and the GL renderer is
the synchronous path every title already uses.

```toml
[video]
render_thread = true   # game.toml; PSX_RENDER_THREAD=0/1 overrides
```

`PSX_RENDER_THREAD_FRAMES=N` (default 2) bounds the closed frames in flight.

**Player settings.** `render_thread`, `present_thread` and `frame_generation`
are also read from the player's `settings.toml` `[video]`, which wins over the
game.toml default; the `PSX_*` env vars override both for one run and are
never saved. The launcher shows them as Settings → Display rows (Render
thread, Present thread, Smooth motion (frame generation); recomp-ui
`RECOMP_LAUNCHER_HAS_RENDER_PIPELINE`, OpenGL only, the latter two disabled
while Render thread is off). The pipeline starts once at boot, so a change
from the in-game launcher is saved and applies at next launch.

**HD texture safety.** Texture replacements and texture dumping use the
synchronous renderer, not the render thread. An active HD session prevents
thread startup. Enabling HD after startup drains and parks the render thread
(also pausing Smooth motion); it resumes at a frame boundary after both
replacements and dumping are disabled. Preferences are preserved, and this
applies to every title. Dynamic resolution remains available on the synchronous
path. The log explains the fallback.

## Why

Before this change one thread ran the recompiled guest, turned every GP0
command into GL calls (`gpu_gl_renderer.c`), presented, and paid every
Apple GL-on-Metal stall: command-buffer waits, uploads, readbacks, the
swap. A frame cost `emu + GL`. With the render thread a frame costs
`max(emu, GL)`, and the guest waits for rendering only at the sync points
below.

## Model

```
emulation thread                         render thread (owns the GL context)
----------------                         -----------------------------------
guest + gpu.c (GP0/GP1, VRAM array,      replays records in order through the
status, timing)                          unchanged glb_* functions; presents
  gr_* -> GL_RT_BACKEND (record) ---->   ring (32 MiB, SPSC) ---->  exec()
  present  (record + overlay copy)
  vblank: frame boundary --------------> frame marker; <= N closed frames
  sync point: drain, take context <----  release context, park
  ... direct GL calls for the rest of the frame ...
  frame boundary: hand context back ---> make current, continue
```

Pieces, with file:line on `feat/render-thread`:

- **Core** — `runtime/src/render_thread.c`, `runtime/include/render_thread.h`.
  Single-producer/single-consumer byte ring with monotonic 64-bit positions.
  A record never splits across the ring end (a pad record fills the tail);
  payloads are copied, and space is freed only after `exec()` returns. No
  lock is held while a record executes; a mutex and two condition variables
  guard sleeping only (each side raises its `*_sleeping` flag under the lock
  before re-checking, the waker publishes its position before reading the
  flag). The consumer spins briefly before sleeping; the producer wakes a
  sleeping consumer every 128 KiB / 256 records and always at a frame end,
  drain or sync point. On macOS the thread is created with
  `QOS_CLASS_USER_INTERACTIVE`: with the default QoS a busy host scheduled
  it onto efficiency cores and it fell behind the emulation thread (R4 4K
  race on a host at load average ~40: 40 Hz with the default class, 60 Hz
  with this one).
- **Frame bound** — `rt_frame_end()` blocks while more than N closed frames
  are unconsumed (backpressure). `rt_frames_ahead()` tells the replaying
  thread how many complete frames are queued behind the current one.
- **Sync point** — `rt_acquire()`: drain, have the render thread release the
  context (`glFlush`, `SDL_GL_MakeCurrent(win, NULL)`), make it current on
  the emulation thread. The emulation thread then *holds* the context and
  calls the backend directly, exactly as with no render thread, until the
  next frame boundary hands it back (`rt_release()`). So a sync point costs
  at most one hand-off per frame, and code that is not converted to
  recording stays correct.
- **Recording table** — `GL_RT_BACKEND` (`gpu_gl_renderer.c:8469`), selected
  by `gl_backend_get()` while the thread runs (`gr_refresh_backend()` in
  `gpu_render.c`). Every state call, primitive, fill, copy, CPU->VRAM upload,
  single-pixel write and native-wide call is recorded by value; queries are
  answered from emulation-side mirrors or are sync points (table below).
- **Hand-off rules** — `gl_renderer_render_thread_frame_boundary()`
  (`:8653`), called from `sdl_vblank_present()` after the present and before
  wall-clock pacing (`main.cpp:8650`). It releases a held context when the
  frame may run asynchronously (`gl_rth_eligible`, `:8173`), takes it when
  it may not, then closes the frame.

### Guest-visible VRAM stays on the emulation thread

The GL backend is GPU-authoritative: `s_hr_fbo` holds VRAM, gpu.c's array is
refreshed only by readbacks (`ensure_cpu`, `:2395`), and pending uploads
stage from that same array (`flush_cpu_upload`, `:838`). gpu.c writes the
array directly while a GP0(A0) payload streams in (`gpu.c:6912`) and hands
the facade a copy at commit (`gpu.c:3156`). If the render thread staged from
the live array it would upload pixels of a later transfer (and its
`sw_vram_transfer_in` would write stale payloads back over the guest's
array).

So the backend's `s_vram` is gpu.c's array whenever the emulation thread
runs GL, and a private copy while the render thread does
(`gl_rth_acquire`/`gl_rth_release`, `:8181`/`:8188`; the SW rasterizer's
pointer moves with it, `sw_renderer_rebind_vram`). The private copy gets
each upload payload in command order; a release first copies gpu.c's array
into it, so it matches the guest at every hand-off. Every readback runs on
the emulation thread under a sync point and writes gpu.c's array, as before.

### Guest state the backend reads while drawing

| State | Read by | Under the render thread |
|---|---|---|
| per-prim widescreen tags (`psx_ws_prim_in_backdrop`, `psx_ws_prim_is_tagged`) | `bd_prim_gate` `:2533` | captured per primitive in the record flags (`rth_prim_flags`) when native-wide and the backdrop stretch are on; replay reads them via `ctx_prim_*` (`:379`) |
| native-wide fast-path latch (`gpu_ws_background_requires_full_composite`, `gpu_ws_netplay_local_viewport_width`), flat backdrop | `wide_fast_center_valid` `:2569` (also at batch flush and present) | sent as an `RTH_STATE` record before the first call that would see a change |
| depth24 display (`gpu_display_is_depth24`) | `depth24_upload_policy` `:4096` and the depth24 helpers | a depth24 display is a sync point (`rth_record_mode`, `:8202`); the render thread only replays 15-bit frames, so `ctx_depth24()` is 0 there |
| host overlays (OSD, volume, rewind strip, savestate menu) | `gl_swap_with_osd` `:7543` | rasterized and copied at record time (`rth_record_present`, `:8216`); `host_osd_present_done` runs at record time |
| present-shot request | `gl_swap_with_osd` | already mutex-guarded (`main.cpp` `present_shot_take`) |

Emulation-side answers without a sync point: `gl_renderer_projective_supported`
(`:3854`) and `gl_renderer_present_wide_fbo`'s return value (`:7981`) from a
mirror of the wide-surface bookkeeping (`rth_mirror_wide_for`, `:8121`, the
same lookup, limit check and slot allocation `wide_fbo_for` does), and
`gr_get_draw_area` from the recorded draw area. `gr_scale`,
`gr_texture_filter` and `gl_renderer_fit_wide_aspect` read values that only
change while the emulation thread holds the context. The mirrors are re-read
from the backend at every release.

## Sync points found in the code

Guest-visible:

| Where | Why |
|---|---|
| GPUREAD `gpu_read_gpuread` (`gpu.c:3743`) -> `gr_vram_read` | VRAM->CPU transfer reads the FBO (`glb_vram_read` -> `ensure_cpu`) |
| GP0(A0) with mask check (`gpu.c:6912`) | per-pixel mask test reads VRAM |
| `gpu_vram_peek` (`gpu.c:6889`) | debug/mod VRAM reads |
| savestate / rewind save (`boot_state.c:148,159,189,356`) | `gr_vram_transfer_out` of all VRAM |
| savestate load (`gl_renderer_restage_vram_after_savestate`, `:5112`) | full restage from gpu.c's array |
| depth24 (FMV) display | the CPU mirror is presented; the emulation thread draws those frames |

Not guest-visible but GL-owned (each is `GL_RT_SYNC` at the top of the entry
point): `gr_render_display*`, `gr_render_wide_display`, `gr_wide_dump_full`
(screenshots, CPU present fallback), every `gl_renderer_pass_*`/`stereo_*`
(render passes), `psx_mod_openxr_*`, texture-bank selection, settings
(scanlines, aspect, gamma, bezel, swap interval, interpolation), the CPU
present / blank / hold-last presents (pause, rewind, netplay hold), and the
debug-server diagnostics (`gl_*` rings, `frame_perf`, `gl_fbo_peek`,
`gl_vram_diff`). Two per-frame debug captures are queued instead of being
sync points: the display ring's readback (`gl_renderer_fbo_peek_deferred`)
and the `--headless-opengl` presented-image capture
(`gl_renderer_ring_capture`); their ring readers sync first
(`gl_renderer_render_thread_sync`). `ensure_cpu` refuses to run on the render
thread: a readback there would land in the private upload source and mark the
CPU side current without gpu.c's array seeing the pixels.

What is *not* a sync point: a frame of primitives, fills, copies, uploads,
native-wide draws and the 15-bit present (`gl_renderer_present_vram` /
`present_wide_fbo`). A Ridge Racer Type 4 race loaded from a savestate takes
one sync point (the load) and then none.

## Frames, backpressure, stale presents

At most N closed frames wait in the ring; past that the emulation thread
blocks in `rt_frame_end`. The render thread must replay every frame's
drawing: later frames sample what earlier frames rendered, so drawing cannot
be skipped. Only the present can. When two newer frames are already recorded
(the emulation thread is at the bound, waiting), the present of the frame
being replayed is skipped (`rth_replay_present`, `:8250`), never twice in a
row, so a saturated render thread still shows at least every other frame.

Consequence: the render thread takes the GL cost off the guest's frame, but
if GL alone exceeds the frame budget the guest is still held back by the
bound. Choosing an internal resolution that fits is the job of dynamic
resolution (below), not of this layer.

## Interoperation

| Feature | Behaviour |
|---|---|
| savestates / rewind | load and save are sync points; the frame stays synchronous, the next frame boundary releases |
| netplay / rollback resimulation | `psx_netplay_active()` or dual-raster makes every frame ineligible: the emulation thread holds the context (synchronous path). Not started when netplay is configured at boot |
| render passes | first `gl_renderer_pass_*` call in a frame is a sync point; the rest of that frame is synchronous. Recommended: leave `render_thread` off with the frame-rate mod |
| frame interpolation | ineligible (held) while enabled; not started when it is on at boot |
| HD texture replacements / texture dumping | ineligible (held) while either is active; not started when HD is active at boot. An existing thread resumes when both are disabled |
| OpenXR | ineligible while a session is active |
| Smooth motion (frame generation) | `[video] frame_generation`: in-between frames drawn by the render thread from recorded lists, docs/FRAME_GENERATION.md |
| native-wide / widescreen | recorded; tags, latch and wide-surface mirror as above |
| internal resolution changes | `gr_set_scale` and every resolution entry point are sync points; dynamic-resolution level steps are recorded (below) |
| screenshots / debug captures | `screenshot*` sync; `present_shot` is fulfilled on the render thread |
| headless | `--headless` (software) never starts it; `--headless-opengl` runs it on the hidden context (no present; the frame boundary still closes every frame), which is what `fp_identity` exercises |
| Vulkan / software | never started (log line says why) |

## Present thread (`[video] present_thread`)

Opt-in, under the render thread, off by default (`present_thread = true`;
`PSX_PRESENT_THREAD=0/1`, `PSX_PRESENT_THREAD_SLOTS=2..4`, default 3). On
macOS a swap on the render thread cost 6-9 ms of waiting on the window
compositor (`[NSOpenGLContext flushBuffer]` on GL-on-Metal) in every frame,
real or generated; with this it costs a fence and a hand-off.

- **Slots.** While it runs, framebuffer 0 of the composing context (the
  render thread's, or the emulation thread's while it holds the context at a
  sync point) is an offscreen slot: every `glBindFramebuffer(..., 0)` goes
  through `gl_bind_fb_redirect` and lands on the current slot, an RGBA8 (RGB8
  if the window has no alpha) texture plus depth-stencil at the window's
  drawable size, reallocated when the window changes. Everything that used
  to be drawn into the back buffer (display quad, OSD, hold-last captures,
  `present_shot`, frame generation's kept real frame) is drawn into it
  unchanged; the two `glReadBuffer(GL_BACK)` reads read its attachment.
- **Swap.** `gl_present_swap`: a fence (`glFenceSync`), `glFlush`, queue the
  slot to the present thread, take the next free slot (waiting only while
  every other slot is queued or on screen: the display is the bottleneck;
  that wait is what the dynamic-resolution and frame-generation ledgers count
  as the swap), and `glWaitSync` on the fence the present thread left after
  its last copy out of that slot.
- **Present thread** (`present_thread.c`, backend-neutral core; GL callbacks
  in `gpu_gl_renderer.c`): owns a second context created on the same window
  with `SDL_GL_SHARE_WITH_CURRENT_CONTEXT` (textures and syncs are shared;
  FBOs are per context, so it keeps its own read FBO per slot). Per slot, in
  submission order: `glWaitSync` on the ready fence, a 1:1 `GL_NEAREST`
  blit to the real framebuffer 0 (scaled only if the window changed since
  the frame was composed), a done fence, the swap. The swap interval is
  applied on its context. `QOS_CLASS_USER_INTERACTIVE` on macOS, above-normal
  priority on Windows.
- **Fallback.** No sync objects, no shared context or a failed start: a log
  line and direct swaps, as without it. It starts and stops with the render
  thread (stop presents what is queued first).
- **Pixels.** The window shows exactly the composed image (gl_frame_gen_test
  reads the window back after each copy). Compared with composing straight
  into the window, the composed image is bit-identical at 1x and for every
  native-wide present; on Apple GL-on-Metal a VRAM present resolved down
  into a smaller window (area resolve) rounded 1 pixel by 1 LSB in 2 of 24
  frames: rasterizing into a texture instead of the drawable. The test
  allows at most 1 LSB on 0.05 % of the pixels and reports it.
- **Latency.** Up to `slots - 1` composed frames can wait for the display.
  Frame generation already paces presents to the refresh, so the queue
  normally holds at most one.
- **R4** (Match display 10x ceiling, dynres, Fit widescreen, 120 Hz panel,
  host at load average 22-26): the render thread's swap went from 4.5-7.8 ms
  to 0.02-0.08 ms; the present thread's swap takes 8-10 ms. 2P VS guest
  56.6-57.6 → 57.2-58.6 Hz; with generation forced (test knob) 1P guest
  25 → 41-44 Hz and presented 50 → 78-85 Hz.

`{"cmd":"render_thread"}` carries `present_thread`: active, slots, submits,
presents, waits and wait time (composing side blocked), swap time (avg/max,
on the present thread), queue high water.

## Dynamic resolution (`[video] dynamic_resolution`)

Opt-in as before (`dynamic_resolution = true`, `dynamic_resolution_min`;
`PSX_DYNRES=0/1`, `PSX_DYNRES_MIN` for one run); no new keys. The level
steps between the configured internal resolution (the ceiling, e.g. Match
display, which the GPU limit and memory budget are checked against) and the
minimum. Every surface (hr VRAM and native-wide) is allocated at the current
level and reallocated at each step: the hr surface is reseeded into the new
allocation from the 1x image plus the displayed rect and draw area at full
detail, the wide surfaces' presented rows and margins are rescaled into
theirs. On Apple's GL-on-Metal every render pass into an attachment loads and
stores the whole attachment, so surfaces held at the ceiling cost the
ceiling's bandwidth at every level; with a 10x ceiling, R4's 2P VS start
(which needs about 4x) ran at 35-40 Hz for its first seconds at any level.
If a new allocation fails, a step down rescales in place inside the old
allocation and a step up past it is refused. With the render
thread off nothing changes: the emulation thread's wall-time controller
(`dynres_*` in `dynamic_resolution.c`) decides. With it on, the guest's
frame no longer pays for GL, so its wall time says nothing about whether the
resolution fits; a second controller (`dynrt_*`, same file) takes over when
the render thread starts and is fed what the render thread measured. The one
target is every guest frame at the guest's rate (60 on NTSC); resolution is
the only lever.

**Cost.** Per replayed guest frame, between its first record and an
`RTH_FRAME` marker the frame boundary records: the render thread's CPU time
(wall time minus its idle waits for records and its time in the swap, the
display's vsync) and the GPU time of the same span (a `GL_TIME_ELAPSED`
query, read back when available, never waited on; `GL_TIMESTAMP` counters
read 0 on macOS). cost = max(CPU, GPU), published as running totals
(`gl_renderer_render_thread_costs`). A frame during which the context went
to the emulation thread is dropped (its span holds the emulation thread's
drawing). While this measures, `frame_perf`'s own TIME_ELAPSED brackets
stand aside (queries of one target cannot nest). `PSX_DYNRES_GPU_TIMER=0`
leaves CPU time only.

**Decisions** (0.25 s windows of guest time; `dynamic_resolution.h` has the
full rules and `dynrt_default_params` the numbers):

- load = mean cost / the guest's nominal interval (one display refresh at
  60 Hz). Budget 0.85 (15 % margin).
- Down, one level, after two consecutive windows over budget, or one window
  in which the emulation thread spent 5 % or more of the time blocked on the
  queue bound (backpressure: the render thread is behind) or the cost was
  over the whole interval. Never below the minimum.
- Guest-bound: when the guest ran below its rate, never waited on the queue
  and the render thread had slack in the interval it was given, the guest is
  the limit and no down step is taken.
- Judged: the window after a step settles (the cost lags the queue and the
  query readback), the next one judges it. A down step that removed less than
  30 % of the predicted cost (and did not end the backpressure) is a strike;
  a second in a row means the cost is not the pixels: both steps are undone
  and down steps are blocked for 20 s, doubling to 320 s.
- Fast first descent: armed when the controller starts (session start,
  render-thread start, a new resolution) and on a savestate load, game entry
  or window resize, for 6 s from the first judged window. While armed, a
  window at or over the whole interval, or two over budget, jumps straight
  to the highest level whose pure-area prediction (load × (S′/S)²) is at or
  below 0.78; the jump is judged like any step and, if still over, jumps
  again from the new measurement. A fixed part of the cost only makes the
  target dearer than predicted, so a jump never lands below the level that
  fits, and the level above it is predicted over the 0.70 up threshold, so no
  up step follows. A fit after a step, an up step or a failed judgement
  disarm it; single steps fine-tune. R4 2P VS from a 10x ceiling (busy
  host): 10→5 or 10→6, then 4-3; guest at 59 Hz or more from 1.2-2.3 s
  after the load (one level per judged window: 3.3 s).
- Up, one level, after 3 s of windows with no backpressure and the next level
  predicted at 0.70 or less, 2 s after the last step, never above the
  ceiling. The prediction (load × ((1−f) + f·(S′/S)²)) learns f from each
  judged step. A level reached by an up step and left within 10 s is blocked
  for up steps, 10 s doubling to 160 s.
- Holds as before (FMV, fast-forward, overlay compiles, resim, resizes,
  boot); the savestate-load and game-entry tails, which only covered the
  wall-time model's settling, are cut to 0.5 s.

**Steps** are recorded (`RTH_DYN_STEP`) after the frame just closed, with the
displayed rect captured at record time, and applied by the render thread in
stream order (`dyn_apply`, the same reseed as without the thread). The
emulation side's level (`gr_scale`, the wide-surface mirror,
`gl_renderer_dynres_stats`) moves at record time. A frame the emulation
thread holds steps directly, as without the thread.

**Telemetry.** `{"cmd":"dynres"}` reports `"mode":"render_thread"`, level,
load, budget, `bp_share`, `guest_hz`, `cpu_ms`/`gpu_ms` (last window's
means), `guest_bound`, the step counters and `up_blocked`.
`PSX_DYNRES_TRACE=<csv>` writes one line per window
(`t_s,level,load,bp_share,guest_hz,cpu_ms,gpu_ms,guest_bound,decision`).

## Out of scope

- Vulkan (the same split would apply; its backend has its own readbacks).
- Converting the remaining sync points to recorded commands (texture banks,
  render passes, CPU/hold presents). Each is correct as a sync point.
- Making replay cheaper (batching across records, fewer GL state changes).
- Diagnostic rings written by both threads (`latency_ring`, `gpu_timeline`)
  can interleave entries; they are diagnostics only.

## Tests

- `render_thread_test` (`runtime/tests/test_render_thread.c`): the core
  against a fake executor and context token — ordering and payload integrity
  over 400 frames of random-size records through ring wraps, oversize
  refusal, the in-flight bound under a slow consumer (and that the one- and
  two-frames-behind cases both occur), 300 randomized acquire/release rounds
  with the context never current on two threads, stop while held. Clean
  under ThreadSanitizer.
- `gl_render_thread_test` (`runtime/tests/run_gl_render_thread.py`,
  `test_gl_render_thread.c`, label `gpu;opengl;hardware`): a real hidden GL
  context, a scripted stream through the facade, render thread off vs on at
  1x and 4x; the values the guest read back, native VRAM, the frame at S and
  the native-wide surface must be identical. The stream has mask set/check,
  a mask-checked upload (per-pixel readback sync point), back-to-back
  uploads of one rect with a catch-up between gpu.c's writes and its commit,
  more than the ring's size recorded, readback frames, presents with a slow
  render-thread present, and widescreen tags that change per primitive and
  are poisoned after each call. It fails for each of these deliberate
  mistakes: replay reading the tags live, staging uploads from the guest
  array, dropping the stream state.
- `video_enhancement_settings_test`: the keys (`render_thread`,
  `present_thread`, `frame_generation`) default off and parse, and the
  settings.toml copies parse and round-trip through `save_user_settings`.
- `present_thread_test` (`test_present_thread.c`): the present core against
  a fake presenter: presentation order is submission order over 600 frames
  with 2-4 slots, a slot is never composed while queued or on screen, each
  done token reaches the composer that next takes its slot, a slow display
  blocks the composer, stop presents the queue and disposes leftover tokens,
  a context failure fails the start. Clean under ThreadSanitizer.
- `gl_frame_gen_test` present-thread runs: generation off and on, VRAM and
  native-wide, flip and late timing: the window shows every composed image,
  in order, bit for bit; real frames match the direct-swap run (see Pixels).
- Windows (MinGW): the runtime builds with the Win32 path of
  `render_thread.c` (SRW lock, condition variable, `CreateThread`), and
  `render_thread_test` and `gl_render_thread_test` run there (the GL test
  exits 77, a CTest skip, on a host with no GL 3.3 context).
- `dynamic_resolution_rt_test` (`test_dynamic_resolution_rt.c`): the
  render-thread controller against a synthetic two-stage pipeline (guest E,
  render a + b·S², two frames in flight, costs four frames late): overrun ->
  one level at a time to the highest level that fits, then no oscillation;
  headroom -> up only after the window; the hysteresis band; guest-bound ->
  no step; a cost that does not scale -> two strikes, undo, back-off; queue
  full with an under-reading meter -> down; floor and ceiling; a heavy
  stretch whose first window is mixed -> one strike, not an undo; holds;
  thin windows; pins; fast descent (10x→5x in one jump in 0.68 s vs 3.88 s
  one level at a time, no overshoot, no step in the next 120 s; re-armed by
  `dynrt_arm_descent`, single steps when not armed; light windows first do
  not use it up; a flat cost still ends in the undo; the target function).
- `gl_render_thread_test` dynres mode: the same level steps between frames
  with the thread off (applied at once) and on (recorded, and directly in
  held frames) give identical readbacks, VRAM, frame and wide surface.
- In game (R4, see the PR): `tools/fp_identity.py` guest identity render
  thread off vs on under `--headless-opengl`, and A/B frame rates from a race
  savestate at Native, 4K and Match display, 4:3 and widescreen.

## Debug server

`{"cmd":"render_thread"}` — active, held, max_frames, records, bytes,
frames produced/consumed, presents and `presents_stale` (skipped), acquires
(sync points) with the 16 most recent reasons and wait times,
backpressure / ring-full waits and time, render busy/idle time, ring high
water. Reading it is not a sync point.
