/* Exercise the real binding fold with a controlled physical mouse poll.
 * The adapter is tested separately; no production polling hook is added. */
#include "psx_keybinds.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
static Uint32 mouse_buttons;
static Uint32 test_mouse_state(void *x, void *y) { (void)x; (void)y; return mouse_buttons; }
#define SDL_GetMouseState test_mouse_state
#include "../src/psx_keybinds.c"
#undef SDL_GetMouseState

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    uint8_t keys[SDL_NUM_SCANCODES] = {0};
    const SDL_Scancode left = PSXKB_MOUSE_SC(SDL_BUTTON_LEFT);
    psx_keybinds_reset_player(1); psx_keybinds_reset_player(2);
    /* Rebinding moves an assigned source; duplicate sources are a valid INI
     * state. Load that state without changing the real rebinding behavior. */
#ifdef _WIN32
    CHECK(_mkdir("mouse_binding_suppression_fixture") == 0);
#else
    CHECK(mkdir("mouse_binding_suppression_fixture", 0755) == 0);
#endif
    FILE *config = fopen("mouse_binding_suppression_fixture/keybinds.ini", "wb");
    CHECK(config != NULL);
    fputs("[player1]\ncross = Mouse1\ncircle = S, Mouse1\n"
          "rs_right = Mouse1\nr1 = Mouse3\n[player2]\ncross = Mouse1\n", config);
    fclose(config);
    psx_keybinds_init("mouse_binding_suppression_fixture/fixture.exe");
    remove("mouse_binding_suppression_fixture/keybinds.ini");
#ifdef _WIN32
    _rmdir("mouse_binding_suppression_fixture");
#else
    rmdir("mouse_binding_suppression_fixture");
#endif
    CHECK(psx_keybinds_get_button(1, PSX_KB_CROSS) == left);
    CHECK(psx_keybinds_get_button_alt(1, PSX_KB_CIRCLE) == left);
    CHECK(psx_keybinds_get_button(1, PSX_KB_RS_RIGHT) == left);
    mouse_buttons = SDL_BUTTON(SDL_BUTTON_LEFT);
    CHECK(psx_keybinds_pad_word(keys, 1) == 0x9FFF);
    psx_keybinds_suppress_control(1, left);
    for (int i = 0; i < 3; ++i) {
        uint8_t axes[4] = {128,128,128,128};
        CHECK(psx_keybinds_pad_word(keys, 1) == 0xFFFF);
        CHECK((0xBFFF & psx_keybinds_pad_word(keys, 1)) == 0xBFFF);
        psx_keybinds_sticks(keys, 1, axes);
        CHECK(axes[0] == 128 && axes[1] == 128 && axes[2] == 128 && axes[3] == 128);
        CHECK(psx_keybinds_pad_word(keys, 2) == 0xBFFF);
    }
    mouse_buttons = 0;
    CHECK(psx_keybinds_pad_word(keys, 1) == 0xFFFF);
    CHECK(!psx_keybinds_control_suppressed(1, left));
    mouse_buttons = SDL_BUTTON(SDL_BUTTON_LEFT);
    CHECK(psx_keybinds_pad_word(keys, 1) == 0x9FFF);
    uint8_t axes[4] = {128,128,128,128};
    psx_keybinds_sticks(keys, 1, axes); CHECK(axes[2] == 255);
    // Rapid owned re-press consumes LEFT again, while independent native
    // keyboard and RIGHT mouse bindings remain live before controller merging.
    psx_keybinds_suppress_control(1, left);
    keys[SDL_SCANCODE_S] = 1;
    mouse_buttons |= SDL_BUTTON(SDL_BUTTON_RIGHT);
    CHECK(psx_keybinds_pad_word(keys, 1) == 0xD7FF);
    CHECK((0xBFFF & psx_keybinds_pad_word(keys, 1)) == 0x97FF);
    // P2 retains both LEFT/Cross and its native S/Circle binding.
    CHECK(psx_keybinds_pad_word(keys, 2) == 0x9FFF);
    // Every poll starts with the new native report. The fold preserves axes
    // already supplied by a controller; it does not erase the previous query.
    axes[0] = axes[1] = axes[2] = axes[3] = 128;
    psx_keybinds_sticks(keys, 1, axes);CHECK(axes[2] == 128 && axes[3] == 128);
    axes[0] = 90; axes[1] = 160; axes[2] = 99; axes[3] = 77;
    psx_keybinds_sticks(keys, 1, axes);
    CHECK(axes[0] == 90 && axes[1] == 160 && axes[2] == 99 && axes[3] == 77);
    keys[SDL_SCANCODE_S] = 0;
    mouse_buttons = SDL_BUTTON(SDL_BUTTON_RIGHT);
    psx_keybinds_release_control(1, left);
    CHECK(psx_keybinds_pad_word(keys, 1) == 0xF7FF);
    CHECK(psx_keybinds_pad_word(keys, 2) == 0xFFFF);
    CHECK(!psx_keybinds_control_suppressed(1, left));
    mouse_buttons = 0;
    CHECK(psx_keybinds_pad_word(keys, 1) == 0xFFFF);
    puts("LEFT activation suppression survives polls/repress, preserves ordinary bindings/P2/controller, and releases");
    return 0;
}
