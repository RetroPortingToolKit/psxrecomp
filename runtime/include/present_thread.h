/* present_thread.h — opt-in present thread under the render thread
 * ([video] present_thread, docs/RENDER_THREAD.md "Present thread").
 *
 * The thread that composes frames (the render thread, or the emulation thread
 * while it holds the context at a sync point) draws each presented frame into
 * an offscreen slot instead of the window, then hands the slot over; the
 * present thread owns a second graphics context on the same window and does
 * the final copy and the swap. The swap's wait on the window compositor then
 * lands on the present thread, not on rendering.
 *
 * This core is backend-neutral: a ring of `slots` numbered slots, each FREE,
 * COMPOSING (the producer's current slot, exactly one), QUEUED or PRESENTING.
 * Queued slots are presented in submission order. The producer blocks in
 * pt_submit() only while every other slot is queued or presenting (the
 * display is the bottleneck; that wait is the swap's wait, moved).
 *
 * Fences are opaque: `ready` travels with a submitted slot to present();
 * present() returns a `done` token (e.g. a fence after its copy) that the
 * producer receives when it next takes that slot, before drawing into it.
 *
 * Producer calls (pt_current / pt_submit) may come from different threads
 * over time, but never concurrently (the context hand-off orders them). */
#ifndef PSX_PRESENT_THREAD_H
#define PSX_PRESENT_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PT_MAX_SLOTS 4

/* Present thread: make the present context current (current != 0, return 0 on
 * failure: pt_start then fails) or release it (current == 0). */
typedef int   (*PtCtxFn)(void *user, int current);
/* Present thread: show `slot` (wait for `ready`, copy, swap). Returns the
 * slot's done token (may be NULL). */
typedef void *(*PtPresentFn)(void *user, int slot, void *ready);
/* Present thread, after the last present at stop or for a token nobody will
 * collect: dispose of a done token. Optional. */
typedef void  (*PtDisposeFn)(void *user, void *token);

typedef struct PtConfig {
    int         slots;     /* 2..PT_MAX_SLOTS */
    PtCtxFn     ctx;
    PtPresentFn present;
    PtDisposeFn dispose;   /* optional */
    void       *user;
} PtConfig;

/* Start the thread; returns once its ctx(1) has run. 1 on success (the
 * producer's current slot is 0), 0 on failure (nothing running). */
int  pt_start(const PtConfig *cfg);
/* Present everything queued, release the context on the present thread,
 * join. Done tokens still held are passed to dispose on the present thread
 * before it releases its context. Safe when not running. */
void pt_stop(void);
int  pt_running(void);

/* Producer: the slot being composed. */
int  pt_current(void);
/* Producer: queue the current slot with `ready`, then take the next free slot
 * (blocking while none is). *next_done receives that slot's done token (NULL
 * if none); *wait_ns the time spent blocked. Returns the new current slot, or
 * -1 when not running. */
int  pt_submit(void *ready, void **next_done, uint64_t *wait_ns);
/* Producer: wait until nothing is queued or presenting. */
void pt_drain(void);

typedef struct PtStats {
    uint64_t submits, presents, waits, wait_ns, present_ns, present_max_ns;
    int      slots, queued_high;
} PtStats;
void pt_get_stats(PtStats *out);

#ifdef __cplusplus
}
#endif
#endif /* PSX_PRESENT_THREAD_H */
