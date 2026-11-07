#ifndef PSX_PRESENTATION_GPU_H
#define PSX_PRESENTATION_GPU_H
#include "presentation_draw.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    PSX_PRESENTATION_TEXTURED = 1, PSX_PRESENTATION_RAW_TEXTURE = 2,
    PSX_PRESENTATION_MASK_SET = 4, PSX_PRESENTATION_MASK_CHECK = 8,
    PSX_PRESENTATION_DITHER = 16
};
typedef struct PsxPresentationMaterial {
    uint64_t texture_version; /* retained immutable native-word texture */
    uint32_t flags, texture_window;
    uint16_t texpage, clut_x, clut_y;
    int16_t semi; /* -1 opaque, otherwise PS1 mode 0..3 */
    int16_t clip[4]; /* inclusive native VRAM draw area */
    uint16_t uv_limits[4];
    uint8_t filter; /* 0 nearest, 1 bilinear, 2 stable, 3 xBR */
} PsxPresentationMaterial;

/* Development renderer integration. All entry points serialize with the GL
 * owner, including ordered async snapshots below. They return
 * failure with a non-GL backend. The game continues on its ordinary presenter.
 * Snapshot copies stay on the GPU. No guest VRAM or GTE/CPU state is written.
 * A successful snapshot owns one reference; release every acquired reference.
 * Handles cannot revive after eviction or context recreation. */
uint64_t psx_presentation_gpu_snapshot(void);
/* Transfer user to callback on success. The ordered render-thread record
 * captures resources before subsequent native drawing, then calls ready on
 * the GL owner. Bounded to four queued captures. Zero means no transfer.
 * With no render thread, executes immediately on the current GL owner. */
typedef void (*PsxPresentationSnapshotReady)(void *user,uint64_t version);
int psx_presentation_gpu_snapshot_async(PsxPresentationSnapshotReady ready,void *user);
/* Lifecycle/debug mutation only: acquire before locking producer-owned data. */
void psx_presentation_gpu_acquire(void);
int psx_presentation_gpu_retain(uint64_t version);
void psx_presentation_gpu_release(uint64_t version);
/* Draw into a private VRAM-space target at the current output resolution.
 * Resolution changes reject old output until freshly rendered. Commands keep their
 * input order. Resource indices select materials; no correspondence is inferred.
 * Failed validation leaves the previous completed output intact. */
int psx_presentation_gpu_render(const PsxPresentationTriangle *triangles,
    uint32_t count, const PsxPresentationMaterial *materials, uint32_t material_count);
uint64_t psx_presentation_gpu_output_version(void);
/* Nonblocking completion check for exactly that private output generation. */
int psx_presentation_gpu_output_ready(uint64_t output_version);
/* Optional title producer, invoked on the GL owner at the ordinary native
 * present boundary. repeated means the same native display region was shown
 * last time. Select an already prepared, GPU-ready private target; output_crop
 * is x,y,width,height in that target. Never execute guest code, read mutable
 * guest state, wait for a scene, or shift the native presentation deadline.
 * A zero result keeps the ordinary native display. Register before starting
 * the renderer or while holding its context. */
typedef int (*PsxPresentationProvider)(void *user,int display_x,int display_y,
    int width,int height,int repeated,int output_crop[4]);
void psx_presentation_gpu_set_provider(PsxPresentationProvider provider,void *user);
/* Debug/export only: RGBA bytes, row zero is native VRAM row y. This explicit
 * readback is never part of snapshot capture or regular intermediate rendering. */
int psx_presentation_gpu_read_rgba(uint8_t *out, size_t capacity,
    int x, int y, int width, int height);
int psx_presentation_gpu_write_png(const char *path,int x,int y,int width,int height);
/* Debug-only qualification: temporarily changes one native texel, proves old
 * and new retained versions by rendered pixels, then restores that texel. */
int psx_presentation_gpu_test_versions(void);
#ifdef __cplusplus
}
#endif
#endif
