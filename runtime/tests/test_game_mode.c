/* game_mode: Steam Deck / gamescope detection and its precedence. */
#include "game_mode.h"

#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *w) { printf("  %s  %s\n", c ? "ok  " : "FAIL", w); if (!c) fails++; }

int main(void) {
    PsxGameModeEnv e;
    const char *why;
    memset(&e, 0, sizeof e); e.cli_game_mode = -1;
    ok(!psx_game_mode_detect(&e, &why) && !strcmp(why, "desktop"), "desktop: off");
    e.xdg_current_desktop = "gamescope";
    ok(psx_game_mode_detect(&e, &why), "XDG_CURRENT_DESKTOP=gamescope: on");
    e.xdg_current_desktop = "KDE"; e.gamescope_display = "gamescope-0";
    ok(psx_game_mode_detect(&e, &why), "GAMESCOPE_WAYLAND_DISPLAY: on");
    e.gamescope_display = NULL; e.steam_gamepad_ui = "1";
    ok(psx_game_mode_detect(&e, &why), "SteamGamepadUI=1: on");
    e.steam_gamepad_ui = "0";
    ok(!psx_game_mode_detect(&e, &why), "SteamGamepadUI=0: off");
    e.steam_gamepad_ui = "1"; e.psx_game_mode = "0";
    ok(!psx_game_mode_detect(&e, &why), "PSX_GAME_MODE=0 beats detection");
    e.psx_game_mode = "1"; e.steam_gamepad_ui = NULL;
    ok(psx_game_mode_detect(&e, &why), "PSX_GAME_MODE=1 on a desktop");
    e.cli_game_mode = 0;
    ok(!psx_game_mode_detect(&e, &why), "--no-game-mode beats PSX_GAME_MODE");
    e.cli_game_mode = 1; e.psx_game_mode = "0";
    ok(psx_game_mode_detect(&e, &why) && !strcmp(why, "--game-mode"), "--game-mode wins");
    ok(!psx_game_mode_detect(NULL, &why), "NULL env is safe");
    printf("%s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
