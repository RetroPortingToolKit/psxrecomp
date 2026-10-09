/* host_overlay.h -- hooks for an optional in-game menu ("overlay provider").
 *
 * Every hook is inert until something registers with it: a title with no
 * overlay provider runs exactly as before. Sections are added by the
 * docs/IN_GAME_OVERLAY.md PR stack (P1 pause, P2 draw, P3 input, ...).
 */
#ifndef PSX_HOST_OVERLAY_H
#define PSX_HOST_OVERLAY_H

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
 * Vulkan and the SDL renderer never call it. NULL unregisters. */
typedef void (*PsxHostOverlayDrawFn)(int width, int height, void *ctx);
void psx_host_overlay_set_draw_cb(PsxHostOverlayDrawFn fn, void *ctx);
int  psx_host_overlay_has_draw(void);
void psx_host_overlay_draw(int width, int height);  /* renderer side */

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
 * The [video] keys an in-game menu may change, in settings.toml terms.
 * psx_video_apply_live() applies what can change on a running game, saves
 * every field to settings.toml, and returns PSX_VIDEO_RESTART_* bits for the
 * fields that only take effect on the next start (compared with boot). Main
 * thread only. */
typedef struct PsxHostVideoSettings {
    int fullscreen;            /* 0 windowed, 1 borderless, 2 exclusive */
    int texture_filter;        /* 0 nearest, 1 bilinear */
    int fmv_filter;            /* VIDEO_FMV_FILTER_* (config value) */
    int scanlines;             /* 0/1 */
    int scanline_strength_pct; /* 0..100 */
    int present_linear;        /* [video] antialiasing (linear present) 0/1 */
    int dynamic_resolution;    /* 0/1 */
    int frame_generation;      /* 0/1 */
    int renderer;              /* restart */
    int internal_resolution;   /* restart */
    int supersampling;         /* restart */
    int render_thread;         /* restart */
    int present_thread;        /* restart */
} PsxHostVideoSettings;

#define PSX_VIDEO_RESTART_RENDERER    (1u << 0)
#define PSX_VIDEO_RESTART_RESOLUTION  (1u << 1)
#define PSX_VIDEO_RESTART_THREADS     (1u << 2)
/* Netplay is running: live changes beyond presentation (dynamic resolution,
 * frame generation, filters) were saved but not applied; they take effect in
 * the next session. */
#define PSX_VIDEO_RESTART_NETPLAY     (1u << 3)

#define PSX_VIDEO_LIVE_FULLSCREEN     (1u << 0)
#define PSX_VIDEO_LIVE_TEXTURE_FILTER (1u << 1)
#define PSX_VIDEO_LIVE_FMV_FILTER     (1u << 2)
#define PSX_VIDEO_LIVE_SCANLINES      (1u << 3)
#define PSX_VIDEO_LIVE_PRESENT_LINEAR (1u << 4)
#define PSX_VIDEO_LIVE_DYNRES         (1u << 5)
#define PSX_VIDEO_LIVE_FRAME_GEN      (1u << 6)

/* Pure helpers (unit-tested): which live fields differ, and which restart
 * fields differ from the values the session booted with. */
unsigned psx_host_video_live_changes(const PsxHostVideoSettings *prev,
                                     const PsxHostVideoSettings *next);
unsigned psx_host_video_restart_bits(const PsxHostVideoSettings *boot,
                                     const PsxHostVideoSettings *next);
/* Runtime side (main.cpp). */
unsigned psx_video_apply_live(const PsxHostVideoSettings *next);
void     psx_video_current(PsxHostVideoSettings *out);

#ifdef __cplusplus
}
#endif
#endif
