/* window_size.h — the game window's opening size.
 *
 * Pure arithmetic over the display's usable bounds, kept out of main.cpp so a
 * test can pin it without SDL.
 *
 * An explicit width (game.toml / settings.toml / launcher) is honoured,
 * clamped to the usable area. With no width, the window opens at a whole
 * multiple of the PS1's native 240 lines that takes about two thirds of the
 * usable height: a normal window that scales with the panel, instead of a
 * maximised one that covers the desktop. The multiple is computed from
 * physical pixels (the runtime is per-monitor DPI aware), so a 4K or 8K
 * panel still gets a proportionally large image. */
#pragma once

#define PSX_WINDOW_NATIVE_LINES 240
#define PSX_WINDOW_DEFAULT_HEIGHT_NUM 2   /* default height <= 2/3 of usable */
#define PSX_WINDOW_DEFAULT_HEIGHT_DEN 3
#define PSX_WINDOW_MIN_WIDTH 640

/* In: *w = requested width (<= 0 means no explicit choice). have_bounds /
 * bounds_w / bounds_h = the display's usable area. num:den = present aspect.
 * Out: *w, *h = the client size to open. */
static inline void psx_window_size(int* w, int* h, int num, int den,
                                   int have_bounds, int bounds_w, int bounds_h) {
    int width = *w;
    if (width <= 0) {
        int scale = 2;
        if (have_bounds) {
            scale = (bounds_h * PSX_WINDOW_DEFAULT_HEIGHT_NUM) /
                    (PSX_WINDOW_DEFAULT_HEIGHT_DEN * PSX_WINDOW_NATIVE_LINES);
            if (scale < 2) scale = 2;
        }
        width = PSX_WINDOW_NATIVE_LINES * scale * num / den;
    }
    if (width < PSX_WINDOW_MIN_WIDTH) width = PSX_WINDOW_MIN_WIDTH;
    if (have_bounds) {
        if (width > bounds_w)             width = bounds_w;
        if (width * den / num > bounds_h) width = bounds_h * num / den;
    }
    *w = width;
    *h = width * den / num;
}
