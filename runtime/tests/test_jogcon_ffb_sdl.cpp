/* JogCon force feedback SDL backend against SDL's virtual joystick.
 *
 * SDL has no virtual haptic device: SDL3 virtual joysticks expose rumble,
 * trigger rumble, LED and sensors, never SDL_Haptic effects, and no software
 * haptic driver exists. So this checks what SDL can represent: a virtual
 * wheel-type joystick with rumble is NOT driven by the force-feedback path
 * (gamepads keep their rumble path), the backend calls are safe no-ops on it,
 * and close / neutral leave nothing running. Real constant / spring output
 * needs a physical force-feedback wheel. Exit 77 = skipped (SDL2 build). */
#include "psx_sdl.h"
#include "psx_jogcon_ffb_sdl.h"

#include <cstdio>

static int s_fail;
#define CHECK(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL %s\n", m); ++s_fail; } } while (0)

#if defined(PSX_SDL3)
static int s_rumbles;
static bool SDLCALL virt_rumble(void*, Uint16, Uint16) { ++s_rumbles; return true; }

int main() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD) != 0) {  /* psx_sdl.h: 0 = ok */
        std::printf("skip: SDL_Init: %s\n", SDL_GetError());
        return 77;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_WHEEL;
    desc.naxes = 6;
    desc.nbuttons = 15;
    desc.name = "psxrecomp virtual wheel";
    desc.Rumble = virt_rumble;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0, "virtual wheel attached");
    SDL_Joystick* joy = id ? SDL_OpenJoystick(id) : nullptr;
    CHECK(joy != nullptr, "virtual wheel opened");

    PsxJogconHaptic h;
    const int opened = psx_jogcon_haptic_open(&h, joy);
    CHECK(opened == 0 && h.haptic == nullptr && h.constant < 0 && h.spring < 0,
          "no SDL haptic behind a virtual joystick: FFB path stays off");
    PsxJogconFfbBackend b = psx_jogcon_haptic_backend(&h);
    PsxJogconFfbDriver d = {{0, 0}, 0};
    psx_jogcon_ffb_tick(&d, &b, 1, 3, 15, 0);     /* plays into no device */
    psx_jogcon_ffb_neutral(&d, &b);
    CHECK(s_rumbles == 0, "FFB never falls back to gamepad rumble");
    CHECK(SDL_RumbleJoystick(joy, 0x4000, 0x4000, 50) && s_rumbles == 1,
          "the virtual device itself is live (rumble reaches it)");
    psx_jogcon_haptic_close(&h);
    psx_jogcon_haptic_close(&h);                  /* idempotent */
    CHECK(psx_jogcon_haptic_open(&h, nullptr) == 0, "null joystick is safe");

    if (joy) SDL_CloseJoystick(joy);
    if (id) SDL_DetachVirtualJoystick(id);
    SDL_Quit();
    if (s_fail) return 1;
    std::puts("JogCon FFB SDL (virtual joystick) tests passed");
    return 0;
}
#else
int main() { std::puts("skip: needs SDL3 virtual joysticks"); return 77; }
#endif
