/* psx_jogcon_ffb_sdl.cpp - SDL haptic backend for JogCon force feedback. */
#include "psx_sdl.h"
#include "psx_jogcon_ffb_sdl.h"

#include <cstring>

static void ffb_effect(SDL_HapticEffect *e, Uint16 type, uint32_t ms) {
    std::memset(e, 0, sizeof *e);
    e->type = type;
    if (type == SDL_HAPTIC_CONSTANT) {
        e->constant.direction.type = SDL_HAPTIC_CARTESIAN;
        e->constant.direction.dir[0] = 1;
        e->constant.length = ms;
    } else {
        e->condition.direction.type = SDL_HAPTIC_CARTESIAN;
        e->condition.length = ms;
        e->condition.right_sat[0] = e->condition.left_sat[0] = 0xFFFF;
    }
}

extern "C" int psx_jogcon_haptic_open(PsxJogconHaptic *h, void *joystick) {
    if (!h) return 0;
    h->haptic = nullptr;
    h->constant = h->spring = -1;
    if (!joystick) return 0;
    /* psx_sdl.h: SDL_InitSubSystem returns 0 on success on SDL2 and SDL3. */
    if (!SDL_WasInit(SDL_INIT_HAPTIC) &&
        SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0)
        return 0;
    SDL_Haptic *haptic = SDL_HapticOpenFromJoystick((SDL_Joystick *)joystick);
    if (!haptic) return 0;
    const unsigned features = (unsigned)SDL_HapticQuery(haptic);
    SDL_HapticEffect e;
    if (features & SDL_HAPTIC_CONSTANT) {
        ffb_effect(&e, SDL_HAPTIC_CONSTANT, PSX_JOGCON_FFB_LIFETIME_MS);
        h->constant = SDL_HapticNewEffect(haptic, &e);
    }
    if (features & SDL_HAPTIC_SPRING) {
        ffb_effect(&e, SDL_HAPTIC_SPRING, PSX_JOGCON_FFB_LIFETIME_MS);
        h->spring = SDL_HapticNewEffect(haptic, &e);
    }
    if (h->constant < 0 && h->spring < 0) {
        SDL_HapticClose(haptic);
        return 0;
    }
    h->haptic = haptic;
    return 1;
}

extern "C" void psx_jogcon_haptic_close(PsxJogconHaptic *h) {
    if (!h) return;
    if (h->haptic) {
        SDL_HapticStopAll((SDL_Haptic *)h->haptic);
        SDL_HapticClose((SDL_Haptic *)h->haptic);
    }
    h->haptic = nullptr;
    h->constant = h->spring = -1;
}

static void ffb_play(void *ctx, const PsxJogconFfb *f, uint32_t ms) {
    PsxJogconHaptic *h = (PsxJogconHaptic *)ctx;
    if (!h || !h->haptic || !f) return;
    SDL_Haptic *haptic = (SDL_Haptic *)h->haptic;
    SDL_HapticEffect e;
    if (h->constant >= 0) {
        ffb_effect(&e, SDL_HAPTIC_CONSTANT, ms);
        e.constant.level = f->constant;
        if (f->constant) {
            (void)SDL_HapticUpdateEffect(haptic, h->constant, &e);
            (void)SDL_HapticRunEffect(haptic, h->constant, 1);
        } else {
            (void)SDL_HapticStopEffect(haptic, h->constant);
        }
    }
    if (h->spring >= 0) {
        ffb_effect(&e, SDL_HAPTIC_SPRING, ms);
        e.condition.right_coeff[0] = e.condition.left_coeff[0] = (Sint16)f->spring;
        if (f->spring) {
            (void)SDL_HapticUpdateEffect(haptic, h->spring, &e);
            (void)SDL_HapticRunEffect(haptic, h->spring, 1);
        } else {
            (void)SDL_HapticStopEffect(haptic, h->spring);
        }
    }
}

static void ffb_stop(void *ctx) {
    PsxJogconHaptic *h = (PsxJogconHaptic *)ctx;
    if (h && h->haptic) SDL_HapticStopAll((SDL_Haptic *)h->haptic);
}

extern "C" PsxJogconFfbBackend psx_jogcon_haptic_backend(PsxJogconHaptic *h) {
    PsxJogconFfbBackend b;
    b.ctx = h;
    b.play = ffb_play;
    b.stop = ffb_stop;
    return b;
}
