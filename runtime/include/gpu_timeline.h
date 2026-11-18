#ifndef PSXRECOMP_GPU_TIMELINE_H
#define PSXRECOMP_GPU_TIMELINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPU frame timeline: an always-on, guest-cycle-stamped record of the events
 * that decide WHAT a present shows — vblank, display-start flips, draw-area
 * changes, GPU linked-list DMA start/end, and each present (swapped or
 * skipped). The GP0, display and presented-image rings are all tagged by
 * frame number only, which cannot order a present against a draw inside the
 * same frame; this ring can. Frozen by the capture mark. */
#define GPU_TIMELINE_CAP 32768u  /* power of two; ~40 s at typical rates */

typedef enum {
    GTL_VBLANK        = 1,  /* a = lcf after toggle */
    GTL_DISP_START    = 2,  /* a = x, b = y (GP1 05h) */
    GTL_DRAW_AREA     = 3,  /* a = left, b = top (GP0 E3h) */
    GTL_LL_START      = 4,  /* a = OT start address */
    GTL_LL_END        = 5,  /* a = nodes, b = words */
    GTL_PRESENT       = 6,  /* a = path, b = disp_x | disp_y << 16 */
    GTL_PRESENT_SKIP  = 7,  /* a = path, b = disp_x | disp_y << 16 (no swap) */
    GTL_DISP_CAPTURE  = 8,  /* display-ring capture; b = disp_x | disp_y << 16 */
    GTL_FILL          = 9,  /* a = x | y << 16, b = w | h << 16 (GP0 02h) */
} GpuTimelineKind;

/* Present paths recorded in GTL_PRESENT/GTL_PRESENT_SKIP `a`. */
enum { GTL_PATH_WIDE_FBO = 1, GTL_PATH_VRAM_FBO = 2, GTL_PATH_CPU = 3,
       GTL_PATH_HOLD = 4,GTL_PATH_GEOMETRY=5 };

typedef struct {
    uint64_t cyc;    /* psx_cycle_count */
    uint32_t frame;  /* s_frame_count */
    uint32_t a, b;
    uint8_t  kind;
    uint8_t  pad[3];
} GpuTimelineEntry;

void     gpu_timeline_note(uint8_t kind, uint32_t a, uint32_t b);
void     gpu_timeline_set_frozen(int frozen);
uint64_t gpu_timeline_total(void);
int      gpu_timeline_get(uint64_t seq, GpuTimelineEntry *out);
const char *gpu_timeline_kind_name(uint8_t kind);

#ifdef __cplusplus
}
#endif

#endif
