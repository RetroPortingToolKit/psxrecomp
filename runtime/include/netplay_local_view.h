#ifndef NETPLAY_LOCAL_VIEW_H
#define NETPLAY_LOCAL_VIEW_H

#include <stdint.h>
#include <string.h>

/* Host-only record of the display sub-rectangle a title asked the present
 * path to show for the local netplay seat (psx_netplay_present_local_view).
 * Never serialized and never visible to the guest: every peer still renders
 * and digests the identical full frame. A request covers the simulation tick
 * it was made on plus PSX_NETPLAY_LOCAL_VIEW_HOLD later ticks, so a title only
 * has to renew it while its multi-view screen is up; menus fall back to the
 * full frame by themselves. Rollback resimulation repeats ticks up to
 * PSX_NETPLAY_LOCAL_VIEW_REPLAY behind the newest request; those never move
 * the stamp backwards. A larger step back (a load) restarts it. */
#define PSX_NETPLAY_LOCAL_VIEW_HOLD 8u
#define PSX_NETPLAY_LOCAL_VIEW_REPLAY 64u

typedef struct PsxNetplayLocalView {
    uint32_t x, y, w, h;
    uint32_t tick;
    uint8_t valid;
} PsxNetplayLocalView;

static inline void psx_netplay_local_view_reset(PsxNetplayLocalView *view)
{
    if (view) memset(view, 0, sizeof(*view));
}

/* Rejects empty rectangles and anything outside 1024x512 VRAM space. */
static inline int psx_netplay_local_view_set(PsxNetplayLocalView *view,
                                             uint32_t tick, uint32_t x,
                                             uint32_t y, uint32_t w,
                                             uint32_t h)
{
    if (!view || w == 0u || h == 0u || x >= 1024u || y >= 512u ||
        w > 1024u - x || h > 512u - y)
        return 0;
    if (view->valid && (int32_t)(tick - view->tick) < 0 &&
        view->tick - tick <= PSX_NETPLAY_LOCAL_VIEW_REPLAY)
        tick = view->tick;
    view->x = x;
    view->y = y;
    view->w = w;
    view->h = h;
    view->tick = tick;
    view->valid = 1u;
    return 1;
}

/* 1 and the rectangle when a request is still current at `now` and fits a
 * display of display_w x display_h; 0 otherwise. */
static inline int psx_netplay_local_view_get(const PsxNetplayLocalView *view,
                                             uint32_t now, uint32_t display_w,
                                             uint32_t display_h, uint32_t *x,
                                             uint32_t *y, uint32_t *w,
                                             uint32_t *h)
{
    if (!view || !view->valid)
        return 0;
    const int32_t age = (int32_t)(now - view->tick);
    if (age < 0 ? view->tick - now > PSX_NETPLAY_LOCAL_VIEW_REPLAY
                : (uint32_t)age > PSX_NETPLAY_LOCAL_VIEW_HOLD)
        return 0;
    if (view->x + view->w > display_w || view->y + view->h > display_h)
        return 0;
    if (x) *x = view->x;
    if (y) *y = view->y;
    if (w) *w = view->w;
    if (h) *h = view->h;
    return 1;
}

/* Load shedding for psx_mod_render_local_view. A peer's own view is
 * presentation only, so when this peer falls behind the others' inputs it
 * goes first, before the simulation itself slips further (a slow peer forces
 * deeper rollbacks on everyone). "Behind" is remote_lead - input delay: the
 * newest remote input is normally about D ticks ahead of the simulation.
 * Shedding starts at PSX_NETPLAY_LOCAL_VIEW_SHED_BEHIND ticks behind and
 * lasts at least PSX_NETPLAY_LOCAL_VIEW_SHED_HOLD ticks and until the peer is
 * within PSX_NETPLAY_LOCAL_VIEW_SHED_CAUGHT_UP ticks again, so the view does
 * not flicker between the own view and the fallback every other frame. */
#define PSX_NETPLAY_LOCAL_VIEW_SHED_BEHIND 4
#define PSX_NETPLAY_LOCAL_VIEW_SHED_CAUGHT_UP 1
#define PSX_NETPLAY_LOCAL_VIEW_SHED_HOLD 60u

typedef struct PsxNetplayLocalViewShed {
    uint32_t until;
    uint8_t on;
} PsxNetplayLocalViewShed;

static inline int psx_netplay_local_view_shed_step(PsxNetplayLocalViewShed *s,
                                                   uint32_t sim,
                                                   int remote_lead, int delay)
{
    const int behind = remote_lead - (delay > 0 ? delay : 0);
    if (!s)
        return 0;
    if (behind >= PSX_NETPLAY_LOCAL_VIEW_SHED_BEHIND) {
        s->on = 1u;
        s->until = sim + PSX_NETPLAY_LOCAL_VIEW_SHED_HOLD;
    } else if (s->on && (int32_t)(sim - s->until) >= 0 &&
               behind <= PSX_NETPLAY_LOCAL_VIEW_SHED_CAUGHT_UP) {
        s->on = 0u;
    } else if (s->on &&
               (uint32_t)(s->until - sim) > 2u * PSX_NETPLAY_LOCAL_VIEW_SHED_HOLD) {
        s->until = sim + PSX_NETPLAY_LOCAL_VIEW_SHED_HOLD; /* stepped back (load) */
    }
    return s->on;
}

#endif
