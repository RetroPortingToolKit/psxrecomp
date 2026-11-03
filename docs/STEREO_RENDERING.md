# Simultaneous stereo rendering

The paired API is an opt-in enhancement alongside temporal render passes. It
uses the same frozen-time checkpoint, store policy, watchdog, VRAM journal and
restore. It does not require interpolation, a FLIP source, a temporal plan or
Q16 phase labels. Each eye callback receives LEFT=0 or RIGHT=1 explicitly.

Call `psx_mod_render_stereo(cpu, frame, callback, user)` from a game's measured
draw boundary outside interrupts or active GPU DMA. The callback must rebuild
and submit geometry, without waiting for VBlank or advancing game logic. The
rect declares the capture region. Restore completes after each eye before the
next one starts. Successful eye textures remain private until both transactions
and capture/restore verification succeed. Failure retains the preceding complete
pair. Host callback state is the plugin's responsibility, including selectors
that must be cleared after watchdog rollback.

`period_vblanks` describes the game's draw cadence (1..8). An EMA of complete-pair
host cost is compared with 80% of that cadence, using the runtime's live NTSC/PAL
period, unless `PSX_STEREO_SHED_BUDGET_MS` supplies the real headset deadline (the
VBlank cadence is a guest-budget fallback, not an OpenXR display deadline).
Expensive work is shed as a complete pair, never as a single eye, and the previous
complete pair is retained. Shedding is bounded both by wall clock
(`PSX_STEREO_SHED_PROBE_MS`, default 250 ms) and by a consecutive count
(`PSX_STEREO_SHED_MAX_STREAK`, default 8), so a stale estimate cannot suppress
indefinitely: whichever bound trips first forces a probe, and the probe's
measurement replaces the estimate rather than blending into it, so a single spike
ends after one bounded streak. Recovery uses a hysteresis band
(`PSX_STEREO_SHED_HYSTERESIS`, default 0.9) and scene/mode transitions reset the
estimate. `PSX_STEREO_SHED=0` disables shedding entirely for a baseline
diagnostic. Allocating or slow initial pairs may exceed the budget; completed
guest work cannot be preempted by a host-time budget. Guest-cycle watchdogs remain
the runaway bound.

OpenGL GPU-authoritative 15-bit rendering is required. CPU-authoritative dual
raster, windowed high-resolution tiles, depth24 and native-wide rendering are
initially unsupported. Session/resimulation/rewind/fast-forward and nesting gates
remain. Faithful defaults do not call this API or enable stereo presentation.

`psx_mod_set_stereo_presentation(1)` opts into side-by-side output: one atomic pair
per swap, LEFT on the left, RIGHT on the right, with each eye using the configured
display aspect. Zero disables it. The first implementation uses ordinary game
VBlank presentation cadence; it does not submit to OpenXR or create headset poses.
Stereo and temporal interpolation should be configured separately; SBS takes
precedence when a compatible complete pair is available. A pair is shown only while
the plugin keeps submitting: it expires 24 VBlanks of guest time after its capture
cycle (3x the slowest 8-VBlank cadence) and the presenter falls back to the normal
flat image (menus, same-size FMV, a plugin that stopped submitting). Savestate/rollback loads and
session resets drop the pair immediately.

Inside an eye callback, `psx_mod_render_view_offset(x,y,z)` replaces a scoped
camera-space translation. RTPS and RTPT add it alongside TR before projection;
the guest translation registers remain unchanged. Repeated transformations do
not accumulate the offset. Normal returns and watchdog rollback restore the
previous host value. Calls outside a render transaction are refused. The GTE
test compares this seam against explicit pre-divide translation and measures
12 pixels at Z=800 versus 3 pixels at Z=3200 for X=24 and H=400.

`psx_mod_render_view` (full pose) is an explicit per-eye projection. With
`projection=1` it REPLACES the guest X/Y projection entirely: guest `H`, the
widescreen/dome squash and the `[video] fov_scale` scaled-H do not feed it, and the
guest-formula PGXP shadows are skipped for those vertices. The only coupling is
`projection_h_ref`: when non-zero the focal lengths are multiplied by (fov-scaled
guest `H`) / `projection_h_ref`, so a plugin can follow per-draw H changes. SZ, the
depth cue and divide flags still come from the guest division. The pose exists only
inside a render transaction and is restored with it, so nothing outside the sandbox
sees it. `test_render_pose` covers the override together with `fov_scale`.

TCP inspection:

- `stereo_stats`: pair outcomes, eye entry cycles/state hashes under VERIFY,
  sampled view offsets, latched failed attempt/eye/retained pair, published
  pair metadata, and the shedding policy (`shedding`, `shed_streak`,
  `shed_streak_ms`, `shed_streak_max`, `stale_frames`, `budget_resets`,
  `last_pair_age_ms`). `render_pass_stats` continues to
  expose shared transaction checks, aborts, leaks and dropped stores; its eye
  transactions also increment pass counters.
- `stereo_dump path=<absolute-directory> count=N`: arm dumps of the next N
  complete published pairs, with left/right PNGs, a side-by-side composite
  sharing one pair ID. Pair/cycle/view metadata is inspected through TCP
  `stereo_stats.captures`, retained for the current capture request (maximum
  100 records); the runtime writes no diagnostic JSON files. A capture client
  can use `tools/stereo_capture.py::write_capture_manifests` to write manifests
after confirming all three PNGs for each returned ID.
  Arming another capture or resetting the session clears the metadata. It captures
  no scene on its own. Failed staging eyes never appear as published pairs.

Validation, source revisions and live limitations are recorded in
[the feature provenance record](internal/upstream/atomic-stereo-render-views.md).
The game's [VR execution plan](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/blob/master/docs/reverse/VR_EXECUTION_PLAN.md)
records historical alpha evidence. Projection scale and eye separation remain
game-specific and must be calibrated independently. This API also works without
a headset backend; it does not enable a headset mode by default.

The optional headset backend is described in [OPENXR_RENDERING.md](OPENXR_RENDERING.md).


## Production startup and performance receipts

`PSX_OPENXR_READY_FILE=<fresh-path>` requests one JSON startup receipt, including
in builds with `PSX_DEBUG_TOOLS=OFF`. Main publishes through a same-directory
temporary and atomic replacement, so partial JSON is never visible. An existing
destination is refused at startup; the caller owns a fresh path and removes it
afterward. Schema 2 records XR session generations, counters and the savestate
receipt. When `PSX_LOAD_SLOT` is set, success requires that load to settle
successfully and a later submission from the current XR session. A failed load
publishes `ready:false` with `state_load_failed`; session re-init publishes a
new receipt after its first current-session submission. An invalid, refused, or
netplay-disabled load request also publishes `ready:false` with a specific error
instead of being treated as if no load were requested. A game-session reboot
starts a fresh load lifecycle, so an earlier success cannot authorize the new
session; the newly staged `PSX_LOAD_SLOT` must settle before readiness can be
published. Without the variable the reporter immediately becomes a no-op.

`PSX_RUNTIME_PERF_DIAG=1` also prints VR pair/shed/empty/failure totals, the pair
EMA/last milliseconds and rollback verification totals at its existing configured
interval (`PSX_RUNTIME_PERF_DIAG_MS`). This exposes production behavior without
per-instruction debug tracing. The pair average is an EMA, not a percentile.
`PSX_FRAME_REPORT_MS` remains the production guest-frame cadence measurement.
