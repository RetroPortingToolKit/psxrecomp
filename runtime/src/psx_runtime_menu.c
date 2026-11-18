#include "psx_runtime_menu.h"
#include "psx_disc_tray.h"
#include "psx_netplay.h"
#include <stdio.h>
#include <string.h>

#if defined(RECOMP_LAUNCHER)
#include "recomp_runtime_ui.h"
static RecompRuntimeUi *s_ui;
static int s_disc, s_fallback_enabled = 1;
static char s_status[128];
static char s_mounted[64];
static PsxRuntimeMenuHost s_host;
static int s_have_host;
static const char *const s_fullscreen_choices[] = {"Windowed", "Borderless", "Exclusive"};
static const char *const s_scale_choices[] = {"1x", "2x", "3x", "4x"};
static RecompRuntimeUiItem s_disc_items[] = {
    {"media.disc", "Disc", "Selected disc", "Choose a disc, then select Insert disc and resume.",
     RECOMP_RUNTIME_UI_INT, 1, 1, 1, NULL, 0, NULL},
    {"media.insert", "Disc", "Insert disc and resume", "Change the disc while keeping your game running.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL},
};
/* In PSX_MENU_SETTING_* order. */
static const RecompRuntimeUiItem s_setting_items[PSX_MENU_SETTING_COUNT] = {
    {"display.fullscreen", "Display", "Fullscreen", "Windowed, borderless or exclusive fullscreen.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, s_fullscreen_choices, 3, NULL},
    {"display.window_scale", "Display", "Window size", "Window width in steps of 320 pixels; windowed mode only.",
     RECOMP_RUNTIME_UI_CHOICE, 1, 4, 1, s_scale_choices, 4, NULL},
    {"audio.volume", "Audio", "Volume", "Host output volume.",
     RECOMP_RUNTIME_UI_INT, 0, 100, 5, NULL, 0, NULL},
};
static const RecompRuntimeUiItem s_resume_item =
    {"system.resume", "Game", "Resume", "Return to your game without changing discs.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL};
static RecompRuntimeUiItem s_items[2 + PSX_MENU_SETTING_COUNT + 1];
static int setting_of(const RecompRuntimeUiItem *item) {
    for (int i = 0; i < PSX_MENU_SETTING_COUNT; ++i)
        if (!strcmp(item->key, s_setting_items[i].key)) return i;
    return -1;
}
void psx_runtime_menu_set_host(const PsxRuntimeMenuHost *host) {
    s_have_host = host && host->get && host->set;
    if (s_have_host) s_host = *host;
    else memset(&s_host, 0, sizeof s_host);
}
static int enabled(void *ctx, const RecompRuntimeUiItem *item) {
    (void)ctx;
    const int setting = setting_of(item);
    if (setting >= 0)
        return !s_host.enabled || s_host.enabled(s_host.context, setting);
    return !strncmp(item->key, "media.", 6) ?
        psx_disc_tray_disc_count() > 1 && !psx_netplay_active() : 1;
}
static int get_value(void *ctx, const RecompRuntimeUiItem *item, int *out) {
    (void)ctx;
    const int setting = setting_of(item);
    if (setting >= 0) return s_host.get(s_host.context, setting, out);
    *out = s_disc; return 1;
}
int psx_runtime_menu_select_disc(int disc) {
    if (!psx_runtime_menu_is_open() || disc < 1 ||
        disc > psx_disc_tray_disc_count() || psx_netplay_active()) return 0;
    s_disc = disc;
    return 1;
}
static int set_value(void *ctx, const RecompRuntimeUiItem *item, int value) {
    (void)ctx;
    const int setting = setting_of(item);
    if (setting >= 0) {
        if (!enabled(ctx, item)) return 0;
        return s_host.set(s_host.context, setting, value);
    }
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
    s_fallback_enabled = 1;
    const int discs = psx_disc_tray_disc_count() >= 2;
    if (!discs && !s_have_host) return;
    size_t n = 0;
    if (discs) {
        s_disc_items[0].maximum = psx_disc_tray_disc_count();
        s_items[n++] = s_disc_items[0];
        s_items[n++] = s_disc_items[1];
    }
    if (s_have_host)
        for (int i = 0; i < PSX_MENU_SETTING_COUNT; ++i) s_items[n++] = s_setting_items[i];
    s_items[n++] = s_resume_item;
    RecompRuntimeUiConfig config = {0};
    config.title = "Game menu";
    config.subtitle = s_mounted;
    config.theme = "psx";
    config.accept_label = "Enter / Cross";
    config.back_label = "Esc / Circle";
    config.items = s_items;
    config.item_count = n;
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;
    config.callbacks.run_action = action;
    config.callbacks.is_enabled = enabled;
    s_ui = recomp_runtime_ui_create(&config);
}
int psx_runtime_menu_open(void) {
    if (!s_ui || !s_fallback_enabled || psx_netplay_active()) return 0;
    s_disc = psx_disc_tray_mounted_disc();
    if (psx_disc_tray_disc_count() < 2) snprintf(s_mounted, sizeof s_mounted, "Paused");
    else if (s_disc > 0) snprintf(s_mounted, sizeof s_mounted, "Mounted: Disc %d", s_disc);
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
void psx_runtime_menu_set_fallback_enabled(int enabled) {
    s_fallback_enabled = enabled != 0;
    if (!enabled) psx_runtime_menu_close();
}
int psx_runtime_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    static uint32_t image[640 * 480];
    if (!psx_runtime_menu_is_open()) return 0;
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
void psx_runtime_menu_set_fallback_enabled(int enabled) { (void)enabled; }
int psx_runtime_menu_overlay_image(const uint32_t **p, int *w, int *h) {
    (void)p; (void)w; (void)h; return 0;
}
void psx_runtime_menu_set_host(const PsxRuntimeMenuHost *host) { (void)host; }
#endif
