/*
 * video_recorder.h - record the COMPOSED present surface plus the final audio
 * mix to an H.264/AAC MP4 through an ffmpeg child process.
 *
 * What is recorded
 *   Video: every presented frame, read back by the backend that owns the
 *   present (GL: async PBO ring in gpu_gl_renderer.c; SDL software renderer:
 *   SDL_RenderReadPixels in main.cpp) AFTER the aspect fit and host OSD — the
 *   same image `present_shot` captures. Vulkan has no readback hook and is
 *   refused at start; --headless has no present surface and is refused too.
 *   Audio: the AUDIO_TAP_SPU_OUT ring (audio_trace.c). That tap is the SPU's
 *   final mix — 24 voices + reverb + the CD input bus, which carries both
 *   CD-DA (Red Book tracks) and decoded XA (cdrom.c -> spu_cd_audio_push) —
 *   before host-only gain (player master volume, turbo mute fades). It is
 *   rendered on guest time (768 guest cycles per 44.1 kHz frame), so it
 *   advances identically with SDL_AUDIODRIVER=dummy or no audio device.
 *
 * Timing (A/V sync without wall clock)
 *   Every present is stamped with the guest cycle counter. Frames are never
 *   dropped or duplicated: frames written == presents captured. The output
 *   frame rate is the MEASURED present rate on the guest clock:
 *     fps = (frames - 1) * 33868800 / (cycle[last] - cycle[first])
 *   snapped to an exact rational when it is within 0.002 fps of N/1 or
 *   N*1000/1001 (one present per vblank measures exactly 60/1 NTSC or 50/1
 *   PAL; GL frame interpolation measures its target rate). The audio slice is
 *   cut sample-exactly from the same guest interval: its first sample is the
 *   one the SPU rendered at the first present's guest cycle, its length is
 *   the video duration * 44100. `fps` in the options forces a fixed output
 *   rate instead (A/V sync then follows the frame count).
 *   Status reports max_av_skew_ms: the worst gap between a frame's position
 *   on the constant-rate output timeline and its guest-time stamp, i.e. how
 *   irregular the present cadence was (FMV present skipping, pauses that keep
 *   presenting, wall-clock coalescing of interpolated presents).
 *
 * Output size is fixed per recording: the first frame's size (rounded up to
 *   even, as yuv420p needs) unless width/height are given. A later frame of
 *   another size (window resize, fullscreen toggle) is scaled to fit with its
 *   aspect preserved (nearest-neighbour) and letterboxed/pillarboxed in black.
 *
 * Backpressure: frames go through a bounded pool. When ffmpeg cannot keep
 *   up, the presenting thread WAITS for a free buffer (the game slows down;
 *   guest-time stamps keep A/V exact). Nothing is ever dropped by design;
 *   `dropped` counts only readback failures and is expected to stay 0.
 *
 * Pipeline: stage 1 streams raw BGRA frames to `ffmpeg -f rawvideo -i -`
 *   (no console window: CREATE_NO_WINDOW on Windows, posix_spawn elsewhere,
 *   popen as the portable fallback) encoding to <path>.part.video.nut, while
 *   the writer thread appends the SPU tap to <path>.part.audio.wav. stop()
 *   finalises on the writer thread (status: stopping -> finalizing -> done):
 *   the audio slice is trimmed/padded to the video duration and a second
 *   ffmpeg run muxes video (stream copy) + AAC audio into <path>. ffmpeg's
 *   stderr goes to <path>.ffmpeg.log.
 *
 * Threading: start/stop/status come from the debug-server / main thread; the
 *   present hooks run on whichever thread owns the present (main, GL render
 *   thread or the interpolation present loop). All shared state is guarded.
 */
#ifndef PSX_VIDEO_RECORDER_H
#define PSX_VIDEO_RECORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VREC_PATH_MAX 1024
#define PSX_GUEST_CPU_HZ 33868800.0

typedef enum {
    VREC_IDLE = 0,       /* never started */
    VREC_RECORDING,      /* capturing presents */
    VREC_STOPPING,       /* stop requested; backend flushing pending readbacks */
    VREC_FINALIZING,     /* encoder draining + audio/video mux */
    VREC_DONE,           /* last recording completed; output exists */
    VREC_ERROR           /* last recording failed; see error */
} VrecState;

typedef enum {
    VREC_BACKEND_NONE = 0,   /* --headless: no present surface */
    VREC_BACKEND_SOFTWARE,   /* SDL_Renderer present (SDL_RenderReadPixels) */
    VREC_BACKEND_OPENGL,     /* GL present (async PBO readback) */
    VREC_BACKEND_VULKAN      /* no readback hook: refused */
} VrecBackend;

typedef struct {
    char   path[VREC_PATH_MAX];     /* final .mp4 (required) */
    char   ffmpeg[VREC_PATH_MAX];   /* "" = $PSX_FFMPEG, then PATH */
    char   encoder[32];             /* ffmpeg video encoder (default libx264) */
    char   preset[32];              /* encoder preset ("" = encoder default;
                                       libx264 defaults to veryfast) */
    char   encoder_args[256];       /* extra whitespace-separated encoder args */
    int    crf;                     /* quality (libx264 -crf / nvenc -cq), 0..51 */
    double fps;                     /* 0 = measured present rate (see above) */
    int    width, height;           /* 0 = first frame's size */
    int    audio;                   /* 1 = mux the SPU_OUT mix (default) */
    int    audio_bitrate_k;         /* AAC kbit/s (default 192) */
    int    queue_frames;            /* backpressure pool depth (2..64, def 8) */
    int    keep_intermediates;      /* keep .part.* files after the mux */
} VrecOptions;

