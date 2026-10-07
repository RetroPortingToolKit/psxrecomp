#include "local_mouse_sdl.h"
#include "local_mouse_policy.h"
#include "psx_keybinds.h"
#include "host_osd.h"

namespace {
SDL_Window* window_ = nullptr;
psx::LocalMouseHost host_;

uint64_t now_ns() {
#if defined(PSX_SDL3)
    return SDL_GetTicksNS();
#else
#if SDL_VERSION_ATLEAST(2, 0, 18)
    return SDL_GetTicks64() * 1000000;
#else
    // Preserve the framework's older SDL2 build path without requiring a
    // new dependency. This owner-thread clock extends the 32-bit tick era.
    static uint32_t previous = 0;
    static uint64_t era = 0;
    const uint32_t ticks = SDL_GetTicks();
    if (ticks < previous) era += uint64_t(1) << 32;
    previous = ticks;
    return (era + ticks) * 1000000;
#endif
#endif
}
uint64_t event_ns(const SDL_Event& event, uint64_t now) {
#if defined(PSX_SDL3)
    return event.common.timestamp;
#else
    // Lift wrapping SDL2 millisecond timestamps into the nearest current era.
    const uint64_t ms = now / 1000000;
    const int32_t delta = static_cast<int32_t>(event.common.timestamp - uint32_t(ms));
    if (delta < 0 && uint64_t(-int64_t(delta)) > ms) return UINT64_MAX;
    return uint64_t(int64_t(ms) + delta) * 1000000;
#endif
}
SDL_Scancode scancode(psx::MouseControl control) {
    if (control == psx::MouseControl::Escape) return SDL_SCANCODE_ESCAPE;
    if (control == psx::MouseControl::LeftAlt) return SDL_SCANCODE_LALT;
    if (control >= psx::MouseControl::Left && control <= psx::MouseControl::X2)
        return PSXKB_MOUSE_SC(int(control));
    return SDL_SCANCODE_UNKNOWN;
}
bool capture(void*, bool on) {
    if (!window_) return !on;
#if defined(PSX_SDL3)
    return SDL_SetWindowRelativeMouseMode(window_, on);
#else
    return SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE) == 0;
#endif
}
void suppress(void*, psx::MouseControl control) {
    psx_keybinds_suppress_control(1, scancode(control));
}
void notice(void*, const char* message) { host_osd_push(message, 2000); }
psx::LocalMousePolicy& state() {
    static psx::LocalMousePolicy policy({nullptr, capture, suppress, notice});
    return policy;
}
bool focus() {
    if (!window_ || SDL_GetKeyboardFocus() != window_) return false;
    const auto flags = SDL_GetWindowFlags(window_);
    return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
}
bool conflict() {
    const SDL_Scancode sc = scancode(state().hold_control());
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    for (int i = 0; i < PSX_KB_COUNT; ++i)
        if (psx_keybinds_get_button(1, i) == sc || psx_keybinds_get_button_alt(1, i) == sc)
            return true;
    return false;
}
void update(uint64_t now) {
    host_.focused = focus();
    host_.hold_conflict = conflict();
    state().update(now, host_);
}
bool control(uint64_t t, uint64_t now, psx::MouseControl control, bool down, bool repeat) {
    const SDL_Scancode sc = scancode(control);
    const bool swallowed = psx_keybinds_control_suppressed(1, sc) != 0;
    if (!down) psx_keybinds_release_control(1, sc);
    return state().control(t, now, control, down, repeat) || swallowed;
}
} // namespace

