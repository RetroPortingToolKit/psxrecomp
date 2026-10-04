#include "psx_keybinds.h"
#include "host_keymap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;

static void expect_word(const char *label, uint16_t expected, uint16_t actual) {
    if (expected == actual) return;
    fprintf(stderr, "FAIL %s: expected=0x%04X actual=0x%04X\n",
            label, (unsigned)expected, (unsigned)actual);
    failures++;
}

static void expect_scancode(const char *label, SDL_Scancode expected,
                            SDL_Scancode actual) {
    if (expected == actual) return;
    fprintf(stderr, "FAIL %s: expected=%d actual=%d\n",
            label, (int)expected, (int)actual);
    failures++;
}

int main(int argc, char **argv) {
    uint8_t keys[SDL_NUM_SCANCODES];
    memset(keys, 0, sizeof(keys));

    /* With an argument, exercise the same keybinds.ini the runtime loads. */
    if (argc > 1) psx_keybinds_init(argv[1]);

    expect_scancode("start binding", SDL_SCANCODE_RETURN,
                    psx_keybinds_get_button(1, PSX_KB_START));
    expect_scancode("select binding", SDL_SCANCODE_RSHIFT,
                    psx_keybinds_get_button(1, PSX_KB_SELECT));

    expect_word("released", 0xFFFFu, psx_keybinds_pad_word(keys, 1));

    keys[SDL_SCANCODE_RSHIFT] = 1;
    expect_word("select held", 0xFFFEu, psx_keybinds_pad_word(keys, 1));

    keys[SDL_SCANCODE_RETURN] = 1;
    expect_word("select+start first poll", 0xFFF6u,
                psx_keybinds_pad_word(keys, 1));
    expect_word("select+start consecutive poll", 0xFFF6u,
                psx_keybinds_pad_word(keys, 1));

    keys[SDL_SCANCODE_RETURN] = 0;
    expect_word("start released, select remains", 0xFFFEu,
                psx_keybinds_pad_word(keys, 1));
    keys[SDL_SCANCODE_RETURN] = 1;
    expect_word("start re-pressed with select held", 0xFFF6u,
                psx_keybinds_pad_word(keys, 1));

    /* Captured Escape must be swallowed before folding; a primary/alternate
     * binding and a simultaneously held controller action are independent. */
    memset(keys, 0, sizeof(keys));
    /* Rebinding intentionally moves a key from its previous input. The INI
     * loader permits shared sources, so construct this fixture through it. */
    {
#ifdef _WIN32
        if (_mkdir("keyboard_pad_chord_fixture") != 0) return 1;
#else
        if (mkdir("keyboard_pad_chord_fixture", 0755) != 0) return 1;
#endif
        FILE *config = fopen("keyboard_pad_chord_fixture/keybinds.ini", "wb");
        if (!config) return 1;
        fputs("[player1]\ncross = Escape\ncircle = S, Escape\n"
              "[player2]\ncross = Escape\n", config);
        fclose(config);
        psx_keybinds_init("keyboard_pad_chord_fixture/fixture.exe");
        remove("keyboard_pad_chord_fixture/keybinds.ini");
#ifdef _WIN32
        _rmdir("keyboard_pad_chord_fixture");
#else
        rmdir("keyboard_pad_chord_fixture");
#endif
        expect_scancode("shared INI primary Escape", SDL_SCANCODE_ESCAPE,
                        psx_keybinds_get_button(1, PSX_KB_CROSS));
        expect_scancode("shared INI alternate Escape", SDL_SCANCODE_ESCAPE,
                        psx_keybinds_get_button_alt(1, PSX_KB_CIRCLE));
    }
    {
        FILE *config = fopen("keyboard_consumed_host_test.ini", "wb");
        if (!config) return 1;
        fputs("[KeyMap]\nTurbo = Escape\n", config);
        fclose(config);
        host_keymap_load("keyboard_consumed_host_test.ini");
        remove("keyboard_consumed_host_test.ini");
    }
    keys[SDL_SCANCODE_ESCAPE] = 1;
    expect_word("configured Turbo=Escape is normally live", 1,
                (uint16_t)host_keymap_down(HOST_KEYMAP_TURBO, keys, 0));
    psx_keybinds_suppress_control(1, SDL_SCANCODE_ESCAPE);
    expect_word("Escape primary and alternate consumed", 0xFFFFu,
                psx_keybinds_pad_word(keys, 1));
    expect_word("held Escape remains consumed", 0xFFFFu,
                psx_keybinds_pad_word(keys, 1));
    for (int poll = 0; poll < 3; ++poll) {
        uint8_t host_keys[SDL_NUM_SCANCODES];
        psx_keybinds_host_keys(keys, host_keys);
        expect_word("captured Escape does not activate polled Turbo", 0,
                    (uint16_t)host_keymap_down(HOST_KEYMAP_TURBO, host_keys, 0));
        expect_word("host filter preserves physical SDL state", 1, keys[SDL_SCANCODE_ESCAPE]);
        expect_word("simultaneous P2 Escape action stays live", 0xBFFFu,
                    psx_keybinds_pad_word(keys, 2));
    }
    expect_word("controller Cross preserved through source merge", 0xBFFFu,
                (uint16_t)(0xBFFFu & psx_keybinds_pad_word(keys, 1)));
    keys[SDL_SCANCODE_ESCAPE] = 0;
    expect_word("release clears source suppression", 0xFFFFu,
                psx_keybinds_pad_word(keys, 1));
    {
        uint8_t host_keys[SDL_NUM_SCANCODES];
        psx_keybinds_host_keys(keys, host_keys);
        expect_word("released Escape is not Turbo", 0,
                    (uint16_t)host_keymap_down(HOST_KEYMAP_TURBO, host_keys, 0));
    }
    keys[SDL_SCANCODE_ESCAPE] = 1;
    expect_word("fresh Escape press is native", 0x9FFFu,
                psx_keybinds_pad_word(keys, 1));
    {
        uint8_t host_keys[SDL_NUM_SCANCODES];
        psx_keybinds_host_keys(keys, host_keys);
        expect_word("fresh post-release native Escape activates Turbo", 1,
                    (uint16_t)host_keymap_down(HOST_KEYMAP_TURBO, host_keys, 0));
    }
    psx_keybinds_suppress_control(1, PSXKB_MOUSE_SC(SDL_BUTTON_LEFT));
    expect_word("mouse suppression is P1 only", 0,
                (uint16_t)psx_keybinds_control_suppressed(2, PSXKB_MOUSE_SC(SDL_BUTTON_LEFT)));
    psx_keybinds_release_control(1, PSXKB_MOUSE_SC(SDL_BUTTON_LEFT));

    if (failures) {
        fprintf(stderr, "keyboard pad chord: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "keyboard pad chord: passed\n");
    return 0;
}
