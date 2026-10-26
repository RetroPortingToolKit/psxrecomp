/* Explicit None is a saved choice, including an entirely unbound player. */
#include "psx_keybinds.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
static Uint32 test_mouse_state(void *x, void *y) { (void)x; (void)y; return 0; }
#define SDL_GetMouseState test_mouse_state
#include "../src/psx_keybinds.c"
#undef SDL_GetMouseState

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static void check_unbound(void) {
    uint8_t keys[SDL_NUM_SCANCODES];
    uint8_t axes[4] = {128,128,128,128};
    memset(keys, 1, sizeof keys);
    CHECK(psx_keybinds_pad_word(keys, 1) != 0xFFFF);
    CHECK(psx_keybinds_pad_word(keys, 2) == 0xFFFF);
    CHECK(psx_keybinds_pad_word(keys, 3) != 0xFFFF);
    CHECK(!psx_keybinds_dpad_active(keys, 2));
    psx_keybinds_sticks(keys, 2, axes);
    CHECK(axes[0] == 128 && axes[1] == 128 && axes[2] == 128 && axes[3] == 128);
    for (int i = 0; i < PSX_KB_COUNT; ++i) {
        CHECK(psx_keybinds_get_button(2, i) == SDL_SCANCODE_UNKNOWN);
        CHECK(psx_keybinds_get_button_alt(2, i) == SDL_SCANCODE_UNKNOWN);
    }
}
int main(void) {
#ifdef _WIN32
    if (_mkdir("keyboard_unbound_fixture") != 0) return 1;
#else
    if (mkdir("keyboard_unbound_fixture", 0755) != 0) return 1;
#endif
    /* Missing file/sections keep the existing defaults. */
    psx_keybinds_init("keyboard_unbound_fixture/fixture.exe");
    CHECK(psx_keybinds_get_button(2, PSX_KB_CROSS) == SDL_SCANCODE_X);
    FILE *config = fopen("keyboard_unbound_fixture/keybinds.ini", "wb");
    if (!config) return 1;
    fputs("[player2]\n", config);
    for (int i = 0; i < PSX_KB_COUNT; ++i)
        fprintf(config, "%s = None\n", psx_keybinds_button_name(i));
    fclose(config);
    psx_keybinds_init("keyboard_unbound_fixture/fixture.exe");
    check_unbound();
    psx_keybinds_save();
    /* Reload must change the live defaults back to the saved unbound slot. */
    psx_keybinds_reset_player(2);
    CHECK(psx_keybinds_get_button(2, PSX_KB_CROSS) == SDL_SCANCODE_X);
    psx_keybinds_init("keyboard_unbound_fixture/fixture.exe");
    check_unbound();
    /* An alternate-only binding is valid; it must not restore primaries. */
    config = fopen("keyboard_unbound_fixture/keybinds.ini", "ab");
    if (!config) return 1;
    fputs("[player2]\ncross = None, X\n", config);
    fclose(config);
    psx_keybinds_init("keyboard_unbound_fixture/fixture.exe");
    CHECK(psx_keybinds_get_button(2, PSX_KB_CROSS) == SDL_SCANCODE_UNKNOWN);
    CHECK(psx_keybinds_get_button_alt(2, PSX_KB_CROSS) == SDL_SCANCODE_X);
    psx_keybinds_reset_player(2);
    CHECK(psx_keybinds_get_button(2, PSX_KB_CROSS) == SDL_SCANCODE_X);
    CHECK(psx_keybinds_get_button_alt(2, PSX_KB_CROSS) == SDL_SCANCODE_UNKNOWN);
    remove("keyboard_unbound_fixture/keybinds.ini");
#ifdef _WIN32
    _rmdir("keyboard_unbound_fixture");
#else
    rmdir("keyboard_unbound_fixture");
#endif
    if (!failures) puts("Explicit None, save/reload, alternate-only and default/reset bindings passed");
    return failures ? 1 : 0;
}
