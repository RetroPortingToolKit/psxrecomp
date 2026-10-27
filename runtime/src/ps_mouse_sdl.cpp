#include "ps_mouse_sdl.h"
#include "ps_mouse_input.h"
#include "host_osd.h"

namespace {
SDL_Window* window_ = nullptr;
bool enabled_ = false;
bool live_ = false;
bool captured_ = false;
bool hinted_ = false;

bool set_relative(bool on) {
    if (!window_) return !on;
#if defined(PSX_SDL3)
    return SDL_SetWindowRelativeMouseMode(window_, on);
#else
    return SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) == 0;
#endif
}

bool focused() {
    if (!window_ || SDL_GetKeyboardFocus() != window_) return false;
    const auto flags = SDL_GetWindowFlags(window_);
    return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
}

bool our_window(Uint32 window_id) {
    return window_ && window_id == SDL_GetWindowID(window_);
}

void acquire() {
    if (!set_relative(true)) {
        host_osd_push("PlayStation Mouse: the mouse could not be captured", 2000);
        return;
    }
    captured_ = true;
    ps_mouse_input_reset();
    host_osd_push("PlayStation Mouse captured (Esc releases)", 1500);
}

int ps_button(Uint8 sdl_button) {
    if (sdl_button == SDL_BUTTON_LEFT) return PS_MOUSE_LEFT;
    if (sdl_button == SDL_BUTTON_RIGHT) return PS_MOUSE_RIGHT;
    return 0;
}
} // namespace

void psx_ps_mouse_release() {
    if (captured_) set_relative(false);
    captured_ = false;
    ps_mouse_input_reset();
}

void psx_ps_mouse_set_enabled(bool enabled) {
    if (!enabled) psx_ps_mouse_release();
    enabled_ = enabled;
    if (!enabled) hinted_ = false;
}

bool psx_ps_mouse_enabled() { return enabled_; }
bool psx_ps_mouse_captured() { return captured_; }

void psx_ps_mouse_begin(SDL_Window* window, bool live) {
    if (!enabled_) return;
    if (window_ && window_ != window) psx_ps_mouse_release();
    window_ = window;
    live_ = live;
    if (captured_ && (!live_ || !focused())) psx_ps_mouse_release();
    if (live_ && !hinted_ && window_) {
        hinted_ = true;
        host_osd_push("PlayStation Mouse: click in the window to capture", 3000);
    }
}

bool psx_ps_mouse_event(const SDL_Event& ev) {
    if (!enabled_) return false;
#if defined(PSX_SDL3)
    if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST || ev.type == SDL_EVENT_WINDOW_HIDDEN ||
        ev.type == SDL_EVENT_WINDOW_MINIMIZED) {
        if (our_window(ev.window.windowID)) psx_ps_mouse_release();
        return false;
    }
    if (ev.type == SDL_EVENT_MOUSE_REMOVED) {
        psx_ps_mouse_release();
        return false;
    }
#else
    if (ev.type == SDL_WINDOWEVENT && our_window(ev.window.windowID) &&
        (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
         ev.window.event == SDL_WINDOWEVENT_HIDDEN ||
         ev.window.event == SDL_WINDOWEVENT_MINIMIZED)) {
        psx_ps_mouse_release();
        return false;
    }
#endif
    if (ev.type == SDL_MOUSEMOTION) {
        if (!captured_ || ev.motion.which == SDL_TOUCH_MOUSEID) return false;
#if defined(PSX_SDL3)
        if (ev.motion.which == SDL_PEN_MOUSEID) return false;
#endif
        ps_mouse_input_motion((float)ev.motion.xrel, (float)ev.motion.yrel);
        return true;
    }
    if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
        if (ev.button.which == SDL_TOUCH_MOUSEID) return false;
#if defined(PSX_SDL3)
        if (ev.button.which == SDL_PEN_MOUSEID) return false;
#endif
        const bool down = ev.type == SDL_MOUSEBUTTONDOWN;
        if (!captured_) {
            // Only a fresh left click in the focused game window, on a live
            // frame, acquires. The acquiring click is not a guest click.
            if (down && live_ && ev.button.button == SDL_BUTTON_LEFT &&
                our_window(ev.button.windowID) && focused()) {
                acquire();
                return captured_;
            }
            return false;
        }
        // The release of a button pressed before capture reports "up" for a
        // button the guest never saw down; that is harmless.
        if (const int b = ps_button(ev.button.button)) ps_mouse_input_button(b, down);
        return true;
    }
    if (captured_ && (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)) {
#if defined(PSX_SDL3)
        const auto sc = ev.key.scancode;
#else
        const auto sc = ev.key.keysym.scancode;
#endif
        if (sc == SDL_SCANCODE_ESCAPE) {
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                psx_ps_mouse_release();
                host_osd_push("PlayStation Mouse released", 1200);
            }
            return true;
        }
    }
    return false;
}
