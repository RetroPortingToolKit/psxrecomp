#pragma once
/* gpu_fmv_hd -- high-definition replacement of FMV movies (24-bit MDEC frames).
 *
 * A pack (<exe>/fmv_hd, or PSX_FMV_HD_DIR) holds one folder per movie:
 *   index.bin   a FmvHdIndexHeader, then for every source frame a sig_w x sig_h
 *               luminance signature (u8, the mean of each cell of the original frame);
 *   frames/NNNNNN.jpg  HD frames, `sub` per source frame (interpolation), numbered
 *               source_index * sub + k.
 * Every presented 24-bit frame's signature is matched against the pack (a local search
 * around the last recognised frame, then a global one); the matching HD frame, decoded
 * ahead by worker threads, is presented instead. Audio, timing and control (skipping the
 * movie) stay the game's own. No pack: no effect.
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
} FmvHdIndexHeader;

typedef struct FmvHdImage {
    const uint8_t *rgba;    /* 8-bit RGBA, valid until the next call */
    int w, h;
    uint64_t serial;        /* changes with the image: otherwise no need to upload it again */
} FmvHdImage;

/* A presented 24-bit frame (32-bit BGRA, src_w x src_h, content_w useful columns or 0).
 * Returns 1 and fills *out when an HD frame replaces it. Called by the presenter. */
int gpu_fmv_hd_lookup(const uint32_t *bgra, int src_w, int src_h, int content_w, FmvHdImage *out);

/* Statistics (debug): frames presented in HD, signatures computed, tracking losses. */
void gpu_fmv_hd_stats(uint64_t *hd_frames, uint64_t *queries, uint64_t *misses);

/* State for the debug server (fmv_hd command): JSON fields without braces. */
int gpu_fmv_hd_debug_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
