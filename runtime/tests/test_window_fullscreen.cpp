#include "window_fullscreen.h"
#if defined(PSX_SDL3)
#include <SDL3/SDL_main.h>
#endif
#include <cstdio>

static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError()); ++failures; }
}

int main() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_Window* window = SDL_CreateWindow("Fullscreen regression", 100, 100,
                                          640, 480, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (!window) return 1;
    PsxWindowFullscreen state;
    SDL_SetWindowPosition(window, 100, 100);
    int old_x, old_y, old_w, old_h;
    SDL_GetWindowPosition(window, &old_x, &old_y);
    SDL_GetWindowSize(window, &old_w, &old_h);
#if defined(PSX_SDL3)
    const SDL_DisplayID original_display = SDL_GetDisplayForWindow(window);
    SDL_DisplayMode original_mode{};
    check(SDL_GetCurrentDisplayMode(original_display, &original_mode) == 0, "read desktop display mode");
    SDL_ShowCursor();
#endif
    for (int i = 0; i < 3; ++i) {
        check(psx_window_fullscreen_set(window, &state, 1) == 0, "enter borderless");
#if defined(_WIN32) && defined(PSX_SDL3)
        check(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN), "borderless never arms native fullscreen");
        check((SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) != 0, "borderless has no decorations");
        check(SDL_GetWindowFullscreenMode(window) == nullptr, "borderless has no exclusive display mode");
        SDL_Rect bounds{};
        SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &bounds);
        int full_w, full_h; SDL_GetWindowSize(window, &full_w, &full_h);
        check(full_w == bounds.w && full_h == bounds.h + 1,
              "borderless covers the current display (one row over: stays composed)");
        check(!SDL_CursorVisible(), "fullscreen cursor hidden");
        SDL_DisplayMode current{};
        check(SDL_GetCurrentDisplayMode(original_display, &current) == 0 &&
              current.w == original_mode.w && current.h == original_mode.h &&
              current.refresh_rate == original_mode.refresh_rate,
              "borderless does not change the desktop display mode");
#endif
        check(psx_window_fullscreen_set(window, &state, 0) == 0, "leave borderless");
        int x, y, w, h;
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        check(x == old_x && y == old_y && w == old_w && h == old_h, "original window geometry restored");
        check(!(SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS), "window border restored");
        check((SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE) != 0, "resizability restored");
#if defined(PSX_SDL3)
        check(SDL_CursorVisible(), "windowed cursor visibility restored");
#endif
    }
    check(psx_window_fullscreen_set(window, &state, 2) == 0, "enter exclusive");
#if defined(PSX_SDL3)
    check(SDL_GetWindowFullscreenMode(window) != nullptr, "exclusive selects an explicit display mode");
#endif
    check(psx_window_fullscreen_set(window, &state, 1) == 0, "exclusive to borderless");
    check(psx_window_fullscreen_set(window, &state, 0) == 0, "borderless to windowed");
    check(psx_window_fullscreen_set(window, &state, 3) == -1 && state.mode == 0, "invalid mode leaves window unchanged");
#if defined(PSX_SDL3)
    SDL_HideCursor();
    check(psx_window_fullscreen_set(window, &state, 1) == 0, "enter borderless with cursor already hidden");
    check(psx_window_fullscreen_set(window, &state, 0) == 0 && !SDL_CursorVisible(),
          "windowed mode preserves a previously hidden cursor");
    SDL_ShowCursor();
    SDL_MaximizeWindow(window);
    SDL_SyncWindow(window);
    const bool was_maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
    check(psx_window_fullscreen_set(window, &state, 1) == 0, "maximized to borderless");
    check(psx_window_fullscreen_set(window, &state, 0) == 0, "borderless restores maximized window");
    SDL_SyncWindow(window);
    check(!was_maximized || (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED),
          "original maximized state restored");
#endif
    SDL_DestroyWindow(window);
    SDL_Quit();
    return failures ? 1 : 0;
}
