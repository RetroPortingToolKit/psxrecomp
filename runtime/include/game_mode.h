#pragma once
/* game_mode.h -- console "Game Mode" (Steam Deck Game Mode / gamescope).
 *
 * Under gamescope there is no desktop: the launcher window is an extra screen
 * with mouse-style controls, and a windowed game sits in a fullscreen
 * compositor anyway. Game Mode boots straight into the game, fullscreen, once
 * the player's disc is set up (the first run still shows the launcher so the
 * disc can be picked). Pure policy; the probes stay in main.cpp. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxGameModeEnv {
    const char *psx_game_mode;      /* PSX_GAME_MODE: "1" on, "0" off, NULL auto */
    const char *xdg_current_desktop;/* "gamescope" in a gamescope session */
    const char *gamescope_display;  /* GAMESCOPE_WAYLAND_DISPLAY */
    const char *steam_gamepad_ui;   /* SteamGamepadUI=1: launched from Big Picture / Game Mode */
    int         cli_game_mode;      /* --game-mode: 1, --no-game-mode: 0, else -1 */
} PsxGameModeEnv;

/* 1 when Game Mode applies. Precedence: the command line, then
 * PSX_GAME_MODE, then detection (gamescope session or Steam's gamepad UI). */
int psx_game_mode_detect(const PsxGameModeEnv *env, const char **why);

#ifdef __cplusplus
}
#endif
