/* host_overlay.h -- hooks for an optional in-game menu ("overlay provider").
 *
 * Every hook is inert until something registers with it: a title with no
 * overlay provider runs exactly as before. Sections are added by the
 * docs/IN_GAME_OVERLAY.md PR stack (P1 pause, P2 draw, P3 input, ...).
 */
#ifndef PSX_HOST_OVERLAY_H
#define PSX_HOST_OVERLAY_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- P1: host pause --------------------------------------------------------
 * A nesting pause owned by the host (not the guest): while depth > 0 the
 * runtime holds the guest at the vblank present, keeps presenting the last
 * frame and pauses audio. Push refuses (returns 0) while the refusal probe
 * says so -- the runtime installs psx_netplay_active() there, because pausing
 * one peer would stall the session. Main thread only. */
typedef int (*PsxHostPauseRefuseFn)(void);
int  psx_host_pause_push(const char *reason);
void psx_host_pause_pop(void);
int  psx_host_pause_depth(void);
const char *psx_host_pause_reason(void);
void psx_host_pause_set_refuse_probe(PsxHostPauseRefuseFn fn);

/* ---- P2: overlay draw callback ---------------------------------------------
 * Called once per presented frame by the OpenGL backend after the game image
 * and the host OSD layers are composed, before present_shot capture and the
 * swap, with the drawable size in pixels. It runs on the thread that owns the
 * GL context (the render thread when it is on, else the main thread), with the
 * window framebuffer bound. The callback must leave GL state as it found it.
 * Vulkan and the SDL renderer never call it. NULL unregisters.
 *
 * Lifetime contract: the registrant owns ctx. psx_host_overlay_set_draw_cb
 * (replace or NULL) returns only after any draw call that may still be using
 * the previous fn/ctx has finished, so the registrant may free ctx and the GL
 * objects it draws with right after it returns, and must keep them alive
 * until then. Unregister before destroying those objects or the GL context
 * (e.g. on renderer shutdown). Called from inside the callback it does not
 * wait; the current call is then the last one with the old ctx. */
typedef void (*PsxHostOverlayDrawFn)(int width, int height, void *ctx);
void psx_host_overlay_set_draw_cb(PsxHostOverlayDrawFn fn, void *ctx);
int  psx_host_overlay_has_draw(void);
void psx_host_overlay_draw(int width, int height);  /* renderer side */

/* ---- Game HUD layer ---------------------------------------------------------
 * A title's own heads-up display, drawn once per presented frame (real and
 * generated) under the in-game menu, same thread and GL rules as the overlay
 * callback above. OpenGL only; Vulkan and the SDL renderer never invoke it.
 * Replacing or NULL unregistering drains prior calls as for the overlay above;
 * the owner may then release its old context. Registration is session-scoped:
 * activation reset and GL shutdown clear it. Register again on activation. */
void psx_host_set_hud_draw_cb(PsxHostOverlayDrawFn fn, void *ctx);
void psx_host_hud_draw(int width, int height);  /* renderer side */

/* ---- Post-effect live uniforms ------------------------------------------
 * Sets the value of a `uniform <name> ...` line of the loaded post-effect chain
 * ([video] post_shader, postfx_chain.h) from game state, e.g. a light source
 * projected to the screen. Any thread; effective at the next present; the
 * preset default applies until the first call. Up to 16 names. */
void psx_host_set_post_uniform(const char *name, float x, float y, float z, float w);

/* ---- P3: input sink -----------------------------------------------------
 * The sink sees every host SDL event first (main thread), in the normal event
 * drain and in the host pause loop. Returning non-zero consumes the event.
 * While ui_capture is on, local game input reads as released/centred, in
 * netplay too (the peer then simply receives "nothing pressed"), so a menu
 * open over a running session never leaks presses into the game.
 * `event` is an SDL_Event*. NULL unregisters. */
typedef int (*PsxHostInputSinkFn)(const void *event, void *ctx);
void psx_host_set_input_sink(PsxHostInputSinkFn fn, void *ctx);
int  psx_host_input_sink_dispatch(const void *event);
void psx_host_set_ui_capture(int on);
int  psx_host_ui_capture_active(void);

