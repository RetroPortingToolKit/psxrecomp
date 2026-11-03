/* psx_menu_host.h -- the runtime's contract with an in-game menu provider.
 *
 * psxrecomp owns this header. A frontend (recomp-ui, recomp-launcher, or any
 * other) implements PsxMenuProvider; the runtime implements PsxMenuHost. The
 * runtime keeps every policy (pause, live settings, mod changes, disc/tray,
 * netplay session start/exit) on its side, so all providers get the same
 * behaviour. Nothing here is specific to one frontend.
 *
 * Selection: no provider is registered by default, so a title's existing UI
 * (recomp-ui) and its pin are unchanged. A title that wants a provider
 * registers it with psx_menu_set_provider() before the game window opens
 * (for example from a static initializer in the title's own code).
 *
 * Threads: attach/event/is_open/set_open run on the main thread; draw runs on
 * the thread that owns the GL context (host_overlay.h P2) with the window
 * framebuffer bound. PsxMenuHost calls may come from either; the runtime
 * applies them on the main thread at the next frame. */
#ifndef PSX_MENU_HOST_H
#define PSX_MENU_HOST_H

#include "host_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_MENU_ABI_VERSION 1
/* PsxMenuHost.default_selection is present (appended, ABI-compatible). */
#define PSX_MENU_HOST_HAS_DEFAULT_SELECTION 1

/* request_exit */
enum {
    PSX_MENU_EXIT_QUIT = 0,         /* leave the game (netplay: back to the lobby) */
    PSX_MENU_EXIT_TO_FRONTEND = 1,  /* end the session, re-enter the frontend loop in-process */
    PSX_MENU_EXIT_RELAUNCH = 2      /* restart this process with its original arguments */
};

/* video_apply returns PSX_VIDEO_RESTART_* bits (host_overlay.h) plus this one:
 * during a netplay match only presentation applies; the rest waits. */
#define PSX_MENU_RESTART_AFTER_MATCH (1u << 31)

/* The shared launcher C ABI (recomp_launcher.h) that recomp-ui and
 * recomp-launcher both speak for the pre-game launcher. Optional: a provider
 * that uses it gets the same game info, mod catalogue and lobby objects the
 * pre-game launcher gets; one that does not ignores these entries. */
struct RecompLauncherCGameInfo;
struct RecompLauncherCSettings;

typedef struct PsxMenuHost {
    int abi_version;                /* PSX_MENU_ABI_VERSION */
    /* Pause the guest and audio (host_overlay.h P1). Returns 0 when refused
     * (netplay): the menu then stays open without pausing. */
    int  (*set_paused)(int paused);
    int  (*netplay_active)(void);
    /* Video settings: current values, and a live apply (P4) that also saves
     * settings.toml. Returns the restart bits of the fields that wait. */
    void     (*video_get)(PsxHostVideoSettings *out);
    unsigned (*video_apply)(const PsxHostVideoSettings *next);
    /* Mod feature switch (option == NULL, value "true"/"false") or option.
     * Saved to mods/state.toml; returns 1 when it applied live (P5), 0 when it
     * waits for the next start. Refused (0, unsaved) during netplay. */
    int  (*mod_set)(const char *package_id, const char *feature_id,
                    const char *option_id, const char *value);
    /* Reload keybinds.ini / input.ini / config.ini after the menu wrote them. */
    void (*reload_bindings)(void);
    /* PSX_MENU_EXIT_*. Applied on the main thread; the menu is closed and its
     * pause dropped before the session ends. */
    void (*request_exit)(int how);
    /* Disc/tray: the runtime's production tray policy (psx_disc_tray.h),
     * shared with its own disc menu and hotkeys; count is 0 without a
     * multi-disc roster or a policy. insert queues the lid cycle for the main
     * thread: 0 = queued, -1 = no such disc, 3 = refused (netplay). */
    int  (*disc_count)(void);
    int  (*disc_mounted)(void);                 /* 1-based, 0 = none */
    const char *(*disc_path)(int disc_1based);
    int  (*disc_insert)(int disc_1based);
    /* Shared launcher ABI helpers (see above); NULL in builds without it. */
    void     (*launcher_game_info)(struct RecompLauncherCGameInfo *out);
    void     (*launcher_settings_get)(struct RecompLauncherCSettings *out);
    unsigned (*launcher_settings_apply)(const struct RecompLauncherCSettings *next);
    const char *assets_dir;          /* the title's assets folder */
    /* 1 when the runtime chose this provider because the title chose none
     * (the UI library's default provider, recomp_ui_psx_menu.h). Such a
     * provider keeps the title's default presentation: it may decline to
     * attach when it would only add a menu the title did not have. */
    int default_selection;
} PsxMenuHost;

typedef struct PsxMenuProvider {
    int abi_version;                /* PSX_MENU_ABI_VERSION */
    const char *name;
    void *ctx;
    /* Once per GL context, with it current. Returns 0 to decline. */
    int  (*attach)(void *ctx, void *sdl_window, void *gl_context, const PsxMenuHost *host);
    /* Every SDL event first; 1 = consumed (the game does not see it). The
     * provider opens itself (its own key/pad binding) from here. */
    int  (*event)(void *ctx, const void *sdl_event);
    int  (*is_open)(void *ctx);
    void (*set_open)(void *ctx, int open);
    /* Draw over the presented frame (P2). */
    void (*draw)(void *ctx, int framebuffer_w, int framebuffer_h);
    /* Before its GL context goes away (session end, rematch), with it current;
     * attach follows for the next context. */
    void (*detach)(void *ctx);
} PsxMenuProvider;

/* Register (or clear, NULL) the in-game menu provider. Main thread, before
 * the game window opens; a provider with another ABI version is refused. */
int  psx_menu_set_provider(const PsxMenuProvider *provider);
const PsxMenuProvider *psx_menu_provider(void);

/* The disc/tray policy publishes itself here (PsxMenuHost disc_* entries). */
typedef struct PsxMenuDiscOps {
    int  (*count)(void);
    int  (*mounted)(void);
    const char *(*path)(int disc_1based);
    int  (*insert)(int disc_1based);
} PsxMenuDiscOps;
void psx_menu_set_disc_ops(const PsxMenuDiscOps *ops);
const PsxMenuDiscOps *psx_menu_disc_ops(void);

#ifdef __cplusplus
}
#endif
#endif
