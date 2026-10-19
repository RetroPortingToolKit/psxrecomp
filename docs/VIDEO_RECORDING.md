# Video recording

The runtime can record exactly what the player sees and hears to an MP4:
every composed present (after the aspect fit, so widescreen, internal
resolution, bezel and OSD are all in it — the same image `present_shot`
captures) plus the SPU's final mix. Use it for promo footage and for
reviewing enhancements without capturing the desktop.

```sh
# From boot, off the desktop (no visible window, no focus steal, no taskbar entry):
SDL_AUDIODRIVER=dummy ./Game.exe --offscreen-window --no-launcher \
    --bios bios/openbios.bin --disc "disc/Game.cue" --debug-port 4590 \
    --record-video F:/captures/clip.mp4

# Or start/stop over TCP (one request per connection):
{"cmd":"video_record","op":"start","path":"F:/captures/clip.mp4"}
{"cmd":"video_record","op":"status"}
{"cmd":"video_record","op":"stop"}          # then poll status until done
```

ffmpeg must be on `PATH`, in `$PSX_FFMPEG`, or passed as `ffmpeg`. If it is
missing, `start` fails with a clear error. ffmpeg runs as a child process with
no console window (Windows: `CREATE_NO_WINDOW`; POSIX: `posix_spawnp`; other
hosts: `popen`). Its messages go to `<path>.ffmpeg.log`.

## Options (`video_record` `op:start`)

| Key | Default | Meaning |
|---|---|---|
| `path` | required | Output `.mp4`. Use forward slashes in JSON |
| `fps` | `0` | `0` = the measured present rate (below). A number forces that output rate |
| `crf` | `18` | Quality: libx264/libx265 `-crf`, `*_nvenc` `-cq` |
| `encoder` | `libx264` | Any ffmpeg video encoder name, e.g. `h264_nvenc` |
| `preset` | `veryfast` for libx264/x265 | Encoder preset |
| `encoder_args` | — | Extra whitespace-separated encoder arguments |
| `width`, `height` | first frame | Fixed output size (rounded up to even) |
| `audio` | `1` | `0` records video only |
| `audio_bitrate_k` | `192` | AAC bit rate |
| `queue_frames` | `8` | Frames buffered before the game waits for the encoder |
| `keep_intermediates` | `0` | Keep `<path>.part.video.nut` / `.part.audio.wav` after the mux |

## What is recorded

**Video.** The backend that owns the present reads back every swap:

- OpenGL: in `gl_swap_now`, the one swap all GL presents go through, so the
  order is display order — interpolated in-between presents and
  frame-generation frames included. Each present's `glReadPixels` goes into a
  pixel-pack buffer (ring of 3); the previous present's buffer is mapped one
  present later, so the GPU is never stalled. Present-thread slots are read
  the same way (the redirected framebuffer 0).
- SDL software renderer: `SDL_RenderReadPixels` before `SDL_RenderPresent`
  (synchronous; the SDL_Renderer API has no async readback).
- Vulkan: no readback hook — `start` is refused with an explicit error.
- `--headless`: no present surface — refused; use `--offscreen-window`.

**Audio.** `AUDIO_TAP_SPU_OUT` (audio_trace.c). That tap is the SPU's final
mix: the 24 voices, reverb and the CD input bus, which carries both CD-DA
(Red Book tracks) and decoded XA (`cdrom.c` -> `spu_cd_audio_push`). It is
taken before host-only gain (the player's master volume, turbo-mute fades).
The SPU is clocked by the guest (768 guest cycles per 44.1 kHz frame,
`spu_catch_up`), so the tap advances the same with `SDL_AUDIODRIVER=dummy` or
no audio device at all.

## Timing and A/V sync

Nothing uses the wall clock.

- Every present is stamped with the guest cycle counter (interpolated
  in-between presents are pulled back by `(1 - alpha)` of a source frame).
- Every captured present becomes exactly one frame: `frames_written ==
  presents`, `dropped == 0`. When the encoder cannot keep up, the presenting
  thread waits for a free buffer (`backpressure_waits`, `backpressure_ms`):
  the game slows down, the recording does not lose frames.
- Output rate = the measured present rate on the guest clock,
  `(frames - 1) * 33868800 / (cycle_last - cycle_first)`, snapped to `N/1` or
  `N*1000/1001` when within 0.002 fps (else millihertz). One present per guest
  vblank measures exactly `60/1` NTSC (`g_psx_vblank_cycles` = 564480) or
  `50/1` PAL; GL frame interpolation measures its present rate. The live
  encode uses the nominal rate (vblank rate x interpolation ratio) and the mux
  retimes only if the measurement differs.
- The audio slice is cut sample-exactly from the same guest interval: the SPU
  publishes which guest cycle its tap head covers (`audio_trace_note_spu_clock`
  from `spu_catch_up`), so the first audio sample is the one rendered at the
  first present's guest cycle, and the track length is the video duration.
- `max_av_skew_ms` reports the worst gap between a frame's slot on the
  constant-rate timeline and its guest-time stamp — how irregular the present
  cadence was (FMV present skipping, presents while the guest is paused,
  wall-clock coalescing of interpolated presents). Regular play reports ~0.

## Output size

Fixed per recording: the first frame's size (rounded up to even, as yuv420p
requires) unless `width`/`height` are given. A later frame of another size
(window resize, fullscreen toggle) is scaled to fit with its aspect preserved
(nearest neighbour) and letterboxed/pillarboxed in black; `resized_frames`
counts them. Set the window size before starting (`window_size` command, or
`[video]` window settings) to record at a chosen resolution.

## Pipeline

1. ffmpeg #1 reads raw BGRA frames on stdin and encodes H.264 yuv420p
   (BT.709) to `<path>.part.video.nut`; the writer thread appends the SPU tap
   to `<path>.part.audio.wav`.
2. `stop` -> `stopping` (the GL backend hands over its in-flight readback at
   the next present; with no present for 3 s it is reported as `unflushed`)
   -> `finalizing` (encoder drained, audio trimmed/padded to the video
   duration, ffmpeg #2 muxes video by stream copy + AAC audio into `<path>`
   with `+faststart`) -> `done`.
3. Process exit while recording stops and muxes the file (`atexit`, after the
   GL context hands over its in-flight frame).

## `--offscreen-window`

Creates the real SDL window and presenter but never shows it: no desktop
window, no focus change, no taskbar entry, and no first-run wizard or modal
(the same unattended rules as `--hidden-window`, which it extends). All
windowed features run (OpenGL renderer, internal resolution, widescreen,
interpolation, OSD), so `present_shot` and `video_record` work. `--headless`
is unchanged: it has no presenter at all.

Measured on Windows 11 + NVIDIA (see the PR): GL readback of the default
framebuffer of the never-shown window returns the real composed frames.
