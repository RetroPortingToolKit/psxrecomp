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

#ifdef __cplusplus
}
#endif
#endif