extern "C" int psx_mod_set_local_mouse_policy(const PSXModMousePolicy* policy) {
    return state().install(policy) ? 1 : 0;
}
void psx_local_mouse_clear() { state().clear(); host_ = {}; window_ = nullptr; }
bool psx_local_mouse_installed() { return state().installed(); }
void psx_local_mouse_reset() {
    state().reset();
    host_.connected = host_.analog = false;
}
void psx_local_mouse_begin(SDL_Window* window, bool live) {
    // No policy: preserve native host input without per-frame mouse polling.
    if (!state().installed()) return;
    if (window_ && window_ != window) state().reset();
    window_ = window;
    host_.live = live;
    update(now_ns());
    // Windows global state survives focus clearing SDL's cached window state.
    // Other backends may not support global mouse polling (e.g. Wayland), so
    // there reconcile whenever focused (captured or not), so a release made
    // while unfocused is caught on focus regain. Neither path manufactures a
    // down or unblocks a held activation across focus loss.
#if defined(_WIN32)
    const bool left_down = (SDL_GetGlobalMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    state().sync_left(now_ns(), left_down);
#else
    if (host_.focused)
        state().sync_left(now_ns(), (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0);
#endif
    // Catch a lost hold release, including one made outside the game window.
    const auto hold = state().hold_control();
    bool down = false;
    if (hold == psx::MouseControl::Right)
        down = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    else if (hold == psx::MouseControl::LeftAlt)
        down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_LALT] != 0;
    if (hold != psx::MouseControl::None && !down)
        state().control(0, now_ns(), hold, false, false);
}
bool psx_local_mouse_event(const SDL_Event& ev) {
    if (!state().installed()) {
        // Ownership of an already consumed source lasts through release even
        // when its plugin has just been disabled/unregistered.
        SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
        bool down = false;
        if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
            if (ev.button.button >= 1 && ev.button.button <= 5)
                sc = PSXKB_MOUSE_SC(ev.button.button);
            down = ev.type == SDL_MOUSEBUTTONDOWN;
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
#if defined(PSX_SDL3)
            sc = ev.key.scancode;
#else
            sc = ev.key.keysym.scancode;
#endif
            down = ev.type == SDL_KEYDOWN;
        }
        const bool swallowed = psx_keybinds_control_suppressed(1, sc) != 0;
        if (!down) psx_keybinds_release_control(1, sc);
        return swallowed;
    }
    const uint64_t now = now_ns(), t = event_ns(ev, now);
    update(now);
#if defined(PSX_SDL3)
    if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST || ev.type == SDL_EVENT_WINDOW_HIDDEN ||
        ev.type == SDL_EVENT_WINDOW_MINIMIZED) {
        if (window_ && ev.window.windowID == SDL_GetWindowID(window_)) state().reset();
    } else if (ev.type == SDL_EVENT_MOUSE_REMOVED) {
        state().reset();
    }
#else
    if (window_ && ev.type == SDL_WINDOWEVENT && ev.window.windowID == SDL_GetWindowID(window_) &&
        (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST || ev.window.event == SDL_WINDOWEVENT_HIDDEN ||
         ev.window.event == SDL_WINDOWEVENT_MINIMIZED)) state().reset();
#endif
    if (ev.type == SDL_MOUSEMOTION) {
        if (!window_ || ev.motion.windowID != SDL_GetWindowID(window_)) return false;
        if (ev.motion.which == SDL_TOUCH_MOUSEID) return false;
#if defined(PSX_SDL3)
        if (ev.motion.which == SDL_PEN_MOUSEID) return false;
#endif
        state().motion(t, now, ev.motion.xrel, ev.motion.yrel);
    } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
        if (ev.button.which == SDL_TOUCH_MOUSEID) return false;
#if defined(PSX_SDL3)
        if (ev.button.which == SDL_PEN_MOUSEID) return false;
#endif
        // An owned activation release stays immediate even outside the game
        // window. Only a fresh down in this window may acquire capture.
        if (!window_ || (ev.button.windowID != SDL_GetWindowID(window_) &&
                        ev.type != SDL_MOUSEBUTTONUP)) return false;
        if (ev.button.button >= 1 && ev.button.button <= 5)
            return control(t, now, psx::MouseControl(ev.button.button), ev.type == SDL_MOUSEBUTTONDOWN, false);
    } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
#if defined(PSX_SDL3)
        if (!window_ || ev.key.windowID != SDL_GetWindowID(window_)) return false;
        const auto sc = ev.key.scancode;
#else
        if (!window_ || ev.key.windowID != SDL_GetWindowID(window_)) return false;
        const auto sc = ev.key.keysym.scancode;
#endif
        const auto c = sc == SDL_SCANCODE_ESCAPE ? psx::MouseControl::Escape :
            sc == SDL_SCANCODE_LALT ? psx::MouseControl::LeftAlt : psx::MouseControl::None;
        if (c != psx::MouseControl::None)
            return control(t, now, c, ev.type == SDL_KEYDOWN, ev.key.repeat != 0);
    }
    return false;
}
void psx_local_mouse_pad(bool connected, bool analog, uint16_t buttons, uint8_t& rx, uint8_t& ry) {
    if (!state().installed()) return;
    host_.connected = connected;
    host_.analog = analog;
    host_.native_right = rx != 128 || ry != 128;
    host_.start = (buttons & 0x0008) == 0;
    host_.buttons = buttons;
    const uint64_t now = now_ns();
    update(now);
    state().sample(now, rx, ry);
}
