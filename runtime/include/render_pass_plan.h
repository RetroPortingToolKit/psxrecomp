#ifndef PSX_RENDER_PASS_PLAN_H
#define PSX_RENDER_PASS_PLAN_H

/* Pure planning and selection math for host-timed render passes
 * (render_pass.c, gpu_gl_renderer.c). No runtime state; unit-tested by
 * runtime/tests/test_render_pass_plan.c.
 *
 * Vocabulary. A game frame F is first presented at host tick `frame_start`
 * and stays on screen for `frame_length` host ticks (its flip period in guest
 * VBlanks times the presenter's source period). Phase p in [0, 1) is the
 * position inside that window. Phase 0 is the game's own image of F; a pass
 * rendered at phase a shows the game state a of the way from F to the next
 * frame. Phases are carried as Q16 (65536 = 1). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RENDER_PASS_MAX_PHASES 16u

typedef struct RenderPassPlanInput {
    double next_deadline;   /* the presenter's next output deadline (ticks) */
    double target_period;   /* ticks per output frame; <= 0 = unknown */
    double frame_start;     /* predicted first-present tick of the frame */
    double frame_length;    /* ticks the frame stays on screen */
    double pass_cost;       /* smoothed host ticks per pass; 0 = unknown:
                               with a budget, one pass is planned until one
                               has been measured */
    double budget;          /* host ticks the passes may use; < 0 = unlimited */
    uint32_t max;           /* caller's array capacity */
} RenderPassPlanInput;

/* Fill alpha_q16[] with the ascending phases the presenter will show during
 * the frame (the output deadlines that fall strictly inside it, excluding
 * ones within 1/64 of phase 0, which the game's own image already covers).
 * When the budget cannot pay for all of them, an evenly spread subset is
 * returned; while the pass cost is unknown that subset is one phase, since
 * passes run on the emulation thread. *wanted (optional) receives the
 * unshed count. Returns the count (0 when nothing is wanted or affordable). */
uint32_t render_pass_plan_phases(const RenderPassPlanInput *in,
                                 uint32_t *alpha_q16, uint32_t *wanted);

/* Pick what to show at phase p (0..1+) from a frame's rendered items.
 * phases[0..n) are ascending Q16 phases, phases[0] == 0 being the game's
 * image. Sets *lo and *hi (item indices) and *t (blend weight of hi, 0..1):
 * inside [phases[i], phases[i+1]] the two neighbours are blended; past the
 * last item it is held (nothing newer exists yet). Returns 0 when n == 0. */
int render_pass_select(const uint32_t *phases, uint32_t n, double p,
                       uint32_t *lo, uint32_t *hi, float *t);

/* How long a promoted generation stays on screen without the next flip, in
 * frame lengths (phase units), before it expires. */
#define RENDER_PASS_GEN_HOLD_MAX 4.0

/* What a promoted generation shows at phase p: render_pass_select, except
 * that it expires (returns 0) past RENDER_PASS_GEN_HOLD_MAX or for a p that
 * is not a number. Phase 1 and beyond means the next flip is late (the game
 * frame lasts longer than the plan assumed, e.g. a lagging tick): the newest
 * image holds, as a late stock frame would, and the presenter never falls
 * back to its own capture of the frame, which is older (phase 0). Only a
 * game that stops flipping for several frame lengths gets that back. */
int render_pass_gen_select(const uint32_t *phases, uint32_t n, double p,
                           uint32_t *lo, uint32_t *hi, float *t);
/* HOLD presents one native phase image without a temporal crossfade. */
int render_pass_gen_select_mode(const uint32_t *phases, uint32_t n, double p,
                                int hold, uint32_t *lo, uint32_t *hi, float *t);

/* Is the flip the FLIP source just saw the one a pending generation waits
 * for? A generation built for the next flip (`shown` 0, the canonical PsyQ
 * VSync-then-PutDispEnv point) waits for the display to flip to its own rect.
 * One built for a frame already on screen (`shown` 1: the game flips each
 * frame as soon as it is drawn) waits for the next flip, to any other rect;
 * its images then show one game frame late. Either way the presented
 * geometry must still be the one the generation was captured at. */
