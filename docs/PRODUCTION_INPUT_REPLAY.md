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
- `PSX_INPUT_REPLAY_REPORT=/absolute/path/frames.csv`: export frame stamps and
  cumulative guest, pacer and render-queue times at shutdown. Enable existing
  `PSX_RUNTIME_PERF_DIAG=1` for guest/pacer counters. Times use microseconds;
  differences between adjacent rows describe the intervening guest interval.
- `PSX_INPUT_REPLAY_SHOTS=/absolute/path/prefix`: request screenshots at route
  offsets 800 and STOP+2, outside the usual measured interval 900..2700.

The caller chooses new output paths and provides a process timeout. Reports
measure guest VBlanks, not distinct rendered images or display scanout.
