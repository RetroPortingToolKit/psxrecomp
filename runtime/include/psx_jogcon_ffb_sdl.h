/* psx_jogcon_ffb_sdl.h - SDL haptic backend for psx_jogcon_ffb.h. */
#ifndef PSX_JOGCON_FFB_SDL_H
#define PSX_JOGCON_FFB_SDL_H

#include "psx_jogcon_ffb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxJogconHaptic {
    void *haptic;        /* SDL_Haptic*, NULL when the device has no FFB */
    int constant, spring;/* effect ids, -1 when unsupported */
} PsxJogconHaptic;

/* Open the haptic device behind an SDL joystick (SDL_Joystick*). Devices
 * without constant or spring support (gamepads, virtual joysticks) get none
 * and return 0; 1 when at least one effect was created. */
int  psx_jogcon_haptic_open(PsxJogconHaptic *h, void *sdl_joystick);
void psx_jogcon_haptic_close(PsxJogconHaptic *h);   /* stops and frees */
PsxJogconFfbBackend psx_jogcon_haptic_backend(PsxJogconHaptic *h);

/* Lets other host code (e.g. the in-game overlay's apply_settings) detect
 * the hook below without depending on this header's revision. */
#define PSX_HAS_JOGCON_FFB_PLAYER_SETTING 1

/* Runtime (main.cpp): the player's EXPERIMENTAL wheel force-feedback setting
 * (settings.toml [controller] jogcon_force_feedback, default off), live. A
 * settings path (launcher exit, the in-game menu's apply_settings) calls
 * this with RecompLauncherCSettings.jogcon_force_feedback (recomp-launcher's
 * header; read only behind RECOMP_LAUNCHER_HAS_JOGCON_FORCE_FEEDBACK, so
 * any launcher provider builds) and persists it. */
void psx_jogcon_ffb_set_player_enabled(int on);
int  psx_jogcon_ffb_player_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* PSX_JOGCON_FFB_SDL_H */
