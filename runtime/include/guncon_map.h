/* guncon_map.h - host pointer to GunCon X/Y (PS1B-305).
 *
 * Host policy from recomp-corpus references/ps1/PERIPHERAL-GUNCON-SPEC.md.
 * The gun counts X in 8 MHz clocks since HSYNC and Y in scanlines since
 * VSYNC; GP1(06h) X1/X2 are video clocks after HSYNC and GP1(07h) Y1/Y2 are
 * scanlines after VSYNC (PSX-SPX). So a point a fraction fx across the shown
 * display area is at video clock X1 + fx*(X2-X1), which reads as that times
 * 8 / f_video, less a constant gun offset (-11, open in the spec). Vertically,
 * the runtime shows source_height lines starting at the line GP1(07h) Y1
 * names (display_scanout.h), so the row under the pointer is its scanline. */
#pragma once

#include <math.h>
#include <stdint.h>

#include "display_scanout.h"

#define GUNCON_X_OFFSET        (-11)
#define GUNCON_NTSC_VIDEO_MHZ  53.693175
#define GUNCON_PAL_VIDEO_MHZ   53.203425
#define GUNCON_NO_LIGHT_X      0x0001u
#define GUNCON_NO_LIGHT_Y      0x000Au

/* fx, fy: the pointer as a fraction of the presented game image, 0..1
 * inclusive. x1/x2: GP1(06h); y1/y2: GP1(07h) (raw); pal: GP1(08h) bit 3.
 * Writes the gun's X/Y and returns 1, or writes "no light" and returns 0
 * when the pointer is outside the image. An unset range falls back to the
 * standard fullscreen one (PSX-SPX: NTSC X 600..3160, Y 16..256; PAL X
 * 624..3184, Y 20..308). */
static inline int guncon_map_pointer(double fx, double fy,
                                     uint32_t x1, uint32_t x2,
                                     uint32_t y1, uint32_t y2, int pal,
                                     uint16_t *gx, uint16_t *gy) {
    if (!(fx >= 0.0 && fx <= 1.0 && fy >= 0.0 && fy <= 1.0)) {
        *gx = (uint16_t)GUNCON_NO_LIGHT_X;
        *gy = (uint16_t)GUNCON_NO_LIGHT_Y;
        return 0;
    }
    if (x2 <= x1) {
        x1 = pal ? 624u : 600u;
        x2 = x1 + 2560u;
    }
    const double f = pal ? GUNCON_PAL_VIDEO_MHZ : GUNCON_NTSC_VIDEO_MHZ;
    long x = lround(((double)x1 + fx * (double)(x2 - x1)) * 8.0 / f) + GUNCON_X_OFFSET;
    if (x < 2) x = 2;            /* never the "no light" X */
    if (x > 0xFFFF) x = 0xFFFF;

    PsxDisplayVerticalLayout v = psx_display_vertical_layout(pal, y1, y2);
    uint32_t top = y1, h = v.source_height;
    if (!v.valid || h == 0u) {
        top = pal ? 20u : 16u;
        h = pal ? 288u : 240u;
    }
    uint32_t row = (uint32_t)(fy * (double)h);
    if (row >= h) row = h - 1u;
    *gx = (uint16_t)x;
    *gy = (uint16_t)(top + row);
    return 1;
}

/* The largest num:den rectangle centred in a w x h window (the runtime's
 * letterbox). Outputs its origin and size in the same units as w, h. */
static inline void guncon_letterbox(int w, int h, int num, int den,
                                    int *rx, int *ry, int *rw, int *rh) {
    if (num <= 0 || den <= 0) { num = 4; den = 3; }
    int dw = w, dh = (int)(((long long)w * den) / num);
    if (dh > h) { dh = h; dw = (int)(((long long)h * num) / den); }
    *rx = (w - dw) / 2;
    *ry = (h - dh) / 2;
    *rw = dw;
    *rh = dh;
}

/* Active-low buttons from configured host bindings. An offscreen shot also
 * suppresses light; this is independent of coordinate and PAL mapping. */
static inline uint16_t guncon_host_buttons(int trigger, int a, int b,
                                           int no_light, int offscreen_shot,
                                           int *out_no_light) {
    uint16_t w = 0xFFFFu;
    if (trigger || offscreen_shot) w &= (uint16_t)~0x2000u;
    if (a) w &= (uint16_t)~0x0008u;
    if (b) w &= (uint16_t)~0x4000u;
    *out_no_light = (no_light || offscreen_shot) ? 1 : 0;
    return w;
}
