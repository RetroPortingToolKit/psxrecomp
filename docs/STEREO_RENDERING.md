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
period. Expensive work is shed as a complete pair; every thirtieth request retries
to recover a stale estimate. This is a conservative cadence budget, not a measured
headset idle budget. Allocating or slow initial pairs may exceed it; completed
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
precedence when a compatible complete pair is available.

Inside an eye callback, `psx_mod_render_view_offset(x,y,z)` replaces a scoped
camera-space translation. RTPS and RTPT add it alongside TR before projection;
the guest translation registers remain unchanged. Repeated transformations do
not accumulate the offset. Normal returns and watchdog rollback restore the
previous host value. Calls outside a render transaction are refused. The GTE
test compares this seam against explicit pre-divide translation and measures
12 pixels at Z=800 versus 3 pixels at Z=3200 for X=24 and H=400.

TCP inspection:

- `stereo_stats`: pair outcomes, eye entry cycles/state hashes under VERIFY,
  sampled view offsets, latched failed attempt/eye/retained pair, and published
  pair metadata. `render_pass_stats` continues to
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
