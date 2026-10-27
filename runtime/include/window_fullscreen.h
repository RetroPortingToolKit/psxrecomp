#pragma once

#include "psx_sdl.h"
#include <cstdlib>
#include <cmath>

/* On Windows, borderless stays a normal desktop-sized window with either SDL
 * backend. Other window managers retain native desktop fullscreen (Wayland
 * owns placement). */
struct PsxWindowFullscreen {
    int mode = 0;
    int x = 0, y = 0, width = 0, height = 0;
    bool maximized = false;
    bool bordered = true;
    bool resizable = true;
    bool cursor_visible = true;
};

static inline int psx_window_fullscreen_set(SDL_Window* window,
                                           PsxWindowFullscreen* state, int mode)
{
    if (!window || !state || mode < 0 || mode > 2) return -1;
    if (state->mode == mode) return 0;
    if (state->mode == 0) {
        const auto flags = SDL_GetWindowFlags(window);
        state->maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        state->bordered = (flags & SDL_WINDOW_BORDERLESS) == 0;
        state->resizable = (flags & SDL_WINDOW_RESIZABLE) != 0;
        if (state->maximized) {
            SDL_RestoreWindow(window);
#if defined(PSX_SDL3)
            SDL_SyncWindow(window);
#endif
        }
        SDL_GetWindowPosition(window, &state->x, &state->y);
        SDL_GetWindowSize(window, &state->width, &state->height);
#if defined(PSX_SDL3)
        state->cursor_visible = SDL_CursorVisible();
#else
        state->cursor_visible = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE;
#endif
    }

    SDL_Rect desktop{};
#if defined(PSX_SDL3)
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (mode == 1 && (!display || !SDL_GetDisplayBounds(display, &desktop))) return -1;
    SDL_DisplayMode exclusive{};
    if (mode == 2) {
        const SDL_DisplayMode* current = SDL_GetDesktopDisplayMode(display);
        if (!current || !SDL_GetClosestFullscreenDisplayMode(display, current->w,
                current->h, current->refresh_rate, true, &exclusive)) return -1;
        /* Optional presentation-only mode for an integer cadence (e.g. PAL
         * 50 Hz on a 100 Hz panel). Never accept a driver's approximate mode,
         * change resolution, or feed this rate into guest video timing. SDL
         * restores the desktop mode when exclusive fullscreen is released. */
        if (const char *text = std::getenv("PSX_EXCLUSIVE_REFRESH_HZ")) {
            const double hz = std::atof(text);
            SDL_DisplayMode requested{};
            if (hz >= 30.0 && hz <= 1000.0 &&
                SDL_GetClosestFullscreenDisplayMode(display, current->w, current->h,
                    (float)hz, true, &requested) &&
                requested.w == current->w && requested.h == current->h &&
                std::fabs((double)requested.refresh_rate - hz) <= hz * 0.002)
                exclusive = requested;
        }
    }
    if (!SDL_SetWindowFullscreen(window, false)) return -1;
    if (!SDL_SetWindowFullscreenMode(window, mode == 2 ? &exclusive : nullptr)) return -1;
#else
    const int display = SDL_GetWindowDisplayIndex(window);
    if (mode == 1 && (display < 0 || SDL_GetDisplayBounds(display, &desktop) != 0)) return -1;
    if (SDL_SetWindowFullscreen(window, 0) != 0) return -1;
#endif
    /* Maximized windows can ignore manual geometry; restore before resizing. */
    SDL_RestoreWindow(window);
#if defined(PSX_SDL3)
    SDL_SyncWindow(window);
#endif
    if (mode == 1) {
#if defined(_WIN32)
        SDL_SetWindowBordered(window, SDL_FALSE);
        SDL_SetWindowResizable(window, SDL_FALSE);
        SDL_SetWindowPosition(window, desktop.x, desktop.y);
        SDL_SetWindowSize(window, desktop.w, desktop.h);
#if defined(PSX_SDL3)
        SDL_SyncWindow(window);
#endif
#elif defined(PSX_SDL3)
        if (!SDL_SetWindowFullscreen(window, true)) return -1;
#else
        if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) return -1;
#endif
    } else {
        SDL_SetWindowBordered(window, state->bordered ? SDL_TRUE : SDL_FALSE);
        SDL_SetWindowResizable(window, state->resizable ? SDL_TRUE : SDL_FALSE);
        SDL_SetWindowSize(window, state->width, state->height);
        SDL_SetWindowPosition(window, state->x, state->y);
        if (mode == 2) {
#if defined(PSX_SDL3)
            if (!SDL_SetWindowFullscreen(window, true)) return -1;
#else
            if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN) != 0) return -1;
#endif
        } else if (state->maximized) {
            SDL_MaximizeWindow(window);
        }
    }
    state->mode = mode;
#if defined(PSX_SDL3)
    if (mode || !state->cursor_visible) SDL_HideCursor();
    else SDL_ShowCursor();
#else
    SDL_ShowCursor(mode || !state->cursor_visible ? SDL_DISABLE : SDL_ENABLE);
#endif
    return 0;
}
