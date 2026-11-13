/* The in-game menu's Display and Audio rows (psx_runtime_menu_set_host),
 * driven through the shared recomp-ui model the way the host drives it. */
#include "psx_runtime_menu.h"
#include "psx_disc_tray.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

uint64_t psx_cycle_count;
static int count = 1, mounted = 1, online;
int psx_netplay_active(void) { return online; }
int cdrom_disc_roster_count(void) { return count; }
int cdrom_disc_selected(void) { return mounted; }
const char *cdrom_disc_roster_path(int n) { return n >= 1 && n <= count ? "disc.cue" : NULL; }
int cdrom_disc_select(int n) { mounted = n; return 1; }
void debug_force_cd_reinsert(void) {}
uint32_t psx_rewind_discard_history(void) { return 0; }
void psx_savestate_menu_note_slots_changed(void) {}

static int value[PSX_MENU_SETTING_COUNT] = {0, 2, 100};
static int sets;
static int get(void *ctx, int setting, int *out) {
    assert(ctx == &sets);
    *out = value[setting];
    return 1;
}
static int set(void *ctx, int setting, int v) {
    assert(ctx == &sets);
    /* The menu must never ask for a window size while fullscreen. */
    assert(setting != PSX_MENU_SETTING_WINDOW_SCALE || value[PSX_MENU_SETTING_FULLSCREEN] == 0);
    value[setting] = v;
    ++sets;
    return 1;
}
static int enabled(void *ctx, int setting) {
    (void)ctx;
    return setting != PSX_MENU_SETTING_WINDOW_SCALE || value[PSX_MENU_SETTING_FULLSCREEN] == 0;
}

int main(void) {
    /* A one-disc game without host settings has no menu, as before. */
    psx_runtime_menu_set_host(NULL);
    psx_runtime_menu_init();
    assert(!psx_runtime_menu_open());

    /* With host settings the menu exists for a one-disc game. */
    const PsxRuntimeMenuHost host = {&sets, get, set, enabled};
    psx_runtime_menu_set_host(&host);
    psx_runtime_menu_init();
    assert(psx_runtime_menu_open());
    /* The disc rows are absent: there is nothing to change to. */
    assert(!psx_runtime_menu_select_disc(1));

    /* Sections: Display, Audio, Game. Enter Display: Fullscreen first. */
    psx_runtime_menu_input(PSX_MENU_ACCEPT, 0);
    psx_runtime_menu_input(PSX_MENU_RIGHT, 0);
    assert(value[PSX_MENU_SETTING_FULLSCREEN] == 1 && sets == 1);
    psx_runtime_menu_input(PSX_MENU_RIGHT, 0);
    assert(value[PSX_MENU_SETTING_FULLSCREEN] == 2);
    /* Window size is shown disabled in fullscreen: stepping it changes
     * nothing and never reaches the host. */
    psx_runtime_menu_input(PSX_MENU_DOWN, 0);
    const int before = sets;
    psx_runtime_menu_input(PSX_MENU_RIGHT, 0);
    psx_runtime_menu_input(PSX_MENU_LEFT, 0);
    assert(sets == before && value[PSX_MENU_SETTING_WINDOW_SCALE] == 2);
    psx_runtime_menu_input(PSX_MENU_UP, 0);
    psx_runtime_menu_input(PSX_MENU_LEFT, 0);
    assert(value[PSX_MENU_SETTING_FULLSCREEN] == 1);
    psx_runtime_menu_input(PSX_MENU_LEFT, 0);
    assert(value[PSX_MENU_SETTING_FULLSCREEN] == 0);
    /* Windowed again: DOWN reaches Window size, RIGHT grows it one step. */
    psx_runtime_menu_input(PSX_MENU_DOWN, 0);
    psx_runtime_menu_input(PSX_MENU_RIGHT, 0);
    assert(value[PSX_MENU_SETTING_WINDOW_SCALE] == 3);
    assert(value[PSX_MENU_SETTING_FULLSCREEN] == 0);

    /* Back to the sections, down to Audio: Volume steps by 5. */
    psx_runtime_menu_input(PSX_MENU_BACK, 0);
    psx_runtime_menu_input(PSX_MENU_DOWN, 0);
    psx_runtime_menu_input(PSX_MENU_ACCEPT, 0);
    psx_runtime_menu_input(PSX_MENU_LEFT, 0);
    assert(value[PSX_MENU_SETTING_VOLUME] == 95);
    psx_runtime_menu_input(PSX_MENU_LEFT, 0);
    assert(value[PSX_MENU_SETTING_VOLUME] == 90);
    psx_runtime_menu_close();
    assert(!psx_runtime_menu_is_open());

    /* A multi-disc game keeps its disc rows ahead of the settings. */
    count = 3;
    psx_runtime_menu_init();
    assert(psx_runtime_menu_open());
    assert(psx_runtime_menu_select_disc(3));
    psx_runtime_menu_input(PSX_MENU_DOWN, 0);   /* Disc -> Display */
    psx_runtime_menu_input(PSX_MENU_ACCEPT, 0);
    psx_runtime_menu_input(PSX_MENU_RIGHT, 0);
    assert(value[PSX_MENU_SETTING_FULLSCREEN] == 1);
    psx_runtime_menu_close();

    /* Netplay closes the menu to the player, settings included. */
    online = 1;
    assert(!psx_runtime_menu_open());
    online = 0;

    psx_runtime_menu_set_host(NULL);
    puts("runtime menu settings checks passed");
    return 0;
}