int render_pass_gen_flip_matches(int shown, int gen_x, int gen_y,
                                 int gen_source, int gen_w, int gen_h,
                                 int flip_x, int flip_y, int flip_source,
                                 int flip_w, int flip_h);

/* Exponential moving average used for the per-pass host cost. */
double render_pass_ema(double current, double sample);

/* Smoothed host cost of one pass at one presented image size (the renderer
 * starts a new one when the size changes). Fed one sample per pass.
 *
 * - `allocated`: the pass created textures or framebuffers (first use, a
 *   size change). That one-time cost says nothing about the next pass -- at
 *   a high internal resolution the first passes cost several times the
 *   steady state -- and a plan that sheds every pass for time never
 *   measures again, so such a sample is left out, at most
 *   RENDER_PASS_ALLOC_SKIPS times in a row (any kept sample resets that).
 * - Warm-up: the first RENDER_PASS_COST_WARMUP kept samples seed the average
 *   with their median, so one slow pass (a busy host at the first race
 *   frame) cannot price passes out. Until then the estimate is 0 (unknown),
 *   for which a plan asks for one pass (render_pass_plan_phases).
 * - Then an exponential moving average (render_pass_ema).
 * - Re-measuring (render_pass_cost_note_plan): only passes that run are
 *   measured, so an estimate that prices every plan out is never corrected
 *   by one -- and the first passes of a race can all run in a transient
 *   (a busy host, code seen for the first time) that costs several times the
 *   steady state. An estimate no pass has been measured against for
 *   `rewarm_after` plans (RENDER_PASS_REWARM_MIN, about a second of 30 Hz
 *   frames) restarts the warm-up. When the new median is more than
 *   RENDER_PASS_REWARM_STALE of the old estimate, the old one was right -- a
 *   size that is truly too expensive, or a machine at its limit -- and the
 *   next wait doubles, up to RENDER_PASS_REWARM_MAX; a stale estimate found
 *   keeps the wait at the minimum. */
#define RENDER_PASS_ALLOC_SKIPS 8u
#define RENDER_PASS_COST_WARMUP 3u
#define RENDER_PASS_REWARM_MIN  30u
#define RENDER_PASS_REWARM_MAX  960u
#define RENDER_PASS_REWARM_STALE 0.75
typedef struct RenderPassCost {
    double   ema;
    double   warm[RENDER_PASS_COST_WARMUP];
    unsigned kept;                  /* kept samples, saturating at WARMUP */
    unsigned skips;                 /* allocating samples left out in a row */
    unsigned unsampled;             /* plans since the last kept sample */
    unsigned rewarm_after;          /* 0 = RENDER_PASS_REWARM_MIN */
    double   rewarm_from;           /* estimate being re-measured; 0 = none */
} RenderPassCost;
void   render_pass_cost_add(RenderPassCost *cost, double sample, int allocated);
/* Host ticks per pass for planning; 0 while the cost is unknown. */
double render_pass_cost_estimate(const RenderPassCost *cost);
/* Call once per plan that wanted passes at this cost's image size. Returns 1
 * when the plan found the estimate stale and restarted the warm-up. */
int    render_pass_cost_note_plan(RenderPassCost *cost);

/* Next frame's pass budget from the last frame: the host time the presenter
 * spent idle-waiting plus the time passes used, scaled by `share` (0..1) and
 * clamped to [0, frame_length]. No history (both zero) -> share of the frame. */
double render_pass_budget(double idle_ticks, double pass_ticks,
                          double frame_length, double share);

/* Store policy inside a pass (memory.c): -1 = the MMIO store may reach its
 * device (GP0; GP1 DMA mode 0x04 / info 0x10; GPU and OTC DMA channels;
 * DPCR/DICR; I_STAT/I_MASK -- all restored after the pass), otherwise the
 * RENDER_PASS_DROP_* class it is dropped and counted under (render_pass.h). */
int render_pass_mmio_class(uint32_t phys, uint32_t val, uint32_t width);

