#pragma once
/* gpu_fmv_hd -- high-definition replacement of FMV movies (24-bit MDEC frames).
 *
 * A pack (<exe>/fmv_hd, or PSX_FMV_HD_DIR) holds one folder per movie:
 *   index.bin   FMVHD003 header, then one sig_w x sig_h luminance signature
 *               per source frame (u8, mean of each cell), then one LE int32
 *               starting sector LBA per source frame (strictly increasing);
 *   frames/NNNNNN.jpg  HD frames, `sub` per source frame (interpolation), numbered
 *               source_index * sub + k.
 * Every presented 24-bit frame's signature is matched against the pack (a local search
 * around the last recognised frame, then a global one); the matching HD frame, decoded
 * ahead by worker threads, is presented instead. The guest CD SetLoc range
 * selects the movie and delivered-sector intervals select its source frame
 * before matching signatures. Subframes advance only with captured
 * guest cycles. Unknown images and failed decodes show the native scanout. Audio, timing
 * and control (including movie skips) stay the game's own. No pack: no effect.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FmvHdIndexHeader {
    char     magic[8];      /* "FMVHD002" */
    uint32_t frames;        /* source frames */
    uint32_t sig_w, sig_h;  /* signature size */
    uint32_t sub;           /* HD frames per source frame (1 = no interpolation) */
    uint32_t fps_num, fps_den;  /* source frame rate */
    uint32_t src_w, src_h;      /* source frame size */
    uint32_t disp_w, disp_h;    /* native PSX display the movie is shown in (e.g. 320x240) */
    int32_t  off_x, off_y;      /* movie position in that display; -1 = centred */
    /* FMVHD003 only: inclusive SetLoc range of the movie on the mounted disc.
     * FMVHD002 ends before these fields and remains opt-in legacy. */
    int32_t  stream_lba_lo, stream_lba_hi;
} FmvHdIndexHeader;

typedef struct FmvHdImage {
    const uint8_t *rgba;    /* 8-bit RGBA, valid until the next call */
    int w, h;
    uint64_t serial;        /* changes with the image: otherwise no need to upload it again */
} FmvHdImage;

/* Captured with the native scanout on the guest thread, never sampled from the
 * presenter. epoch changes whenever a saved/rewound/rollback timeline is
 * restored, even when the restored cycle happens to equal the previous one. */
typedef struct FmvHdTimeline {
    uint64_t cycle;
    uint64_t frame;
    uint64_t epoch;
    uint64_t stream_generation; /* CD SetLoc/seek generation */
    int32_t stream_lba;         /* SetLoc target naming the current stream */
    int32_t delivered_lba;      /* last sector delivered to guest */
    uint32_t decode_serial;     /* guest MDEC colour-decode ordinal */
} FmvHdTimeline;

/* A presented 24-bit frame (32-bit BGRA, src_w x src_h, content_w useful columns or 0).
 * Returns 1 and fills *out when an HD frame replaces it. Called by the presenter. */
int gpu_fmv_hd_lookup(const uint32_t *bgra, int src_w, int src_h, int content_w,
                      FmvHdTimeline timeline, FmvHdImage *out);
/* Native/non-movie scanout ends the current movie; a later match starts at
 * subframe zero even if it resembles the previous movie's first frame. */
void gpu_fmv_hd_reset_tracking(void);
/* Presenter owner, after its render worker has drained and before SDL teardown.
 * Joins decoders and releases pack/held-image state; the next lookup can reload. */
void gpu_fmv_hd_shutdown(void);

/* Presenter-owner statistics: frames in HD, signatures, tracking losses. */
void gpu_fmv_hd_stats(uint64_t *hd_frames, uint64_t *queries, uint64_t *misses);

/* Presenter-owner state: JSON fields without braces. Cross-thread callers
 * use gl_renderer_fmv_hd_debug_json to synchronize with rendering. */
int gpu_fmv_hd_debug_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
