/* GunCon controls in keybinds.ini [guncon] (PS1B-305): the defaults, loading
 * a section with a primary and an alt, a file without the section, and the
 * section surviving a save. argv[1] is a scratch directory. */
#include "psx_keybinds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int ok, const char *what) {
    if (ok) return;
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    char text[16384];
    size_t n;
    if (!f) return 0;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

#define MOUSE(n) ((SDL_Scancode)(512 + (n)))

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: guncon_keybinds_test <scratch-dir>\n"); return 2; }
    char dir[900], ini[1024];
    snprintf(dir, sizeof(dir), "%s/", argv[1]);
    snprintf(ini, sizeof(ini), "%skeybinds.ini", dir);
    uint8_t keys[SDL_NUM_SCANCODES];
    memset(keys, 0, sizeof(keys));

    /* Defaults, before any file. */
    expect(psx_keybinds_guncon_get(PSX_GC_TRIGGER, 0) == MOUSE(1), "trigger defaults to Mouse1");
    expect(psx_keybinds_guncon_get(PSX_GC_A, 0) == MOUSE(3), "A defaults to Mouse3 (right)");
    expect(psx_keybinds_guncon_get(PSX_GC_A, 1) == SDL_SCANCODE_A, "A's alt defaults to the A key");
    expect(psx_keybinds_guncon_get(PSX_GC_B, 0) == MOUSE(2), "B defaults to Mouse2 (middle)");
    expect(psx_keybinds_guncon_get(PSX_GC_B, 1) == SDL_SCANCODE_D, "B's alt defaults to the D key");
    expect(psx_keybinds_guncon_get(PSX_GC_NO_LIGHT, 0) == MOUSE(4), "no light defaults to Mouse4");
    expect(psx_keybinds_guncon_get(PSX_GC_OFFSCREEN_SHOT, 0) == SDL_SCANCODE_W,
           "the off-screen shot defaults to W");
    expect(!strcmp(psx_keybinds_guncon_name(PSX_GC_OFFSCREEN_SHOT), "offscreen_shot"),
           "ini name of the off-screen shot");

    keys[SDL_SCANCODE_W] = 1;
    expect(psx_keybinds_guncon_held(keys, PSX_GC_OFFSCREEN_SHOT), "W holds the off-screen shot");
    expect(!psx_keybinds_guncon_held(keys, PSX_GC_A), "W is not A");
    keys[SDL_SCANCODE_W] = 0;
    keys[SDL_SCANCODE_A] = 1;
    expect(psx_keybinds_guncon_held(keys, PSX_GC_A), "the A key holds GunCon A");
    keys[SDL_SCANCODE_A] = 0;
    keys[SDL_SCANCODE_D] = 1;
    expect(psx_keybinds_guncon_held(keys, PSX_GC_B), "the D key holds GunCon B");
    keys[SDL_SCANCODE_D] = 0;
    expect(!psx_keybinds_guncon_held(keys, PSX_GC_COUNT), "an out-of-range control is never held");
    expect(!psx_keybinds_guncon_held(NULL, PSX_GC_A), "no key state, nothing held");

    /* A [guncon] section: primary and alt, "None", keys in any case. The
     * [player1] section around it still loads. */
    remove(ini);
    write_file(ini,
        "[player1]\n"
        "cross = X\n"
        "[guncon]\n"
        "# a comment inside the section\n"
        "trigger = Space, Mouse1\n"
        "A = Q\n"
        "offscreen_shot = None\n"
        "[player2]\n"
        "cross = K\n");
    psx_keybinds_init(dir);
    expect(psx_keybinds_guncon_get(PSX_GC_TRIGGER, 0) == SDL_SCANCODE_SPACE, "trigger loads Space");
    expect(psx_keybinds_guncon_get(PSX_GC_TRIGGER, 1) == MOUSE(1), "trigger's alt loads Mouse1");
    expect(psx_keybinds_guncon_get(PSX_GC_A, 0) == SDL_SCANCODE_Q, "A loads Q");
    expect(psx_keybinds_guncon_get(PSX_GC_A, 1) == SDL_SCANCODE_UNKNOWN, "A has no alt now");
    expect(psx_keybinds_guncon_get(PSX_GC_OFFSCREEN_SHOT, 0) == SDL_SCANCODE_UNKNOWN,
           "None unbinds the off-screen shot");
    expect(psx_keybinds_guncon_get(PSX_GC_B, 1) == SDL_SCANCODE_D, "a missing key keeps its default");
    expect(psx_keybinds_get_button(1, PSX_KB_CROSS) == SDL_SCANCODE_X, "player1 still loads");
    expect(psx_keybinds_get_button(2, PSX_KB_CROSS) == SDL_SCANCODE_K,
           "player2 after [guncon] still loads");
    keys[SDL_SCANCODE_W] = 1;
    expect(!psx_keybinds_guncon_held(keys, PSX_GC_OFFSCREEN_SHOT), "W no longer shoots off-screen");
    keys[SDL_SCANCODE_W] = 0;

    /* Saved and loaded again: the section round-trips. */
    psx_keybinds_save();
    expect(file_contains(ini, "[guncon]"), "the saved file has [guncon]");
    expect(file_contains(ini, "trigger        = Space, Mouse1"), "trigger saved with its alt");
    expect(file_contains(ini, "offscreen_shot = None"), "the unbound control saved as None");
    psx_keybinds_init(dir);
    expect(psx_keybinds_guncon_get(PSX_GC_TRIGGER, 1) == MOUSE(1), "trigger's alt survives a reload");
    expect(psx_keybinds_guncon_get(PSX_GC_A, 0) == SDL_SCANCODE_Q, "A survives a reload");

    /* A file without [guncon] gives the defaults again. */
    write_file(ini, "[player1]\ncross = X\n");
    psx_keybinds_init(dir);
    expect(psx_keybinds_guncon_get(PSX_GC_TRIGGER, 0) == MOUSE(1), "no section: trigger default");
    expect(psx_keybinds_guncon_get(PSX_GC_A, 1) == SDL_SCANCODE_A, "no section: A's alt default");
    expect(psx_keybinds_guncon_get(PSX_GC_OFFSCREEN_SHOT, 0) == SDL_SCANCODE_W,
           "no section: off-screen shot default");

    /* A new file is written with the section and its defaults. */
    remove(ini);
    psx_keybinds_init(dir);
    expect(file_contains(ini, "[guncon]") && file_contains(ini, "offscreen_shot = W"),
           "a generated file has [guncon] with the defaults");
    expect(file_contains(ini, "a              = Mouse3, A"), "A's default written as Mouse3, A");
    remove(ini);

    if (failures) { fprintf(stderr, "guncon keybinds: %d failure(s)\n", failures); return 1; }
    printf("guncon keybinds: passed\n");
    return 0;
}
