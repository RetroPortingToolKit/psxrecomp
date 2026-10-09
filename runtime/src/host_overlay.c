/* host_overlay.c -- see host_overlay.h. */
#include "host_overlay.h"

#include <stddef.h>

/* ---- P1: host pause ------------------------------------------------------ */
#define PSX_HOST_PAUSE_MAX 8
static int s_pause_depth = 0;
static const char *s_pause_reason[PSX_HOST_PAUSE_MAX];
static PsxHostPauseRefuseFn s_pause_refuse = NULL;

int psx_host_pause_push(const char *reason) {
    if (s_pause_refuse && s_pause_refuse()) return 0;
    if (s_pause_depth >= PSX_HOST_PAUSE_MAX) return 0;
    s_pause_reason[s_pause_depth++] = reason ? reason : "host";
    return 1;
}

void psx_host_pause_pop(void) {
    if (s_pause_depth > 0) s_pause_reason[--s_pause_depth] = NULL;
}

int psx_host_pause_depth(void) { return s_pause_depth; }

const char *psx_host_pause_reason(void) {
    return s_pause_depth > 0 ? s_pause_reason[s_pause_depth - 1] : NULL;
}

void psx_host_pause_set_refuse_probe(PsxHostPauseRefuseFn fn) { s_pause_refuse = fn; }

/* ---- P2: overlay draw callback -------------------------------------------- */
/* Written on the main thread before the overlay can open, read on the GL
 * thread; a pointer-sized store, and registration happens once at startup. */
static PsxHostOverlayDrawFn volatile s_draw_fn = NULL;
static void *volatile s_draw_ctx = NULL;

void psx_host_overlay_set_draw_cb(PsxHostOverlayDrawFn fn, void *ctx) {
    s_draw_ctx = ctx;
    s_draw_fn = fn;
}

int psx_host_overlay_has_draw(void) { return s_draw_fn != NULL; }

void psx_host_overlay_draw(int width, int height) {
    PsxHostOverlayDrawFn fn = s_draw_fn;
    if (fn && width > 0 && height > 0) fn(width, height, s_draw_ctx);
}
