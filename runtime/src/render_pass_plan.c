#include "render_pass_plan.h"
#include "render_pass.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define Q16 65536.0

static uint32_t to_q16(double p) {
    double q = floor(p * Q16 + 0.5);
    if (q < 1.0) q = 1.0;
    if (q > Q16 - 1.0) q = Q16 - 1.0;
    return (uint32_t)q;
}

uint32_t render_pass_plan_phases(const RenderPassPlanInput *in,
                                 uint32_t *alpha_q16, uint32_t *wanted) {
    double phases[RENDER_PASS_MAX_PHASES * 4u];
    uint32_t count = 0, cap, n;

    if (wanted) *wanted = 0;
    if (!in || !alpha_q16 || in->max == 0 || !(in->frame_length > 0.0) ||
        !isfinite(in->frame_length))
        return 0;
    cap = sizeof phases / sizeof phases[0];

    /* No live output schedule: nothing would show a pass. */
    if (!(in->target_period > 0.0) || !isfinite(in->target_period) ||
        !isfinite(in->next_deadline) || !isfinite(in->frame_start))
        return 0;
    {
        /* Walk the presenter's free-running output grid through the frame. */
        double end = in->frame_start + in->frame_length;
        double d = in->next_deadline;
        if (d <= in->frame_start) {
            double steps = floor((in->frame_start - d) / in->target_period) + 1.0;
            d += steps * in->target_period;
        }
        for (; d < end && count < cap; d += in->target_period) {
            double p = (d - in->frame_start) / in->frame_length;
            if (p < 1.0 / 64.0) continue;
            if (p >= 1.0) break;
            phases[count++] = p;
        }
    }
    if (count == 0) return 0;
    if (wanted) *wanted = count;

    n = count;
    if (n > in->max) n = in->max;
    if (n > RENDER_PASS_MAX_PHASES) n = RENDER_PASS_MAX_PHASES;
    if (in->budget >= 0.0) {
        /* Cost unknown (no pass measured at this image size yet): one pass
         * measures it without stalling the guest for a whole plan. */
        double fit = in->pass_cost > 0.0 ? floor(in->budget / in->pass_cost)
                                         : (in->budget > 0.0 ? 1.0 : 0.0);
        if (fit < 0.0) fit = 0.0;
        if (fit < (double)n) n = (uint32_t)fit;
    }
    if (n == 0) return 0;
    if (n == count) {
        for (uint32_t i = 0; i < n; i++) alpha_q16[i] = to_q16(phases[i]);
    } else {
        /* Evenly spread subset; the presenter blends across the gaps. */
        uint32_t last = UINT32_MAX, out = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = (uint32_t)floor(((double)i + 0.5) *
                                           (double)count / (double)n);
            if (idx >= count) idx = count - 1;
            if (idx == last) continue;
            alpha_q16[out++] = to_q16(phases[idx]);
            last = idx;
        }
        n = out;
    }
    return n;
}

int render_pass_select(const uint32_t *phases, uint32_t n, double p,
                       uint32_t *lo, uint32_t *hi, float *t) {
    uint32_t i;
    double q, a, b, w;
    if (!phases || n == 0) return 0;
    q = p * Q16;
    if (!(q > (double)phases[0])) {
        *lo = *hi = 0;
        *t = 0.0f;
        return 1;
    }
    for (i = 0; i + 1 < n && (double)phases[i + 1] <= q; i++) { }
    if (i + 1 >= n) {
        *lo = *hi = n - 1;
        *t = 0.0f;
        return 1;
    }
    a = (double)phases[i];
    b = (double)phases[i + 1];
    w = b > a ? (q - a) / (b - a) : 0.0;
    if (w <= 1.0 / 256.0) {
        *lo = *hi = i;
        *t = 0.0f;
    } else if (w >= 1.0 - 1.0 / 256.0) {
        *lo = *hi = i + 1;
        *t = 0.0f;
    } else {
        *lo = i;
        *hi = i + 1;
        *t = (float)w;
    }
    return 1;
}

int render_pass_gen_flip_matches(int shown, int gen_x, int gen_y,
                                 int gen_source, int gen_w, int gen_h,
                                 int flip_x, int flip_y, int flip_source,
                                 int flip_w, int flip_h) {
    const int same_rect = gen_x == flip_x && gen_y == flip_y;
    if (gen_source != flip_source || gen_w != flip_w || gen_h != flip_h)
        return 0;
    return shown ? !same_rect : same_rect;
}

