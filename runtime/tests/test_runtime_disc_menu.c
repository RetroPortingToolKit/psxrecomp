#include "psx_runtime_menu.h"
#include "psx_disc_tray.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

uint64_t psx_cycle_count;
static int count = 4, mounted = 1, online, missing, opens, reinserts, drops, notices, persisted;
int psx_netplay_active(void) { return online; }
int cdrom_disc_roster_count(void) { return count; }
int cdrom_disc_selected(void) { return mounted; }
const char *cdrom_disc_roster_path(int n) { return n >= 1 && n <= count ? "disc.cue" : NULL; }
int cdrom_disc_select(int n) { ++opens; mounted = missing ? 0 : n; return !missing; }
void debug_force_cd_reinsert(void) { ++reinserts; }
uint32_t psx_rewind_discard_history(void) { ++drops; return 7; }
void psx_savestate_menu_note_slots_changed(void) { ++notices; }
static void persist(void *ctx, int n, const char *path) { (void)ctx; assert(path); persisted = n; }
int main(void) {
    psx_disc_tray_set_host_hook(persist, NULL);
    psx_runtime_menu_init();
    /* A provider that attached owns input/pause/rendering exclusively. Even
     * debug requests cannot open a second menu behind it. */
    psx_runtime_menu_set_fallback_enabled(0);
    assert(!psx_runtime_menu_open());
    psx_runtime_menu_set_fallback_enabled(1);
    assert(psx_runtime_menu_open());
    psx_runtime_menu_set_fallback_enabled(0);
    assert(!psx_runtime_menu_is_open());
    psx_runtime_menu_set_fallback_enabled(1);
    assert(psx_runtime_menu_open());
    assert(psx_runtime_menu_selected_disc() == 1);
    /* Real shared-model navigation: enter Disc, browse to 4, then cancel. */
    psx_runtime_menu_input(PSX_MENU_ACCEPT, 0);
    for (int i = 0; i < 3; ++i) psx_runtime_menu_input(PSX_MENU_RIGHT, 0);
    assert(psx_runtime_menu_selected_disc() == 4 && opens == 0);
    psx_runtime_menu_close();
    assert(opens == 0 && mounted == 1);
    assert(psx_runtime_menu_open());
    assert(psx_runtime_menu_selected_disc() == 1);
    assert(psx_runtime_menu_select_disc(4));
    assert(!psx_runtime_menu_select_disc(0) && !psx_runtime_menu_select_disc(5));
    /* Repeat-held accept cannot accidentally insert. */
    psx_runtime_menu_input(PSX_MENU_ACCEPT, 0);
    psx_runtime_menu_input(PSX_MENU_DOWN, 0);
    psx_runtime_menu_input(PSX_MENU_ACCEPT, 1);
    assert(opens == 0);
    psx_runtime_menu_input(PSX_MENU_ACCEPT, 0);
    assert(opens == 1 && mounted == 4 && persisted == 4);
    assert(drops == 1 && notices == 1 && !psx_runtime_menu_is_open());
    assert(psx_runtime_menu_open() && psx_runtime_menu_selected_disc() == 4);
    assert(psx_runtime_menu_insert()); /* Same disc is a reinsert, no rewind drop. */
    assert(opens == 2 && drops == 1);
    assert(psx_runtime_menu_open());
    assert(psx_runtime_menu_select_disc(2));
    missing = 1;
    assert(!psx_runtime_menu_insert() && psx_runtime_menu_is_open());
    assert(mounted == 0 && persisted == 4 && drops == 2);
    missing = 0;
    assert(psx_runtime_menu_insert() && mounted == 2 && persisted == 2);
    online = 1;
    assert(!psx_runtime_menu_open());
    assert(psx_disc_tray_cycle(1, PSX_DISC_TRAY_ORIGIN_DEBUG_SELECT) == PSX_DISC_TRAY_ERR_NETPLAY);
    assert(psx_disc_tray_cycle(0, PSX_DISC_TRAY_ORIGIN_HOTKEY_REINSERT) == PSX_DISC_TRAY_ERR_NETPLAY);
    assert(reinserts == 0 && mounted == 2);
    online = 0;
    psx_disc_tray_auto_poll(0, 0);
    int before = opens;
    assert(psx_disc_tray_auto_poll(3, 0) == -1 && opens == before);
    assert(psx_disc_tray_auto_poll(3, 1) == PSX_DISC_TRAY_OK && mounted == 3);
    assert(psx_disc_tray_auto_poll(3, 1) == -1 && opens == before + 1);
    psx_disc_tray_auto_poll(0, 0);
    missing = 1;
    assert(psx_disc_tray_auto_poll(4, 1) == PSX_DISC_TRAY_ERR_OPEN_FAILED);
    before = opens;
    psx_disc_tray_auto_poll(4, 0);
    assert(psx_disc_tray_auto_poll(4, 1) == -1 && opens == before);
    missing = 0;
    assert(psx_disc_tray_auto_poll(4, 1) == -1); /* Manual fallback, no retry loop. */
    assert(psx_disc_tray_cycle(4, PSX_DISC_TRAY_ORIGIN_MENU) == PSX_DISC_TRAY_OK);
    psx_disc_tray_auto_poll(0, 0);
    assert(psx_disc_tray_auto_poll(4, 1) == -1); /* Already inserted. */
    online = 1;
    assert(psx_disc_tray_auto_poll(1, 1) == PSX_DISC_TRAY_ERR_NETPLAY);
    assert(mounted == 4);
    online = 0;
    psx_disc_tray_auto_poll(0, 0);
    assert(psx_disc_tray_auto_poll(1, 1) == PSX_DISC_TRAY_OK && mounted == 1);
    /* A failed request stays suppressed within a session, but the same
     * request must be attempted after a soft session restart. */
    psx_disc_tray_auto_poll(0, 0);
    missing = 1;
    assert(psx_disc_tray_auto_poll(2, 1) == PSX_DISC_TRAY_ERR_OPEN_FAILED);
    before = opens;
    missing = 0;
    assert(psx_disc_tray_auto_poll(2, 1) == -1 && opens == before);
    psx_disc_tray_reset_session();
    assert(psx_disc_tray_auto_poll(2, 1) == PSX_DISC_TRAY_OK);
    assert(opens == before + 1 && mounted == 2);
    /* Always-on ring wraps and retains the latest result in sequence order. */
    for (int i = 0; i < 40; ++i) psx_disc_tray_cycle(5, PSX_DISC_TRAY_ORIGIN_DEBUG_SELECT);
    const PsxDiscTrayEvent *events;
    uint64_t total = psx_disc_tray_log(&events);
    assert(total > PSX_DISC_TRAY_LOG_CAP);
    for (uint64_t n = total - PSX_DISC_TRAY_LOG_CAP; n < total; ++n) {
        assert(events[n % PSX_DISC_TRAY_LOG_CAP].seq == n);
        assert(events[n % PSX_DISC_TRAY_LOG_CAP].result == PSX_DISC_TRAY_ERR_RANGE);
    }
    count = 1;
    psx_runtime_menu_init();
    assert(!psx_runtime_menu_open());
    assert(psx_disc_tray_cycle(1, PSX_DISC_TRAY_ORIGIN_MENU) == PSX_DISC_TRAY_ERR_NO_ROSTER);
    puts("runtime disc menu and tray: passed");
    return 0;
}