/* One guest store made inside a pass (memory.c render_pass_store): RAM and
 * scratchpad are written directly, bypassing every live-timeline observer;
 * MMIO reaches `mmio_write` only when render_pass_mmio_class allows it; KSEG2
 * (cache control), mod memory, expansion and ROM are dropped. Returns -1 when
 * the store was made (or absorbed: an isolated-cache store changes nothing a
 * pass restores), else the RENDER_PASS_DROP_* class it was dropped under,
 * which the caller counts. */
typedef struct RenderPassStoreTarget {
    uint8_t *ram;              /* main RAM backing store */
    uint32_t ram_size;         /* LIVE geometry, a power of two: 2 MiB retail
                                  (mirrored 4x in the 8 MiB window) or 8 MiB
                                  (psx_ram_live_bytes()) */
    uint8_t *scratchpad;
    uint32_t scratchpad_size;
    int      isolate_cache;    /* COP0 SR IsC is set */
    void   (*mmio_write)(uint32_t phys, uint32_t val, uint32_t width);
} RenderPassStoreTarget;
int render_pass_store_to(const RenderPassStoreTarget *t, uint32_t addr,
                         uint32_t val, uint32_t width);

/* VRAM journal for a pass's writes outside its display rect
 * (gpu_gl_renderer.c pass_refuse_write): the CPU-side policy and the CPU VRAM
 * rows. The renderer keeps the matching GPU copies per entry. */
/* Jersey Devil streams more than 16 small texture/CLUT rectangles in a draw.
 * Keep the transaction bounded, but allow the complete set to roll back. */
#define RENDER_PASS_JOURNAL_MAX 64
enum {
    RENDER_PASS_VRAM_ALLOW = 0,    /* inside the rect, empty, or already journaled */
    RENDER_PASS_VRAM_JOURNAL = 1,  /* outside: back up, then allow */
    RENDER_PASS_VRAM_REFUSE = 2    /* outside and cannot be journaled */
};
typedef struct RenderPassJournal {
    int      n;
    int      x[RENDER_PASS_JOURNAL_MAX], y[RENDER_PASS_JOURNAL_MAX];
    int      w[RENDER_PASS_JOURNAL_MAX], h[RENDER_PASS_JOURNAL_MAX];
    uint16_t *rows[RENDER_PASS_JOURNAL_MAX];
    size_t   cap[RENDER_PASS_JOURNAL_MAX];
} RenderPassJournal;
/* Clip the write (*x, *y, *w, *h) to vram_w x vram_h and decide it against
 * the pass rect (px, py, pw, ph) and the journal. can_journal = 0 refuses
 * every outside write. */
int  render_pass_vram_policy(const RenderPassJournal *j, int px, int py,
                             int pw, int ph, int vram_w, int vram_h,
                             int can_journal, int *x, int *y, int *w, int *h);
/* Record the CPU rows of rect (x, y, w, h) of vram (vram_w halfwords per
 * row) as the next entry. Returns its index, or -1 when full / out of
 * memory (nothing recorded). */
int  render_pass_journal_add(RenderPassJournal *j, const uint16_t *vram,
                             int vram_w, int x, int y, int w, int h);
/* Put every entry's rows back, newest first, and empty the journal. */
void render_pass_journal_rollback(RenderPassJournal *j, uint16_t *vram,
                                  int vram_w);
void render_pass_journal_free(RenderPassJournal *j);

/* A published stereo pair may stay on screen only while the plugin keeps
 * submitting. The pair is fresh while the guest clock is within
 * RENDER_PASS_STEREO_MAX_AGE_VBLANKS vblanks after the pair's capture cycle
 * (3x the 8-vblank maximum draw cadence). A clock behind the pair (savestate
 * or rollback load) is stale. */
#define RENDER_PASS_STEREO_MAX_AGE_VBLANKS 24u
int render_pass_stereo_pair_fresh(uint64_t pair_cycle, uint64_t now_cycle,
                                  uint32_t vblank_cycles);

#ifdef __cplusplus
}
#endif

#endif
