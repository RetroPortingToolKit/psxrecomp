#include "render_pass_plan.h"
#include "render_pass.h"
#include <stdlib.h>
#include <string.h>

int render_pass_mmio_class(uint32_t phys, uint32_t val, uint32_t width) {
    if (phys == 0x1F801810u) return width == 4 ? -1 : RENDER_PASS_DROP_GPU;
    if (phys == 0x1F801814u) {
        uint32_t cmd = val >> 24;
        return (width == 4 && (cmd == 0x04u || cmd == 0x10u))
            ? -1 : RENDER_PASS_DROP_GPU;
    }
    if (phys >= 0x1F801070u && phys <= 0x1F801077u) return -1;
    if ((phys >= 0x1F8010A0u && phys <= 0x1F8010AFu) ||   /* ch2 GPU */
        (phys >= 0x1F8010E0u && phys <= 0x1F8010EFu) ||   /* ch6 OTC */
        (phys >= 0x1F8010F0u && phys <= 0x1F8010F7u))     /* DPCR/DICR */
        return -1;
    if (phys >= 0x1F801080u && phys <= 0x1F8010FFu) return RENDER_PASS_DROP_DMA;
    if (phys >= 0x1F801C00u && phys <= 0x1F801FFFu) return RENDER_PASS_DROP_SPU;
    if (phys >= 0x1F801800u && phys <= 0x1F801803u) return RENDER_PASS_DROP_CD;
    if (phys >= 0x1F801100u && phys <= 0x1F80112Fu) return RENDER_PASS_DROP_TIMER;
    return RENDER_PASS_DROP_OTHER;
}

int render_pass_store_to(const RenderPassStoreTarget *t, uint32_t addr,
                         uint32_t val, uint32_t width) {
    uint32_t phys;
    if (addr >= 0xC0000000u) return RENDER_PASS_DROP_OTHER;   /* KSEG2 */
    if (t->isolate_cache) return -1;                /* cache-only store */
    phys = addr & 0x1FFFFFFFu;
    /* The 8 MiB DRAM decode window folds through the live geometry, exactly
     * as psx_ram_map_write: retail mirrors 2 MiB four times, the 8 MB RAM
     * map is unique. */
    if (phys < 0x00800000u) phys &= t->ram_size - 1u;
    if (phys < t->ram_size) {
        for (uint32_t i = 0; i < width; i++)
            t->ram[phys + i] = (uint8_t)(val >> (8u * i));
        return -1;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800000u + t->scratchpad_size) {
        uint32_t off = phys - 0x1F800000u;
        for (uint32_t i = 0; i < width && off + i < t->scratchpad_size; i++)
            t->scratchpad[off + i] = (uint8_t)(val >> (8u * i));
        return -1;
    }
    if (phys >= 0x1F801000u && phys <= 0x1F803FFFu) {
        int cls = render_pass_mmio_class(phys, val, width);
        if (cls >= 0) return cls;
        t->mmio_write(phys, val, width);
        return -1;
    }
    /* Expansion, ROM, unmapped: never written by a pass. (Mod arenas are
     * written and journaled before this policy: memory.c
     * render_pass_mod_store.) */
    return RENDER_PASS_DROP_OTHER;
}

int render_pass_vram_policy(const RenderPassJournal *j, int px, int py,
                            int pw, int ph, int vram_w, int vram_h,
                            int can_journal, int *x, int *y, int *w, int *h) {
    if (*w <= 0 || *h <= 0) return RENDER_PASS_VRAM_ALLOW;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > vram_w) *w = vram_w - *x;
    if (*y + *h > vram_h) *h = vram_h - *y;
    if (*w <= 0 || *h <= 0) return RENDER_PASS_VRAM_ALLOW;
    if (*x >= px && *y >= py && *x + *w <= px + pw && *y + *h <= py + ph)
        return RENDER_PASS_VRAM_ALLOW;
    if (!can_journal) return RENDER_PASS_VRAM_REFUSE;
    for (int i = 0; i < j->n; i++)
        if (*x >= j->x[i] && *y >= j->y[i] && *x + *w <= j->x[i] + j->w[i] &&
            *y + *h <= j->y[i] + j->h[i])
            return RENDER_PASS_VRAM_ALLOW;      /* already covered */
    if (j->n >= RENDER_PASS_JOURNAL_MAX) return RENDER_PASS_VRAM_REFUSE;
    /* Reserve the last entry for a whole-VRAM spill. A sprite scene can
     * upload more than 64 disjoint animation/CLUT rectangles in one redraw.
     * Back up the current contents once, including earlier temporary writes;
     * reverse-order rollback restores this spill FIRST and then restores
     * those earlier rectangles to their original contents. Subsequent writes
     * are covered by the spill, so capacity remains bounded without dropping
     * legitimate texture uploads or disabling native interpolation. */
    if (j->n == RENDER_PASS_JOURNAL_MAX - 1) {
        *x = *y = 0; *w = vram_w; *h = vram_h;
    }
    return RENDER_PASS_VRAM_JOURNAL;
}

int render_pass_journal_add(RenderPassJournal *j, const uint16_t *vram,
                            int vram_w, int x, int y, int w, int h) {
    int i = j->n;
    size_t need = (size_t)w * (size_t)h;
    if (i >= RENDER_PASS_JOURNAL_MAX || w <= 0 || h <= 0) return -1;
    if (j->cap[i] < need) {
        uint16_t *p = (uint16_t *)realloc(j->rows[i], need * sizeof(uint16_t));
        if (!p) return -1;
        j->rows[i] = p;
        j->cap[i] = need;
    }
    for (int row = 0; row < h; row++)
        memcpy(j->rows[i] + (size_t)row * (size_t)w,
               vram + (size_t)(y + row) * (size_t)vram_w + (size_t)x,
               (size_t)w * sizeof(uint16_t));
    j->x[i] = x; j->y[i] = y; j->w[i] = w; j->h[i] = h;
    j->n = i + 1;
    return i;
}

void render_pass_journal_rollback(RenderPassJournal *j, uint16_t *vram,
                                  int vram_w) {
    for (int i = j->n - 1; i >= 0; i--)
        for (int row = 0; row < j->h[i]; row++)
            memcpy(vram + (size_t)(j->y[i] + row) * (size_t)vram_w +
                       (size_t)j->x[i],
                   j->rows[i] + (size_t)row * (size_t)j->w[i],
                   (size_t)j->w[i] * sizeof(uint16_t));
    j->n = 0;
}

void render_pass_journal_free(RenderPassJournal *j) {
    for (int i = 0; i < RENDER_PASS_JOURNAL_MAX; i++) {
        free(j->rows[i]);
        j->rows[i] = NULL;
        j->cap[i] = 0;
    }
    j->n = 0;
}

int render_pass_stereo_pair_fresh(uint64_t pair_cycle, uint64_t now_cycle,
                                  uint32_t vblank_cycles) {
    if (now_cycle < pair_cycle) return 0;
    return now_cycle - pair_cycle <=
           (uint64_t)RENDER_PASS_STEREO_MAX_AGE_VBLANKS * vblank_cycles;
}
