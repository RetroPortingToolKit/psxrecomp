#ifndef PSX_RENDER_PASS_PLAN_H
#define PSX_RENDER_PASS_PLAN_H

/* Bounded store policy and VRAM journal for stereo and own-view transactions.
 * Temporal phase planning and image selection have been removed. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

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