int render_pass_gen_select(const uint32_t *phases, uint32_t n, double p,
                           uint32_t *lo, uint32_t *hi, float *t) {
    if (!(p <= RENDER_PASS_GEN_HOLD_MAX)) return 0;
    return render_pass_select(phases, n, p, lo, hi, t);
}

int render_pass_gen_select_mode(const uint32_t *phases, uint32_t n, double p,
                                int hold, uint32_t *lo, uint32_t *hi, float *t) {
    if (!render_pass_gen_select(phases, n, p, lo, hi, t)) return 0;
    if (hold) { *hi = *lo; *t = 0.0f; }
    return 1;
}

double render_pass_ema(double current, double sample) {
    if (!(sample >= 0.0) || !isfinite(sample)) return current;
    if (!(current > 0.0)) return sample;
    return current * 0.75 + sample * 0.25;
}

void render_pass_cost_add(RenderPassCost *cost, double sample, int allocated) {
    if (!cost || !(sample >= 0.0) || !isfinite(sample)) return;
    if (allocated && cost->skips < RENDER_PASS_ALLOC_SKIPS) {
        cost->skips++;
        return;
    }
    cost->skips = 0;
    cost->unsampled = 0;
    if (cost->kept < RENDER_PASS_COST_WARMUP) {
        cost->warm[cost->kept++] = sample;
        if (cost->kept == RENDER_PASS_COST_WARMUP) {
            /* Median of the warm-up samples (insertion sort of three). */
            double w[RENDER_PASS_COST_WARMUP];
            memcpy(w, cost->warm, sizeof w);
            for (unsigned i = 1; i < RENDER_PASS_COST_WARMUP; i++)
                for (unsigned j = i; j > 0 && w[j - 1] > w[j]; j--) {
                    double t = w[j]; w[j] = w[j - 1]; w[j - 1] = t;
                }
            cost->ema = w[RENDER_PASS_COST_WARMUP / 2u];
            if (cost->rewarm_from > 0.0) {
                /* A re-measure: wait longer before the next one unless it
                 * found the old estimate stale. */
                unsigned limit = cost->rewarm_after ? cost->rewarm_after
                                                    : RENDER_PASS_REWARM_MIN;
                if (cost->ema <= cost->rewarm_from * RENDER_PASS_REWARM_STALE)
                    cost->rewarm_after = 0;
                else
                    cost->rewarm_after = limit >= RENDER_PASS_REWARM_MAX / 2u
                                         ? RENDER_PASS_REWARM_MAX : limit * 2u;
                cost->rewarm_from = 0.0;
            }
        }
        return;
    }
    cost->ema = render_pass_ema(cost->ema, sample);
}

double render_pass_cost_estimate(const RenderPassCost *cost) {
    if (!cost || cost->kept < RENDER_PASS_COST_WARMUP) return 0.0;
    return cost->ema;
}

int render_pass_cost_note_plan(RenderPassCost *cost) {
    unsigned limit;
    /* Warming up: its one-pass plans are what measures the cost. */
    if (!cost || cost->kept < RENDER_PASS_COST_WARMUP) return 0;
    limit = cost->rewarm_after ? cost->rewarm_after : RENDER_PASS_REWARM_MIN;
    if (++cost->unsampled < limit) return 0;
    /* No pass has been measured against the estimate for `limit` plans:
     * measure again (render_pass_cost_add sets the next wait). */
    cost->rewarm_from = cost->ema;
    cost->kept = 0;
    cost->skips = 0;
    cost->unsampled = 0;
    return 1;
}

double render_pass_budget(double idle_ticks, double pass_ticks,
                          double frame_length, double share) {
    double b;
    if (!(frame_length > 0.0)) return 0.0;
    if (!(share > 0.0)) return 0.0;
    if (share > 1.0) share = 1.0;
    if (!(idle_ticks > 0.0) && !(pass_ticks > 0.0))
        return frame_length * share;
    b = ((idle_ticks > 0.0 ? idle_ticks : 0.0) +
         (pass_ticks > 0.0 ? pass_ticks : 0.0)) * share;
    if (b > frame_length) b = frame_length;
    return b;
}

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
    return j->n < RENDER_PASS_JOURNAL_MAX ? RENDER_PASS_VRAM_JOURNAL
                                          : RENDER_PASS_VRAM_REFUSE;
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
