/* host_overlay.c -- see host_overlay.h. */
#include "host_overlay.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

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
/* Registered on the main thread, read on the render / present thread. The
 * function and its context are published together as one immutable record
 * behind an atomic pointer (release on store, acquire on load), so a reader
 * can never pair a live function with a stale or NULL context. Records are
 * never freed: registration happens a handful of times per process, and a
 * reader may still hold the previous record when it is replaced. */
typedef struct {
    PsxHostOverlayDrawFn fn;
    void *ctx;
} PsxHostOverlayDraw;

static _Atomic(PsxHostOverlayDraw *) s_draw = NULL;

void psx_host_overlay_set_draw_cb(PsxHostOverlayDrawFn fn, void *ctx) {
    PsxHostOverlayDraw *rec = NULL;
    if (fn) {
        rec = (PsxHostOverlayDraw *)malloc(sizeof(*rec));
        if (!rec) return;
        rec->fn = fn;
        rec->ctx = ctx;
    }
    atomic_store_explicit(&s_draw, rec, memory_order_release);
}

int psx_host_overlay_has_draw(void) {
    return atomic_load_explicit(&s_draw, memory_order_acquire) != NULL;
}

void psx_host_overlay_draw(int width, int height) {
    const PsxHostOverlayDraw *rec = atomic_load_explicit(&s_draw, memory_order_acquire);
    if (rec && rec->fn && width > 0 && height > 0) rec->fn(width, height, rec->ctx);
}

/* ---- P3: input sink -------------------------------------------------------- */
static PsxHostInputSinkFn s_sink_fn = NULL;
static void *s_sink_ctx = NULL;
static volatile int s_ui_capture = 0;

void psx_host_set_input_sink(PsxHostInputSinkFn fn, void *ctx) {
    s_sink_fn = fn;
    s_sink_ctx = ctx;
}

int psx_host_input_sink_dispatch(const void *event) {
    return (s_sink_fn && event) ? (s_sink_fn(event, s_sink_ctx) != 0) : 0;
}

void psx_host_set_ui_capture(int on) { s_ui_capture = on ? 1 : 0; }
int psx_host_ui_capture_active(void) { return s_ui_capture; }

/* ---- P4: live video settings ------------------------------------------------ */
unsigned psx_host_video_live_changes(const PsxHostVideoSettings *a,
                                     const PsxHostVideoSettings *b) {
    unsigned m = 0;
    if (!a || !b) return 0;
    if (a->fullscreen != b->fullscreen) m |= PSX_VIDEO_LIVE_FULLSCREEN;
    if (a->texture_filter != b->texture_filter) m |= PSX_VIDEO_LIVE_TEXTURE_FILTER;
    if (a->fmv_filter != b->fmv_filter) m |= PSX_VIDEO_LIVE_FMV_FILTER;
    if (a->scanlines != b->scanlines || a->scanline_strength_pct != b->scanline_strength_pct)
        m |= PSX_VIDEO_LIVE_SCANLINES;
    if (a->present_linear != b->present_linear) m |= PSX_VIDEO_LIVE_PRESENT_LINEAR;
    if (a->dynamic_resolution != b->dynamic_resolution) m |= PSX_VIDEO_LIVE_DYNRES;
    if (a->frame_generation != b->frame_generation) m |= PSX_VIDEO_LIVE_FRAME_GEN;
    return m;
}

unsigned psx_host_video_restart_bits(const PsxHostVideoSettings *boot,
                                     const PsxHostVideoSettings *next) {
    unsigned m = 0;
    if (!boot || !next) return 0;
    if (boot->renderer != next->renderer) m |= PSX_VIDEO_RESTART_RENDERER;
    if (boot->internal_resolution != next->internal_resolution ||
        boot->supersampling != next->supersampling)
        m |= PSX_VIDEO_RESTART_RESOLUTION;
    if (boot->render_thread != next->render_thread ||
        boot->present_thread != next->present_thread)
        m |= PSX_VIDEO_RESTART_THREADS;
    return m;
}
