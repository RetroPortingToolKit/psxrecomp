#pragma once

/*
 * Runtime-facing SDL compatibility seam.
 *
 * PSXRecomp uses SDL3 by default, while PSX_SDL_BACKEND=SDL2 keeps the
 * previous implementation available for comparison and older platforms.
 */
#if defined(PSX_SDL3)

#define SDL_ENABLE_OLD_NAMES
#define SDL_FUNCTION_POINTER_IS_VOID_POINTER
#include <SDL3/SDL.h>

#ifndef SDL_WINDOW_SHOWN
#define SDL_WINDOW_SHOWN 0
#endif
#ifndef SDL_WINDOW_FULLSCREEN_DESKTOP
#define SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN
#endif
#ifndef SDL_RENDERER_ACCELERATED
#define SDL_RENDERER_ACCELERATED 0x00000001u
#endif
#ifndef SDL_RENDERER_PRESENTVSYNC
#define SDL_RENDERER_PRESENTVSYNC 0x00000002u
#endif
#ifndef SDL_RENDERER_SOFTWARE
#define SDL_RENDERER_SOFTWARE 0x00000004u
#endif
#ifndef SDL_MUTEX_TIMEDOUT
#define SDL_MUTEX_TIMEDOUT 1
#endif
#ifndef SDL_HINT_RENDER_SCALE_QUALITY
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"
#endif

static inline int psx_sdl_init(SDL_InitFlags flags)
{
    return SDL_Init(flags) ? 0 : -1;
}

static inline int psx_sdl_init_subsystem(SDL_InitFlags flags)
{
    return SDL_InitSubSystem(flags) ? 0 : -1;
}

static inline SDL_Window *psx_sdl_create_window(
    const char *title, int x, int y, int w, int h, Uint64 flags)
{
    (void)x;
    (void)y;
    return SDL_CreateWindow(title, w, h, (SDL_WindowFlags)flags);
}

static inline SDL_Renderer *psx_sdl_create_renderer(
    SDL_Window *window, int driver_index, Uint32 flags)
{
    (void)driver_index;
    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (renderer && (flags & SDL_RENDERER_PRESENTVSYNC)) {
        (void)SDL_SetRenderVSync(renderer, 1);
    }
    return renderer;
}

static inline int psx_sdl_render_set_logical_size(
    SDL_Renderer *renderer, int w, int h)
{
    return SDL_SetRenderLogicalPresentation(
               renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX)
               ? 0
               : -1;
}

static inline int psx_sdl_render_copy(
    SDL_Renderer *renderer, SDL_Texture *texture,
    const SDL_Rect *src, const SDL_Rect *dst)
{
    SDL_FRect srcf;
    SDL_FRect dstf;
    const SDL_FRect *srcp = NULL;
    const SDL_FRect *dstp = NULL;
    if (src) {
        srcf.x = (float)src->x;
        srcf.y = (float)src->y;
        srcf.w = (float)src->w;
        srcf.h = (float)src->h;
        srcp = &srcf;
    }
    if (dst) {
        dstf.x = (float)dst->x;
        dstf.y = (float)dst->y;
        dstf.w = (float)dst->w;
        dstf.h = (float)dst->h;
        dstp = &dstf;
    }
    return SDL_RenderTexture(renderer, texture, srcp, dstp) ? 0 : -1;
}

static inline int psx_sdl_render_set_vsync(
    SDL_Renderer *renderer, int enabled)
{
    return SDL_SetRenderVSync(renderer, enabled) ? 0 : -1;
}

static inline int psx_sdl_gl_make_current(
    SDL_Window *window, SDL_GLContext context)
{
    return SDL_GL_MakeCurrent(window, context) ? 0 : -1;
}

static inline int psx_sdl_gl_set_swap_interval(int interval)
{
    return SDL_GL_SetSwapInterval(interval) ? 0 : -1;
}

static inline void psx_sdl_gl_get_drawable_size(
    SDL_Window *window, int *w, int *h)
{
    (void)SDL_GetWindowSizeInPixels(window, w, h);
}

static inline int psx_sdl_get_display_usable_bounds(
    SDL_DisplayID display, SDL_Rect *bounds)
{
    if (!display) display = SDL_GetPrimaryDisplay();
    return SDL_GetDisplayUsableBounds(display, bounds) ? 0 : -1;
}

static inline int psx_sdl_get_current_display_mode(
    SDL_DisplayID display, SDL_DisplayMode *mode)
{
    if (!display) display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *current = SDL_GetCurrentDisplayMode(display);
    if (!current) return -1;
    *mode = *current;
    return 0;
}

/* The highest refresh the display offers at the current mode's size (a VRR
 * or ProMotion panel's maximum); 0 when unknown. */
static inline double psx_sdl_display_max_refresh(SDL_DisplayID display)
{
    if (!display) display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *cur = SDL_GetCurrentDisplayMode(display);
    double best = cur ? (double)cur->refresh_rate : 0.0;
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (modes) {
        for (int i = 0; i < count; ++i)
            if (modes[i] && cur && modes[i]->w == cur->w && modes[i]->h == cur->h &&
                (double)modes[i]->refresh_rate > best)
                best = (double)modes[i]->refresh_rate;
        SDL_free(modes);
    }
    return best;
}

/* Opt-in high-pixel-density game window. Without it, SDL3 on a Retina Mac
 * gives the GL context a drawable in POINTS (half the panel resolution) and
 * the compositor stretches it, so any internal resolution above that is
 * averaged away. Only set when the player chose an internal resolution. */
#define PSX_SDL_WINDOW_HIGH_DENSITY SDL_WINDOW_HIGH_PIXEL_DENSITY

