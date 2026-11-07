# Frame tracing (stereo / OpenXR)

`runtime/src/frame_trace.c` records one row per traced stereo frame so a visible
VR hitch can be attributed to a stage instead of guessed at. It is the
measurement floor for the VR optimization work: nothing else should be tuned
before there are traces.

## Enable

    PSX_FRAME_TRACE=1          # off by default; one branch when off
    PSX_FRAME_TRACE_CAP=2048   # optional power-of-two ring size (default 2048)

The ring is static, the record holds fixed-size fields, and the record path
allocates nothing and writes no files (`runtime/tests/test_frame_trace_guard.py`
fails the build if that changes). Instrumentation off vs on is an A/B control.

## Read

Over the debug TCP server:

    {"cmd":"frame_trace","format":"json","max":240}
    {"cmd":"frame_trace","format":"chrome","max":240}   # Perfetto trace-event JSON

`max` bounds the newest records (0 = all retained). The game-side helper is
`vr/trace_frames.py`, which writes `frame_trace.json`, `frame_trace.csv`,
optional `frame_trace_perfetto.json`, and a `summary.json` with P50/P95/P99 of
`pair_total`, each eye's total, and `xr_wait_frame`, plus stale/shed/failed
counts and the worst frames.

## Stages (microseconds)

    guest_sim                       host time between traced frames
    pair_total                      both eyes, pair open -> publish
    eye_{begin,guest,end,restore}_{l,r}
                                    one eye's VRAM backup+checkpoint, guest draw,
                                    flush/capture/VRAM restore, machine restore
    xr_wait_frame / xr_begin_frame  xrWaitFrame / xrBeginFrame
    xr_acquire / xr_wait_image / xr_copy / xr_release / xr_end_frame
    hud_capture / hud_draw / hud_readback
                                    a HUD capture transaction, its guest draw and
                                    its readback
    game_env / game_world / game_text / game_traverse / game_icon /
    game_submit / game_laser
                                    the guest scene rebuild one eye replays, split
                                    by the call the game hook makes: draw env and
                                    ordering-table setup, world passes, text,
                                    scene traversal, HUD icon, DrawOTag, and the
                                    hook's own per-eye raycast. PAIR totals (both
                                    eyes write the same slots), so compare them
                                    against eye_guest_l + eye_guest_r. Written by
                                    the hook through
                                    psx_mod_frame_trace_stage_begin/end, which
                                    only accepts them inside a traced stereo eye.
    snapshot_extract / snapshot_publish
                                    the existing transactional pair's baseline
                                    checkpoint and pair publish. Not the future
                                    four-slot scene mailbox.
    xr_locate / host_prep
                                    xrLocateViews and the host view math + hand
                                    location after it. CPU spans, not GPU time.

Per frame: `seq`, `open_ms`, guest frame/cycle, stereo attempt id, published
pair id, `pair_age_ms` (age of the retained whole pair), `source` (1 pair / 2
native), `eye_failed`, shed skipped/streak, `prims`/`tex_prims` (the last eye's
own count), `prims_l`/`prims_r`, `tex_prims_l`/`tex_prims_r`,
`cycles_l`/`cycles_r` (guest cycles each eye's replay executed, separating
emulated machine work from host submission time), `vram_leaks`, `tex_bytes`,
`gpu_scene_us`/`gpu_present_us`/`gpu_sample_age_frames` (asynchronous
`GL_TIME_ELAPSED` results retrieved later; never added to CPU stages), and
flags (`published`, `stale`, `shed`, `failed`, `xr_empty`, `scene`, `io`,
`no_telemetry`).

The GPU timer ring is enabled for diagnostics builds and for
`PSX_FRAME_TRACE=1`; `PSX_GL_PERF=0` disables it. Results may describe a frame
several presents old, so `gpu_sample_age_frames` is part of the reading.

## Guarantees

* `xr_*` stages are CPU wall-clock waits, never GPU execution time.
* Only a complete pair is published; a stale frame retains the previous whole
  pair (`pair_age_ms` says how old it is).
* A frame is `stale` when it published nothing, `shed` when the pair was skipped
  on purpose, `failed` when a pair was attempted and lost, and
  `no_telemetry` when an expected-to-be-instrumented frame recorded no stage.
* Timestamps are monotonic even if the host clock steps backwards.
