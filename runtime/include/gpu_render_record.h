#ifndef PSX_GPU_RENDER_RECORD_H
#define PSX_GPU_RENDER_RECORD_H

/* Renderer-call recordings for offline renderer comparison.
 *
 * While recording, every call through the active software backend is
 * written to a file with its arguments. Software rendering reads and writes VRAM only through these calls,
 * so replaying them from the opening snapshot must reproduce the closing
 * snapshot exactly; replay tools check that before comparing renderers.
 *
 * Little-endian file layout:
 *   "PSXGRR1\0", uint32 version, uint32 state size, opening sw_state_save()
 *   records: uint8 op, then the op's int32/float/uint16 fields in call order
 *   GRR_END, uint32 state size, closing sw_state_save()
 *
 * Fields per op, all int32 unless noted:
 *   SET_SCALE scale | SET_TEXTURE_FILTER on | SET_SEMI on mode
 *   SET_MASK set check | SET_TEXWIN raw(uint32) | SET_COLOR_MOD r g b raw
 *   SET_PRECISE on x0 y0 x1 y1 x2 y2 | SET_PERSPECTIVE on q0 q1 q2 (float)
 *   FILL x y w h color | COPY sx sy dx dy w h
 *   FLAT_TRI x0 y0 x1 y1 x2 y2 color
 *   GOURAUD_TRI x0 y0 c0 x1 y1 c1 x2 y2 c2
 *   TEX_TRI x0 y0 u0 v0 x1 y1 u1 v1 x2 y2 u2 v2 clut_x clut_y texpage
 *   SHADED_TEX_TRI x0 y0 u0 v0 color0 x1 y1 u1 v1 color1 x2 y2 u2 v2 color2
 *                  clut_x clut_y texpage raw
 *   FLAT_RECT x y w h color | TEX_RECT x y w h u v clut_x clut_y texpage
 *   TEX_RECT_SCALED x y w h u0 v0 u1 v1 clut_x clut_y texpage
 *   LINE x0 y0 x1 y1 color | SHADED_LINE x0 y0 c0 x1 y1 c1
 *   RENDER_DISPLAY, RENDER_DISPLAY_HIRES pitch dx dy dw dh result
 *   VRAM_WRITE x y pixel | VRAM_READ x y result
 *   TRANSFER_IN, TRANSFER_OUT x y w h, then w*h uint16 pixels (out: result)
 *   SET_DRAW_AREA x1 y1 x2 y2 | GET_DRAW_AREA x1 y1 x2 y2 (result)
 *   SET_DRAW_OFFSET x y
 *   UNSUPPORTED: no fields
 *   WIDE_CONFIGURE width offset | WIDE_SET_VIEW on shift left right
 *   WIDE_SET_TARGET base_x | WIDE_DISABLE_TARGET: no fields
 * The wide view and target setters run every frame even when widescreen is
 * off. Wide surfaces are outside the snapshot, so drawing into them, and
 * renderer re-initialization, are recorded as UNSUPPORTED and replay tools
 * reject the recording. */

#include <stdint.h>

enum {
    GRR_END = 0,
    GRR_SET_SCALE, GRR_SET_TEXTURE_FILTER, GRR_SET_SEMI, GRR_SET_MASK,
    GRR_SET_TEXWIN, GRR_SET_COLOR_MOD, GRR_SET_PRECISE, GRR_SET_PERSPECTIVE,
    GRR_FILL, GRR_COPY, GRR_FLAT_TRI, GRR_GOURAUD_TRI, GRR_TEX_TRI,
    GRR_SHADED_TEX_TRI, GRR_FLAT_RECT, GRR_TEX_RECT, GRR_TEX_RECT_SCALED,
    GRR_LINE, GRR_SHADED_LINE, GRR_RENDER_DISPLAY, GRR_RENDER_DISPLAY_HIRES,
    GRR_VRAM_WRITE, GRR_VRAM_READ, GRR_TRANSFER_IN, GRR_TRANSFER_OUT,
    GRR_SET_DRAW_AREA, GRR_GET_DRAW_AREA, GRR_SET_DRAW_OFFSET,
    GRR_UNSUPPORTED,
    GRR_WIDE_CONFIGURE, GRR_WIDE_SET_VIEW, GRR_WIDE_SET_TARGET,
    GRR_WIDE_DISABLE_TARGET,
    GRR_OP_COUNT
};

#define GRR_MAGIC "PSXGRR1"
#define GRR_VERSION 1u

/* Starts recording the software backend to path. Returns 0 when another
 * backend is active, the wide compositor is configured, a recording is
 * already open or the file cannot be written. */
int gr_record_start(const char *path);
/* Writes the closing snapshot and closes the file. Returns 0 if nothing was
 * recording or a write failed. */
int gr_record_stop(void);
int gr_record_active(void);
/* Calls made per op since recording started. */
uint64_t gr_record_count(int op);

#endif