typedef struct {
    VrecState state;
    uint32_t  session;              /* increments on every successful start */
    char      path[VREC_PATH_MAX];
    char      error[512];
    int       width, height;        /* output size (0 until the first frame) */
    double    fps;                  /* final rate (measured/forced; live estimate while recording) */
    uint64_t  presents;             /* presents captured while recording */
    uint64_t  frames_written;       /* frames delivered to the encoder */
    uint64_t  dropped;              /* readback failures (expected 0) */
    uint64_t  unflushed;            /* captured but never read back (stop with no further present) */
    uint64_t  resized_frames;       /* frames fitted into the fixed output size */
    uint64_t  backpressure_waits;   /* submits that waited for a free buffer */
    double    backpressure_ms;      /* total time presenters waited */
    int       queue_depth;          /* frames waiting for the encoder now */
    uint64_t  audio_frames;         /* stereo frames written to the audio track */
    uint64_t  audio_pad_frames;     /* silence added (slice not yet rendered / evicted) */
    uint64_t  audio_trim_frames;    /* rendered beyond the video duration, cut */
    int       audio_anchor;         /* 1 = audio start located via the SPU guest clock */
    int64_t   audio_slip_frames;    /* SPU frames rendered off the guest budget inside
                                       the recording (turbo mute tail / dropped debt);
                                       0 = audio stayed sample-locked to guest time */
    double    guest_seconds;        /* guest time spanned by the captured presents */
    double    video_seconds;        /* frames / fps */
    double    max_av_skew_ms;       /* worst cadence deviation (see header) */
    double    min_present_ms, max_present_ms; /* guest ms between presents */
    uint64_t  bytes_out;            /* intermediate bytes while recording, final size when done */
    int       ffmpeg_exit;          /* stage-1 encoder exit code (-1 = n/a) */
    int       mux_exit;             /* stage-2 mux exit code (-1 = n/a) */
} VrecStatus;

/* ---- control (debug server / CLI) ------------------------------------- */
void vrec_options_default(VrecOptions *o);
/* Validate and normalise. Returns 1 ok, else 0 with a message in err. */
int  vrec_options_validate(VrecOptions *o, char *err, int errcap);
/* Start a recording. Returns 1, or 0 with err filled (already recording,
 * unsupported backend, ffmpeg missing, unwritable path, bad options). */
int  video_recorder_start(const VrecOptions *o, char *err, int errcap);
/* Request stop: returns 1 when a recording was active (finalisation then runs
 * in the background — poll status until done/error), else 0 with err. */
int  video_recorder_stop(char *err, int errcap);
/* Block until the current recording (if any) reaches done/error, waiting at
 * most timeout_ms (<0 = forever). Returns the resulting state. */
VrecState video_recorder_wait(int timeout_ms);
void video_recorder_get_status(VrecStatus *out);
const char *video_recorder_state_name(VrecState s);
/* Process exit: stop + finalise synchronously so the MP4 is complete. */
void video_recorder_shutdown(void);

/* ---- host wiring (main.cpp) -------------------------------------------- */
/* Fixed backend (tests) or a live query (main.cpp: asked on every start and
 * status, so a renderer fallback or lobby rematch is seen). The fn wins. */
void        video_recorder_set_backend(VrecBackend b);
void        video_recorder_set_backend_fn(VrecBackend (*fn)(void));
VrecBackend video_recorder_backend(void);
/* Nominal present rate (presents per guest second) used for the live encode
 * before the rate is measured: guest vblank rate, times the GL interpolation
 * ratio when that is active. NULL = 60. */
void video_recorder_set_rate_hint_fn(double (*fn)(void));
/* Locate ffmpeg: explicit path (must exist), $PSX_FFMPEG, then PATH.
 * Returns 1 and fills out, else 0. */
int  video_recorder_find_ffmpeg(const char *explicit_path, char *out, int outcap);

/* ---- present hooks (backend present thread) ---------------------------- */
enum { VREC_PRESENT_IDLE = 0, VREC_PRESENT_CAPTURE = 1, VREC_PRESENT_FLUSH = 2 };
/* Once per present, right before the swap. CAPTURE: read this present back
 * and submit it (immediately or, for async readback, later with its stamp).
 * FLUSH: stop was requested — submit every pending readback, do not capture
 * this present, then call video_recorder_flush_done(). The session out-param
 * lets an async backend discard readbacks that belong to an older session. */
int  video_recorder_present_poll(uint32_t *session);
/* BGRA bytes (B,G,R,A per pixel), stride in bytes, bottom_up for GL rows.
 * guest_cycle: the guest clock this present shows. Blocks for backpressure. */
void video_recorder_submit_frame(uint32_t session, const void *bgra, int w, int h,
                                 int stride, int bottom_up, uint64_t guest_cycle);
/* An async readback failed: the frame is lost (counted in `dropped`). */
void video_recorder_note_lost_frame(uint32_t session);
void video_recorder_flush_done(uint32_t session);
/* Non-counting probe for context teardown: 1 (and the session) while a stop
 * waits for the backend's pending readbacks. */
int  video_recorder_flush_wanted(uint32_t *session);

/* ---- pure helpers (exposed for tests) ----------------------------------- */
/* Fit a src_w x src_h image into dst_w x dst_h keeping aspect; outputs the
 * destination rect. */
void vrec_fit_rect(int src_w, int src_h, int dst_w, int dst_h,
                   int *x, int *y, int *w, int *h);
/* Turn a measured rate into an exact rational (see header). */
void vrec_rationalize_fps(double fps, int64_t *num, int64_t *den);
/* Split whitespace-separated args (no quoting); returns count (<= max). */
int  vrec_split_args(const char *s, char out[][128], int max);

#ifdef __cplusplus
}
#endif

#endif /* PSX_VIDEO_RECORDER_H */
