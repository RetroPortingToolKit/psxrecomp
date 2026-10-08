/* game_mode.c -- see game_mode.h. */
#include "game_mode.h"

#include <string.h>

static int on(const char *s) { return s && s[0] && strcmp(s, "0") != 0; }

int psx_game_mode_detect(const PsxGameModeEnv *e, const char **why) {
    const char *dummy;
    if (!why) why = &dummy;
    if (!e) { *why = "no environment"; return 0; }
    if (e->cli_game_mode >= 0) {
        *why = e->cli_game_mode ? "--game-mode" : "--no-game-mode";
        return e->cli_game_mode;
    }
    if (e->psx_game_mode && e->psx_game_mode[0]) {
        *why = "PSX_GAME_MODE";
        return on(e->psx_game_mode);
    }
    if (e->xdg_current_desktop && strstr(e->xdg_current_desktop, "gamescope")) {
        *why = "gamescope session";
        return 1;
    }
    if (e->gamescope_display && e->gamescope_display[0]) {
        *why = "gamescope session";
        return 1;
    }
    if (on(e->steam_gamepad_ui)) {
        *why = "Steam gamepad UI";
        return 1;
    }
    *why = "desktop";
    return 0;
}