/* ---- P4: live video settings ---------------------------------------------
 * The session's video state an in-game menu (or the debug server's video_set)
 * may change. Screen model, scanlines, smoothing, texture and FMV filtering
 * and the internal resolution are the built-in CRT Filter, Image Filtering
 * and Internal Resolution mods (host_enhancements.h), changed live through
 * their mod options (psx_mod_set_option_live); the internal_resolution and
 * supersample_milli fields here are the session's effective values, for the
 * debug server and the restart bits, and are not player settings.
 * psx_video_apply_live() applies what can change on a running game, saves the
 * player's PC/system fields (fullscreen, renderer, threads, frame generation)
 * to settings.toml, and returns PSX_VIDEO_RESTART_* bits for the fields that
 * only take effect on the next start (compared with boot). Main thread
 * only. */
typedef struct PsxHostVideoSettings {
    int fullscreen;            /* 0 windowed, 1 borderless, 2 exclusive */
    int frame_generation;      /* 0/1 */
    int renderer;              /* restart */
    int internal_resolution;   /* session, live (psx_video_apply_live);
                                * PSX_IR_AUTO = Match display with dynamic
                                * resolution; not saved */
    int supersampling;         /* the title's legacy factor; restart */
    int render_thread;         /* restart */
    int present_thread;        /* restart */
    int supersample_milli;     /* session, live: 1000 off .. 4000; not saved */
} PsxHostVideoSettings;
/* PsxHostVideoSettings no longer carries texture_filter, fmv_filter,
 * scanlines, scanline_strength_pct, present_linear or dynamic_resolution: a
 * menu provider can test this to stay source-compatible with both shapes. */
#define PSX_HOST_VIDEO_SETTINGS_MOD_OWNED_LOOK 1

#define PSX_VIDEO_RESTART_RENDERER    (1u << 0)
#define PSX_VIDEO_RESTART_RESOLUTION  (1u << 1)
#define PSX_VIDEO_RESTART_THREADS     (1u << 2)
/* Netplay is running: live changes beyond presentation (dynamic resolution,
 * frame generation, filters) were saved but not applied; they take effect in
 * the next session. */
#define PSX_VIDEO_RESTART_NETPLAY     (1u << 3)

#define PSX_VIDEO_LIVE_FULLSCREEN     (1u << 0)
/* Bits 1..5 were texture filter, FMV filter, scanlines, linear present and
 * dynamic resolution (now mod options / PSX_IR_AUTO); left unused. */
#define PSX_VIDEO_LIVE_FRAME_GEN      (1u << 6)
/* Internal resolution (Auto included) or supersampling: the scale moves at
 * the next frame. */
#define PSX_VIDEO_LIVE_RESOLUTION     (1u << 7)

/* Pure helpers (unit-tested): which live fields differ, and which restart
 * fields differ from the values the session booted with. */
unsigned psx_host_video_live_changes(const PsxHostVideoSettings *prev,
                                     const PsxHostVideoSettings *next);
unsigned psx_host_video_restart_bits(const PsxHostVideoSettings *boot,
                                     const PsxHostVideoSettings *next);
/* psx_host_video_restart_bits for a backend that may not move the resolution
 * live: when res_live_ok is 0 (backend incapable, or a live change was
 * refused), an internal resolution / supersample that differs from boot keeps
 * PSX_VIDEO_RESTART_RESOLUTION. */
unsigned psx_host_video_restart_bits_backend(const PsxHostVideoSettings *boot,
                                             const PsxHostVideoSettings *next,
                                             int res_live_ok);
/* Runtime side (main.cpp). */
unsigned psx_video_apply_live(const PsxHostVideoSettings *next);
void     psx_video_current(PsxHostVideoSettings *out);

/* Replace this process with a fresh copy started with the same arguments
 * (the in-game menu's quick relaunch for restart-only settings). Returns only
 * on failure. */
void psx_relaunch_self(int argc, char** argv);
/* Windows command-line quoting for one argument (CommandLineToArgvW rules);
 * returns the quoted length, writing at most cap-1 chars plus NUL to out
 * (out may be NULL to measure). Portable, for tests. */
size_t psx_win_cmdline_quote(const char* arg, char* out, size_t cap);
/* All arguments quoted and space-joined; malloc'd (free it), NULL on OOM. */
char* psx_win_cmdline_join(int argc, const char* const* argv);

#ifdef __cplusplus
}
#endif
#endif