/* Pixel height of the display a window is on (the primary display when the
 * window is NULL), for "Match display". 0 when unknown. */
static inline int psx_sdl_display_pixel_height(SDL_Window *window)
{
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : 0;
    if (!display) display = SDL_GetPrimaryDisplay();
    if (!display) return 0;
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    if (!mode || mode->h <= 0) return 0;
    float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
    return (int)((float)mode->h * density + 0.5f);
}

static inline const Uint8 *psx_sdl_get_keyboard_state(int *count)
{
    return (const Uint8 *)(const void *)SDL_GetKeyboardState(count);
}

static inline SDL_JoystickID psx_sdl_joystick_id_for_index(int index)
{
    int count = 0;
    SDL_JoystickID result = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids && index >= 0 && index < count) result = ids[index];
    SDL_free(ids);
    return result;
}

static inline int psx_sdl_num_joysticks(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    SDL_free(ids);
    return count;
}

static inline bool psx_sdl_is_game_controller(int index)
{
    SDL_JoystickID id = psx_sdl_joystick_id_for_index(index);
    return id && SDL_IsGamepad(id);
}

static inline SDL_Gamepad *psx_sdl_game_controller_open(int index)
{
    SDL_JoystickID id = psx_sdl_joystick_id_for_index(index);
    return id ? SDL_OpenGamepad(id) : NULL;
}

static inline SDL_GUID psx_sdl_joystick_get_device_guid(int index)
{
    return SDL_GetJoystickGUIDForID(psx_sdl_joystick_id_for_index(index));
}

static inline SDL_JoystickID psx_sdl_joystick_get_device_instance_id(int index)
{
    return psx_sdl_joystick_id_for_index(index);
}

static inline void psx_sdl_joystick_get_guid_string(
    SDL_GUID guid, char *buffer, int buffer_size)
{
    SDL_GUIDToString(guid, buffer, buffer_size);
}

static inline int psx_sdl_cond_wait_timeout(
    SDL_Condition *condition, SDL_Mutex *mutex, Sint32 timeout_ms)
{
    return SDL_WaitConditionTimeout(condition, mutex, timeout_ms)
               ? 0
               : SDL_MUTEX_TIMEDOUT;
}

#undef SDL_Init
#define SDL_Init psx_sdl_init
#undef SDL_InitSubSystem
#define SDL_InitSubSystem psx_sdl_init_subsystem
#undef SDL_CreateWindow
#define SDL_CreateWindow psx_sdl_create_window
#undef SDL_CreateRenderer
#define SDL_CreateRenderer psx_sdl_create_renderer
#undef SDL_RenderSetLogicalSize
#define SDL_RenderSetLogicalSize psx_sdl_render_set_logical_size
#undef SDL_RenderCopy
#define SDL_RenderCopy psx_sdl_render_copy
#undef SDL_RenderSetVSync
#define SDL_RenderSetVSync psx_sdl_render_set_vsync
#undef SDL_GL_MakeCurrent
#define SDL_GL_MakeCurrent psx_sdl_gl_make_current
#undef SDL_GL_SetSwapInterval
#define SDL_GL_SetSwapInterval psx_sdl_gl_set_swap_interval
#define SDL_GL_GetDrawableSize psx_sdl_gl_get_drawable_size
#undef SDL_GetDisplayUsableBounds
#define SDL_GetDisplayUsableBounds psx_sdl_get_display_usable_bounds
#undef SDL_GetCurrentDisplayMode
#define SDL_GetCurrentDisplayMode psx_sdl_get_current_display_mode
#undef SDL_GetKeyboardState
#define SDL_GetKeyboardState psx_sdl_get_keyboard_state
#define SDL_NumJoysticks psx_sdl_num_joysticks
#undef SDL_IsGameController
#define SDL_IsGameController psx_sdl_is_game_controller
#undef SDL_GameControllerOpen
#define SDL_GameControllerOpen psx_sdl_game_controller_open
#undef SDL_JoystickGetDeviceGUID
#define SDL_JoystickGetDeviceGUID psx_sdl_joystick_get_device_guid
#undef SDL_JoystickGetDeviceInstanceID
#define SDL_JoystickGetDeviceInstanceID psx_sdl_joystick_get_device_instance_id
#undef SDL_JoystickGetGUIDString
#define SDL_JoystickGetGUIDString psx_sdl_joystick_get_guid_string
#undef SDL_CondWaitTimeout
#define SDL_CondWaitTimeout psx_sdl_cond_wait_timeout

#else

#include <SDL.h>

#define PSX_SDL_WINDOW_HIGH_DENSITY SDL_WINDOW_ALLOW_HIGHDPI

static inline int psx_sdl_display_pixel_height(SDL_Window *window)
{
    int index = window ? SDL_GetWindowDisplayIndex(window) : 0;
    SDL_DisplayMode mode;
    if (index < 0) index = 0;
    if (SDL_GetCurrentDisplayMode(index, &mode) != 0 || mode.h <= 0) return 0;
    return mode.h;
}

static inline double psx_sdl_display_max_refresh(int display)
{
    SDL_DisplayMode cur, m;
    if (display < 0) display = 0;
    if (SDL_GetCurrentDisplayMode(display, &cur) != 0) return 0.0;
    double best = (double)cur.refresh_rate;
    const int n = SDL_GetNumDisplayModes(display);
    for (int i = 0; i < n; ++i)
        if (SDL_GetDisplayMode(display, i, &m) == 0 && m.w == cur.w && m.h == cur.h &&
            (double)m.refresh_rate > best)
            best = (double)m.refresh_rate;
    return best;
}
#endif
