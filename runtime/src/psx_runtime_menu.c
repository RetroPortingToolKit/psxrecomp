#include "psx_runtime_menu.h"
#include "psx_disc_tray.h"
#include "psx_netplay.h"
#include <stdio.h>
#include <string.h>

#if defined(RECOMP_LAUNCHER)
#include "recomp_runtime_ui.h"
static RecompRuntimeUi *s_ui;
static int s_disc, s_modern;
static char s_status[128];
static char s_mounted[64];
static RecompRuntimeUiItem s_items[] = {
    {"media.disc", "Disc", "Selected disc", "Choose a disc, then select Insert disc and resume.",
     RECOMP_RUNTIME_UI_INT, 1, 1, 1, NULL, 0, NULL},
    {"media.insert", "Disc", "Insert disc and resume", "Change the disc while keeping your game running.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL},
    {"system.resume", "Game", "Resume", "Return to your game without changing discs.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL},
};
static int enabled(void *ctx, const RecompRuntimeUiItem *item) {
    (void)ctx;
    return !strncmp(item->key, "media.", 6) ?
        psx_disc_tray_disc_count() > 1 && !psx_netplay_active() : 1;
}
static int get_value(void *ctx, const RecompRuntimeUiItem *item, int *out) {
    (void)ctx; (void)item; *out = s_disc; return 1;
}
int psx_runtime_menu_select_disc(int disc) {
    if (!psx_runtime_menu_is_open() || disc < 1 ||
        disc > psx_disc_tray_disc_count() || psx_netplay_active()) return 0;
    s_disc = disc;
    return 1;
}
static int set_value(void *ctx, const RecompRuntimeUiItem *item, int value) {
    (void)ctx; (void)item;
    return psx_runtime_menu_select_disc(value);
}
int psx_runtime_menu_insert(void) {
    if (!psx_runtime_menu_is_open()) return 0;
    int result = psx_disc_tray_cycle(s_disc, PSX_DISC_TRAY_ORIGIN_MENU);
    if (result != PSX_DISC_TRAY_OK) {
        if (!psx_disc_tray_mounted_disc())
            snprintf(s_mounted, sizeof s_mounted, "No disc inserted");
        snprintf(s_status, sizeof s_status, "%s", result == PSX_DISC_TRAY_ERR_OPEN_FAILED ?
            "Could not open that disc. Check its image files and try again." :
            "Disc changes are unavailable in this game mode.");
        recomp_runtime_ui_set_status(s_ui, s_status);
        return 0;
    }
    recomp_runtime_ui_close(s_ui);
    return 1;
}
static int action(void *ctx, const RecompRuntimeUiItem *item) {
    (void)ctx;
    if (!strcmp(item->key, "media.insert")) {
        psx_runtime_menu_insert();
    } else {
        recomp_runtime_ui_close(s_ui);
    }
    return 0; /* Keep the specific error instead of the generic Done status. */
}
void psx_runtime_menu_init(void) {
    if (s_ui) recomp_runtime_ui_destroy(s_ui);
    s_ui = NULL;
    s_disc = 0;
    if (psx_disc_tray_disc_count() < 2) return;
    s_items[0].maximum = psx_disc_tray_disc_count();
    RecompRuntimeUiConfig config = {0};
    config.title = "Game menu";
    config.subtitle = s_mounted;
    config.theme = "psx";
    config.accept_label = "Enter / Cross";
    config.back_label = "Esc / Circle";
    config.items = s_items;
    config.item_count = sizeof s_items / sizeof s_items[0];
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;
    config.callbacks.run_action = action;
    config.callbacks.is_enabled = enabled;
    s_ui = recomp_runtime_ui_create(&config);
}
int psx_runtime_menu_open(void) {
    if (!s_ui || psx_netplay_active()) return 0;
    s_disc = psx_disc_tray_mounted_disc();
    if (s_disc > 0) snprintf(s_mounted, sizeof s_mounted, "Mounted: Disc %d", s_disc);
    else snprintf(s_mounted, sizeof s_mounted, "No disc inserted");
    if (s_disc < 1) s_disc = 1;
    recomp_runtime_ui_open(s_ui);
    recomp_runtime_ui_set_status(s_ui, NULL);
    return 1;
}
void psx_runtime_menu_close(void) { recomp_runtime_ui_close(s_ui); }
int psx_runtime_menu_is_open(void) { return recomp_runtime_ui_is_open(s_ui); }
void psx_runtime_menu_input(int input, int repeat) {
    if (input >= PSX_MENU_TOGGLE && input <= PSX_MENU_ACCEPT)
        recomp_runtime_ui_handle_input(s_ui, (RecompRuntimeUiInput)input, 1, repeat);
}
int psx_runtime_menu_selected_disc(void) { return s_disc; }
RecompRuntimeUi *psx_runtime_menu_model(void) { return s_ui; }
void psx_runtime_menu_set_modern(int active) { s_modern = active; }
int psx_runtime_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    static uint32_t image[640 * 480];
    if (!psx_runtime_menu_is_open() || s_modern) return 0;
    /* Compatibility presentation for hosts without an active ImGui backend. */
    for (int i = 0; i < 640 * 480; ++i) image[i] = 0xff10151c;
    recomp_runtime_ui_render_argb8888(s_ui, image, 640, 480, 640 * 4);
    *pixels = image; *w = 640; *h = 480;
    return 1;
}
#else
void psx_runtime_menu_init(void) {}
int psx_runtime_menu_open(void) { return 0; }
void psx_runtime_menu_close(void) {}
int psx_runtime_menu_is_open(void) { return 0; }
void psx_runtime_menu_input(int input, int repeat) { (void)input; (void)repeat; }
int psx_runtime_menu_selected_disc(void) { return 0; }
int psx_runtime_menu_select_disc(int disc) { (void)disc; return 0; }
int psx_runtime_menu_insert(void) { return 0; }
struct RecompRuntimeUi *psx_runtime_menu_model(void) { return NULL; }
void psx_runtime_menu_set_modern(int active) { (void)active; }
int psx_runtime_menu_overlay_image(const uint32_t **p, int *w, int *h) {
    (void)p; (void)w; (void)h; return 0;
}
#endif
