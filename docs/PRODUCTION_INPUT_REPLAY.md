# Production input replay

`PSX_INPUT_REPLAY=/absolute/path/route.txt` supplies recorded active-low digital
pad words in every build, including `PSX_DEBUG_TOOLS=OFF`. Each non-comment
line is `frame_count hexadecimal_buttons`, for example `60 bfff`. Counts must
be positive; pad words must fit 16 bits. The route starts at the first VBlank
after a successful disk savestate load, advances by guest frames, and returns
neutral input after its end. A failed requested state load is fatal. Netplay
and file replay cannot run together.

Use the exact state paired with the recording. This is a host input source;
it does not bypass savestate compatibility checks or change guest timing.

Optional bounded benchmark controls:

- `PSX_INPUT_REPLAY_STOP=2700`: request normal shutdown ten frames after this
  zero-based route offset.
- `PSX_INPUT_REPLAY_SECONDS=60`: instead of STOP, run a wall-time window of
  1..60 seconds beginning at route offset 900. Stop at the first guest frame
  after its deadline, then request shutdown ten frames later.
- `PSX_INPUT_REPLAY_REPORT=/absolute/path/frames.csv`: export frame stamps and
  cumulative guest, pacer and render-queue times at shutdown. Enable existing
  `PSX_RUNTIME_PERF_DIAG=1` for guest/pacer counters. Times use microseconds;
  differences between adjacent rows describe the intervening guest interval.
  A companion `.spans.csv` exports the bounded host attribution ring.
  `.render.csv` exports GL work counts. With `PSX_GL_CPU_PERF=1` it also
  reports inclusive CPU wall spans (calls, total and maximum) for depth
  restoration, queue replay, VRAM copies, interpolation captures/intervals and
  centre splices. These spans overlap; do not sum them as exclusive costs.
  Collection is off by default and its queue drain/export happens after the
  measured route window. Use separate profiling and acceptance runs.
  `PSX_GL_CPU_PERF=2` additionally finishes queued GPU work before and after
  queue replay, VRAM copies, captures and centre splices. These spans include
  GPU completion; `_prior_drain` rows measure earlier queued work separately.
  This serializes the pipeline and is attribution only. Totals cover the
  whole replay, including warmup. `.rth.json` records queue waits and recent
  context-acquisition reasons before the report's own drain.
  `PSX_GL_CPU_PERF=3` instead samples one in 128 high-resolution textured
  depth batches: `colour_gpu_sample` and `depth_gpu_sample` finish GPU work
  around the same batch's colour and depth draws. Sampling perturbs cadence;
  these completion spans are not acceptance frame times.
  `PSX_GL_PERF=1` additionally enables GPU queries in production and exports
  rolling GPU aggregates to `.gpu.csv`; use this for separate attribution
  trials, since queries can perturb the measured workload.
- `PSX_INPUT_REPLAY_SHOTS=/absolute/path/prefix`: request screenshots at route
  offsets 800 and STOP+2, outside the usual measured interval 900..2700.

The caller chooses new output paths and provides a process timeout. Reports
measure guest VBlanks, not distinct rendered images or display scanout.
