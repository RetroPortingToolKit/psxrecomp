/* pgxp.cpp — PGXP value-propagation engine (docs/ENHANCEMENTS.md G1.2/G1.3).
 *
 * CLEAN-ROOM implementation of the publicly documented PGXP technique
 * (psx-spx GTE docs + public design write-ups + our own G1 measurements).
 * The vendored duckstation/ (CC BY-NC-ND) and beetle-psx/ (GPL) trees are
 * black-box behavioral oracles only — no code from them appears here.
 *
 * Model
 * -----
 * Every 32-bit word of guest RAM/scratchpad, every GPR (plus HI/LO), and
 * every GTE data register owns a shadow slot recording the sub-pixel screen
 * position that word carries (16.16 X/Y + projected SZ depth), the exact
 * guest word it describes (`value`), and per-half validity flags. RTPS/RTPT
 * fill the SXY shadow FIFO with the pre-truncation projection; the
 * psx_pgxp_* hooks copy shadows along with the data (loads, stores, COP2
 * transfers, and — in cpu-mode — the arithmetic games use to repack vertex
 * halves); the GPU asks for the precise position of a GP0 vertex word by the
 * packet's RAM address.
 *
 * The safety invariant: a shadow is only ever BELIEVED after validation
 * against the actual guest word it claims to describe, and only while no
 * writer it did not see has replaced that word. A matching word alone does
 * not prove provenance: two projections that round to the same integer X/Y
 * carry different fractions and depths. So every CPU write of a GPR or
 * memory word runs a hook that carries or resets its shadow, and DMA / host
 * stores drop the shadow of each word they touch (pgxp_invalidate_word, from
 * memory.c). Validation still catches any other writer that changes the
 * word. We never model side effects — overwrite, invalidate, validate.
 *
 * Everything here is host-only and visual-only: guest-visible state is never
 * read back from shadows, shadows are dropped on savestate/rewind, and the
 * speculative native-validation bracket suppresses all recording.
 */

#include "pgxp.h"
#include "pgxp_hooks.h"
#include "cpu_state.h"
#include "psx_memory.h"
#include "mod_memory.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

/* gte.cpp — position-cache fallback tier (ambiguity-gated, G1.4 exact table) */
extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16);
extern "C" int gte_geometry_correction_lookup_probe(uint32_t packed,
                                                    int32_t *x16, int32_t *y16);

/* ------------------------------------------------------------------------- */
/* Shadow storage                                                             */
/* ------------------------------------------------------------------------- */

enum {
    PGXP_F_VX = 1u << 0,   /* low half  (screen X) tracked                    */
    PGXP_F_VY = 1u << 1,   /* high half (screen Y) tracked                    */
    PGXP_F_VZ = 1u << 2,   /* projected depth rode along intact               */
    PGXP_F_PROJECTION = 1u << 3,
    /* The tracked half was DERIVED by guest arithmetic that rounds (a CPU
     * divide of a precise sum, see the scalar tier below), so the guest
     * integer is that arithmetic's rounding of the value, not its floor: the
     * precise value may sit up to one pixel below or two above it. */
    PGXP_F_DX = 1u << 4,
    PGXP_F_DY = 1u << 5,
    PGXP_F_VXY = PGXP_F_VX | PGXP_F_VY,
};

struct PGXPValue {
    int32_t  x16, y16;   /* sub-pixel screen coords, 16.16                    */
    uint16_t z;          /* projected SZ depth (perspective source), 0 = none */
    uint16_t flags;
    uint32_t value;      /* the guest word this shadow describes              */
    uint32_t gen;        /* valid iff == s_gen (O(1) invalidate-all)          */
    PGXPProjection projection;
};

/* Shadow covers the host RAM backing so the opt-in 8 MB map tracks its high
 * banks; retail sessions only ever touch the low 2 MiB of it. */
#define PGXP_RAM_WORDS     (PSX_MAIN_RAM_BACKING_BYTES >> 2)
#define PGXP_SCRATCH_WORDS (0x400u >> 2)      /* 1 KB scratchpad              */
#define PGXP_REG_HI        32
#define PGXP_REG_LO        33

static PGXPValue *s_ram = nullptr;            /* lazily allocated, 72 MiB VA  */
// Extended draw buffers live outside the main-RAM decode window. Give their
// allocated words separate shadows, in stable pages created only on writes.
// Read misses never allocate, and a page cannot move while a checkpoint uses it.
#define PGXP_GPU_PAGE_WORDS 1024u
#define PGXP_GPU_WORDS (PSX_MOD_GPU_DMA_APERTURE_SIZE >> 2)
static PGXPValue *s_gpu_pages[PGXP_GPU_WORDS / PGXP_GPU_PAGE_WORDS];
static PGXPValue  s_scratch[PGXP_SCRATCH_WORDS];
static PGXPValue  s_gpr[34];                  /* 32 GPRs + HI + LO            */
static PGXPValue  s_gte[32];                  /* GTE data registers           */
static PGXPValue  s_gtc[32];                  /* GTE control registers (CTC2) */

/* Scalar tier (cpu-mode). Engines also use the GTE as a multiplier for
 * vertex arithmetic: Ape Escape's terrain subdivider packs two projected X
 * (or Y) halves into a light-matrix row with CTC2, weights them with MVMVA
 * by their depths, reads MAC back with MFC2 and DIVs by the depth sum to get
 * the screen position of an edge midpoint. A 16.16 half cannot hold those
 * intermediate products, so registers carry an optional full-range precise
 * value beside their half shadows: valid while `gen` is current and `value`
 * still equals the register, killed by every hooked write that does not
 * produce one. It turns back into a half shadow only when stored (SH / SW)
 * as a 16-bit coordinate. */
struct PGXPScalar {
    double   v;          /* precise value of the 32-bit register              */
    uint32_t value;      /* the register value it describes                   */
    uint32_t gen;
};
static PGXPScalar s_gpr_s[34];                /* GPRs + HI + LO               */
static PGXPScalar s_gte_s[32];                /* GTE data registers (IR, MAC) */
static PGXPScalar s_gtc_s[32];                /* GTE control (TR / BK words)  */

static uint32_t s_gen = 1;
static int      s_enabled = 0;
static int      s_cpu_mode = 0;
static int      s_projection_tracking = 0;
static float    s_tolerance = 0.5f;   /* user-validated seam clamp (G1.10) */
static int      s_position_fallback = 1;   /* G1.4 cache tier (G1.11 switch) */
static int      s_preserve_projection = 0; /* exact-projection shadows (G1.11) */
static int      s_culling = 0;             /* precise NCLIP sign (G1.12)   */
static uint32_t s_suppress = 0;
static int      s_deferred_invalidate = 0;

/* Single hot-path gate for every hook. */
static int g_pgxp_active = 0;
static inline void recompute_active(void) {
    g_pgxp_active = (s_enabled && s_suppress == 0) ? 1 : 0;
}

static PGXPStats s_stats;
static PGXPStoreRecord *s_store_ring = nullptr;   /* pgxp_store_ring; with s_ram */

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

static void ck_wrapped(void);

extern "C" void pgxp_invalidate_all(void) {
    if (s_suppress != 0) { s_deferred_invalidate = 1; return; }
    if (++s_gen == 0) {
        ck_wrapped();
        /* generation wrapped: physically clear so stale slots can't revive */
        if (s_ram) std::memset(s_ram, 0, PGXP_RAM_WORDS * sizeof(PGXPValue));
        for (PGXPValue* page : s_gpu_pages)
            if (page) std::memset(page, 0, PGXP_GPU_PAGE_WORDS * sizeof(PGXPValue));
        std::memset(s_scratch, 0, sizeof(s_scratch));
        std::memset(s_gpr, 0, sizeof(s_gpr));
        std::memset(s_gte, 0, sizeof(s_gte));
        std::memset(s_gtc, 0, sizeof(s_gtc));
        std::memset(s_gpr_s, 0, sizeof(s_gpr_s));
        std::memset(s_gte_s, 0, sizeof(s_gte_s));
        std::memset(s_gtc_s, 0, sizeof(s_gtc_s));
        s_gen = 1;
    }
}

extern "C" void pgxp_set_enabled(int enabled) {
    int resized = 0;
    if (enabled && !s_store_ring)
        s_store_ring = (PGXPStoreRecord *)std::calloc(PGXP_STORE_RING_CAP,
                                                      sizeof(PGXPStoreRecord));
    if (enabled && !s_ram) {
        s_ram = (PGXPValue *)std::calloc(PGXP_RAM_WORDS, sizeof(PGXPValue));
        if (s_ram)
            resized = 1;
        else
            enabled = 0;                      /* fail closed: stay faithful   */
    }
    /* Re-applying configuration must not invalidate every live shadow. */
    if (s_enabled == (enabled ? 1 : 0) && !resized)
        return;
    s_enabled = enabled ? 1 : 0;
    pgxp_invalidate_all();
    recompute_active();
}

extern "C" int pgxp_enabled(void) { return s_enabled; }
extern "C" int pgxp_active(void) { return g_pgxp_active; }

extern "C" void pgxp_set_cpu_mode(int enabled) { s_cpu_mode = enabled ? 1 : 0; }
extern "C" int  pgxp_cpu_mode(void) { return s_cpu_mode; }

extern "C" void pgxp_set_projection_tracking(int enabled) {
    if (s_projection_tracking == !!enabled) return;
    s_projection_tracking = !!enabled;
    pgxp_invalidate_all();
}
extern "C" int pgxp_projection_tracking(void) { return s_projection_tracking; }

extern "C" void  pgxp_set_tolerance(float pixels) { s_tolerance = pixels; }
extern "C" float pgxp_tolerance(void) { return s_tolerance; }

extern "C" void pgxp_set_position_fallback(int enabled) {
    s_position_fallback = enabled ? 1 : 0;
}
extern "C" int pgxp_position_fallback(void) { return s_position_fallback; }

extern "C" void pgxp_set_preserve_projection(int enabled) {
    s_preserve_projection = enabled ? 1 : 0;
}
extern "C" int pgxp_preserve_projection(void) { return s_preserve_projection; }

extern "C" void pgxp_set_culling(int enabled) { s_culling = enabled ? 1 : 0; }
extern "C" int pgxp_culling(void) { return s_culling; }

static int s_mod_request = 0;
static int s_mod_request_cpu = 0;
static int s_mod_request_cull = 0;

extern "C" void pgxp_mod_request(int enabled, int cpu_mode, int culling) {
    s_mod_request = enabled ? 1 : 0;
    s_mod_request_cpu = (enabled && cpu_mode) ? 1 : 0;
    s_mod_request_cull = (enabled && culling) ? 1 : 0;
}

extern "C" int pgxp_mod_requested(int *cpu_mode, int *culling) {
    if (cpu_mode) *cpu_mode = s_mod_request_cpu;
    if (culling) *culling = s_mod_request_cull;
    return s_mod_request;
}

extern "C" int pgxp_mod_request_take(int *cpu_mode, int *culling) {
    const int requested = pgxp_mod_requested(cpu_mode, culling);
    pgxp_mod_request(0, 0, 0);
    return requested;
}

extern "C" void pgxp_suppress_begin(void) {
    ++s_suppress;
    recompute_active();
}

extern "C" void pgxp_suppress_end(void) {
    if (s_suppress != 0 && --s_suppress == 0) {
        if (s_deferred_invalidate) {
            s_deferred_invalidate = 0;
            pgxp_invalidate_all();
        }
        recompute_active();
    }
}

extern "C" void pgxp_get_stats(PGXPStats *out) {
    if (out) *out = s_stats;
}

extern "C" void pgxp_note_rect_bypass(int all_precise) {
    if (all_precise) s_stats.rect_bypass++;
    else             s_stats.rect_partial++;
}

extern "C" void pgxp_note_nclip(int disagree, int corrected) {
    if (disagree)  s_stats.nclip_disagree++;
    if (corrected) s_stats.nclip_corrected++;
}

extern "C" void pgxp_note_triangle(int precise) {
    if (precise >= 3)     s_stats.tri_precise++;
    else if (precise > 0) s_stats.tri_mixed++;
    else                  s_stats.tri_native++;
}

static PGXPTriRecord s_tri_ring[PGXP_TRI_RING_CAP];
static uint64_t      s_tri_seq = 0;

static int ck_in_pass(void);                  /* render-pass checkpoint open */

extern "C" void pgxp_note_triangle_detail(const PGXPTriRecord *rec) {
    PGXPTriRecord *r = &s_tri_ring[s_tri_seq % PGXP_TRI_RING_CAP];
    *r = *rec;
    r->seq = s_tri_seq++;
    r->pass = ck_in_pass() ? 1u : 0u;
}

extern "C" uint64_t pgxp_tri_ring(const PGXPTriRecord **ring, uint32_t *cap) {
    *ring = s_tri_ring;
    *cap = PGXP_TRI_RING_CAP;
    return s_tri_seq;
}

/* Store PC and frame counter belong to the host (debug server); it hands
 * their addresses over once (pgxp_set_trace_sources) so the engine stays
 * linkable on its own in the white-box tests. */
static const uint32_t  *s_trace_pc = nullptr;
static const uint64_t  *s_trace_frame = nullptr;
static uint64_t         s_store_seq = 0;

extern "C" void pgxp_set_trace_sources(const uint32_t *store_pc,
                                       const uint64_t *frame) {
    s_trace_pc = store_pc;
    s_trace_frame = frame;
}

static void note_store(uint32_t op, uint32_t reg, const PGXPValue *src,
                       uint32_t addr, uint32_t value, const PGXPValue *dst) {
    if (!s_store_ring) return;
    PGXPStoreRecord *r = &s_store_ring[s_store_seq % PGXP_STORE_RING_CAP];
    r->seq = s_store_seq++;
    r->frame = s_trace_frame ? (uint32_t)*s_trace_frame : 0u;
    r->pc = s_trace_pc ? *s_trace_pc : 0u;
    r->addr = addr;
    r->value = value;
    r->op = (uint8_t)op;
    r->reg = (uint8_t)reg;
    r->src_live = (src && src->gen == s_gen) ? 1u : 0u;
    r->pad = 0;
    r->src_flags = src ? src->flags : 0u;
    r->dst_flags = (dst && dst->gen == s_gen) ? dst->flags : 0u;
    r->dst_x16 = dst ? dst->x16 : 0;
    r->dst_y16 = dst ? dst->y16 : 0;
}

extern "C" uint64_t pgxp_store_ring(const PGXPStoreRecord **ring, uint32_t *cap) {
    *ring = s_store_ring;
    *cap = s_store_ring ? PGXP_STORE_RING_CAP : 0u;
    return s_store_ring ? s_store_seq : 0u;
}

/* ------------------------------------------------------------------------- */
/* Address mapping + validation                                               */
/* ------------------------------------------------------------------------- */

/* Guest address -> shadow slot, or NULL for BIOS/MMIO/KSEG2 (untrackable). */
static inline PGXPValue *pgxp_ptr(uint32_t addr, bool create = false) {
    uint32_t m = addr & 0x1FFFFFFFu;
    if (m < PSX_MAIN_RAM_WINDOW_BYTES)         /* RAM + its mirrors (live map) */
        return s_ram ? &s_ram[psx_ram_canonical_offset(m) >> 2] : nullptr;
    if ((m & 0xFFFFFC00u) == 0x1F800000u)      /* scratchpad                  */
        return &s_scratch[(m & 0x3FCu) >> 2];
    if (addr < 0xC0000000u && m >= PSX_MOD_GPU_DMA_APERTURE_BASE &&
        m < PSX_MOD_GPU_DMA_APERTURE_BASE + PSX_MOD_GPU_DMA_APERTURE_SIZE &&
        psx_mod_gpu_dma_memory_contains(addr & ~3u, 4u)) {
        const uint32_t word = (m - PSX_MOD_GPU_DMA_APERTURE_BASE) >> 2;
        PGXPValue*& page = s_gpu_pages[word / PGXP_GPU_PAGE_WORDS];
        if (!page && create)
            page = static_cast<PGXPValue*>(std::calloc(PGXP_GPU_PAGE_WORDS, sizeof(PGXPValue)));
        return page ? &page[word % PGXP_GPU_PAGE_WORDS] : nullptr;
    }
    return nullptr;
}

/* ------------------------------------------------------------------------- */
/* Checkpoint (render passes)                                                 */
/* ------------------------------------------------------------------------- */

/* A render pass (render_pass.c) runs guest draw code on a sandboxed machine
 * and puts RAM, scratchpad and registers back with raw copies afterwards. The
 * shadows must roll back with them: otherwise they describe the words the
 * pass wrote (interpolated vertices), and the live frame's packets - consumed
 * after the passes - fail validation. Copying the shadow arrays whole per
 * pass would cost tens of MB; instead every RAM / scratchpad slot a pass
 * mutates is journaled on its first write, and register shadows (small) are
 * copied whole. A journal that cannot grow, or a generation wrap during the
 * pass, fails closed: rollback invalidates everything. */
struct PGXPJournalEntry {
    PGXPValue *slot;
    size_t index;
    PGXPValue old;
};

static uint32_t          s_ck_depth = 0;
static uint8_t          *s_ck_bits = nullptr;     /* one bit per shadow slot */
static PGXPValue        *s_ck_ram = nullptr;      /* s_ram the bits index    */
static PGXPJournalEntry *s_ck_log = nullptr;
static size_t            s_ck_n = 0, s_ck_cap = 0;
static int               s_ck_lossy = 0;
static PGXPValue         s_ck_gpr[34], s_ck_gte[32], s_ck_gtc[32];
static PGXPScalar        s_ck_gpr_s[34], s_ck_gte_s[32], s_ck_gtc_s[32];
static uint32_t          s_ck_gen = 0, s_ck_suppress = 0;
static int               s_ck_deferred = 0;

static inline size_t ck_index(uint32_t addr) {
    const uint32_t physical = addr & 0x1FFFFFFFu;
    if (physical < PSX_MAIN_RAM_WINDOW_BYTES)
        return psx_ram_canonical_offset(physical) >> 2;
    if ((physical & 0xFFFFFC00u) == 0x1F800000u)
        return (size_t)PGXP_RAM_WORDS + ((physical & 0x3FCu) >> 2);
    return (size_t)PGXP_RAM_WORDS + PGXP_SCRATCH_WORDS +
           ((physical - PSX_MOD_GPU_DMA_APERTURE_BASE) >> 2);
}

/* Journal a RAM / scratchpad slot before its first mutation in a pass. */
static inline void ck_note(PGXPValue *pv, uint32_t addr) {
    if (s_ck_depth == 0 || !pv) return;
    if (s_ck_lossy || !s_ck_bits) { s_ck_lossy = 1; return; }
    if (s_ram != s_ck_ram && (addr & 0x1FFFFFFFu) < PSX_MAIN_RAM_WINDOW_BYTES) {
        s_ck_lossy = 1;                        /* shadow RAM appeared mid-pass */
        return;
    }
    size_t i = ck_index(addr);
    uint8_t bit = (uint8_t)(1u << (i & 7u));
    if (s_ck_bits[i >> 3] & bit) return;
    if (s_ck_n == s_ck_cap) {
        size_t cap = s_ck_cap ? s_ck_cap * 2 : 65536;
        PGXPJournalEntry *p = (PGXPJournalEntry *)std::realloc(
            s_ck_log, cap * sizeof *p);
        if (!p) { s_ck_lossy = 1; return; }
        s_ck_log = p;
        s_ck_cap = cap;
    }
    s_ck_bits[i >> 3] |= bit;
    s_ck_log[s_ck_n].slot = pv;
    s_ck_log[s_ck_n].index = i;
    s_ck_log[s_ck_n].old = *pv;
    s_ck_n++;
}

static void ck_wrapped(void) {
    if (s_ck_depth != 0) s_ck_lossy = 1;       /* shadows were cleared        */
}

static int ck_in_pass(void) { return s_ck_depth != 0; }

static inline PGXPValue *pgxp_ptr_w(uint32_t addr) {
    PGXPValue *pv = pgxp_ptr(addr, true);
    ck_note(pv, addr);
    return pv;
}

static int packet_shadow_address(uint32_t addr, uint32_t *canonical) {
    if ((addr & 3u) || addr >= 0xC0000000u) return 0;
    const uint32_t physical = addr & 0x1FFFFFFFu;
    if (physical < PSX_MAIN_RAM_WINDOW_BYTES) {
        *canonical = psx_ram_canonical_offset(physical);
        return 1;
    }
    if ((physical & 0xFFFFFC00u) == 0x1F800000u) {
        *canonical = physical;
        return 1;
    }
    if (psx_mod_gpu_dma_memory_contains(addr, 4u)) {
        *canonical = physical;
        return 1;
    }
    return 0;
}

extern "C" int pgxp_capture_word_shadow(uint32_t addr, uint32_t expected,
                                       PGXPWordShadow *out) {
    uint32_t canonical;
    if (!out || !packet_shadow_address(addr, &canonical)) return 0;
    PGXPWordShadow receipt{};
    receipt.address = canonical;
    receipt.value = expected;
    receipt.source_generation = s_gen;
    const PGXPValue *source = pgxp_ptr(addr);
    if (s_enabled && source && source->gen == s_gen && source->value == expected) {
        receipt.valid = 1;
        receipt.x16 = source->x16;
        receipt.y16 = source->y16;
        receipt.z = source->z;
        receipt.flags = source->flags;
        receipt.projection = source->projection;
    }
    *out = receipt;
    return 1;
}

static int restore_word_shadow(uint32_t addr, uint32_t expected,
                               const PGXPWordShadow *in, bool relocated) {
    uint32_t canonical, source;
    if (!in || !s_ck_depth || s_ck_lossy || !packet_shadow_address(addr, &canonical) ||
        !packet_shadow_address(in->address, &source) || source != in->address ||
        (!relocated && in->address != canonical) ||
        in->value != expected || in->valid > 1u ||
        in->source_generation != s_ck_gen) return 0;
    if (in->valid && !s_enabled) return 0;
    PGXPValue *destination = pgxp_ptr(addr, in->valid != 0u);
    // With PGXP disabled/unallocated, there is already no RAM shadow to clear.
    if (!destination) return in->valid == 0u;
    ck_note(destination, addr);
    if (s_ck_lossy) return 0;
    PGXPValue restored{};
    if (in->valid) {
        restored.value = in->value;
        restored.gen = s_gen;
        restored.x16 = in->x16;
        restored.y16 = in->y16;
        restored.z = in->z;
        restored.flags = in->flags;
        restored.projection = in->projection;
    }
    *destination = restored;
    return 1;
}

extern "C" int pgxp_restore_word_shadow(uint32_t addr, uint32_t expected,
                                       const PGXPWordShadow *in) {
    return restore_word_shadow(addr, expected, in, false);
}

extern "C" int pgxp_restore_relocated_word_shadow(uint32_t destination,
                                                 uint32_t expected,
                                                 const PGXPWordShadow *in) {
    return restore_word_shadow(destination, expected, in, true);
}

extern "C" void pgxp_checkpoint_begin(void) {
    if (s_ck_depth++ != 0) return;             /* the outermost pass journals */
    if (!s_ck_bits) {
        s_ck_bits = (uint8_t *)std::calloc(
            ((size_t)PGXP_RAM_WORDS + PGXP_SCRATCH_WORDS + PGXP_GPU_WORDS + 7u) / 8u, 1);
        if (!s_ck_bits) s_ck_lossy = 1;
    }
    s_ck_ram = s_ram;
    s_ck_n = 0;
    std::memcpy(s_ck_gpr, s_gpr, sizeof s_gpr);
    std::memcpy(s_ck_gte, s_gte, sizeof s_gte);
    std::memcpy(s_ck_gtc, s_gtc, sizeof s_gtc);
    std::memcpy(s_ck_gpr_s, s_gpr_s, sizeof s_gpr_s);
    std::memcpy(s_ck_gte_s, s_gte_s, sizeof s_gte_s);
    std::memcpy(s_ck_gtc_s, s_gtc_s, sizeof s_gtc_s);
    s_ck_gen = s_gen;
    s_ck_suppress = s_suppress;
    s_ck_deferred = s_deferred_invalidate;
}

extern "C" void pgxp_checkpoint_rollback(void) {
    if (s_ck_depth == 0) return;
    if (--s_ck_depth != 0) return;
    for (size_t i = s_ck_n; i-- > 0;) {
        PGXPJournalEntry *e = &s_ck_log[i];
        *e->slot = e->old;
        size_t k = e->index;
        s_ck_bits[k >> 3] &= (uint8_t)~(1u << (k & 7u));
    }
    s_ck_n = 0;
    std::memcpy(s_gpr, s_ck_gpr, sizeof s_gpr);
    std::memcpy(s_gte, s_ck_gte, sizeof s_gte);
    std::memcpy(s_gtc, s_ck_gtc, sizeof s_gtc);
    std::memcpy(s_gpr_s, s_ck_gpr_s, sizeof s_gpr_s);
    std::memcpy(s_gte_s, s_ck_gte_s, sizeof s_gte_s);
    std::memcpy(s_gtc_s, s_ck_gtc_s, sizeof s_gtc_s);
    s_gen = s_ck_gen;
    /* A watchdog abort can leave a suppress bracket open: the machine it
     * interrupted is gone, so its bracket is too. */
    s_suppress = s_ck_suppress;
    s_deferred_invalidate = s_ck_deferred;
    recompute_active();
    if (s_ck_lossy || s_ram != s_ck_ram) {
        s_ck_lossy = 0;
        pgxp_invalidate_all();
    }
}

static inline int pv_live(const PGXPValue *pv) {
    return pv && pv->gen == s_gen && (pv->flags & PGXP_F_VXY) != 0;
}

/* Drop whichever tracked halves no longer match the actual guest word; the
 * depth belongs to the whole vertex, so any half going stale kills it too. */
static inline void pv_validate(PGXPValue *pv, uint32_t actual) {
    if (pv->gen != s_gen) return;
    uint32_t diff = pv->value ^ actual;
    if (diff) pv->flags &= (uint16_t)~PGXP_F_PROJECTION;
    if ((pv->flags & PGXP_F_VX) && (diff & 0x0000FFFFu))
        pv->flags &= (uint16_t)~(PGXP_F_VX | PGXP_F_DX | PGXP_F_VZ);
    if ((pv->flags & PGXP_F_VY) && (diff & 0xFFFF0000u))
        pv->flags &= (uint16_t)~(PGXP_F_VY | PGXP_F_DY | PGXP_F_VZ);
    pv->value = actual;
}

/* Mark a slot as "known word, no precision" — keeps `value` current so later
 * half-merges stay keyed correctly. */
static inline void pv_reset(PGXPValue *pv, uint32_t value) {
    pv->x16 = 0; pv->y16 = 0; pv->z = 0;
    pv->flags = 0;
    pv->value = value;
    pv->gen = s_gen;
}

static inline void pv_kill(PGXPValue *pv) { pv->gen = 0; }

extern "C" void pgxp_invalidate_word(uint32_t addr) {
    if (!g_pgxp_active) return;
    PGXPValue *pv = pgxp_ptr_w(addr);       /* a render pass rolls it back */
    if (pv) pv_kill(pv);
}

/* ------------------------------------------------------------------------- */
/* Instruction field helpers                                                  */
/* ------------------------------------------------------------------------- */

static inline uint32_t f_op(uint32_t i)    { return i >> 26; }
static inline uint32_t f_rs(uint32_t i)    { return (i >> 21) & 31u; }
static inline uint32_t f_rt(uint32_t i)    { return (i >> 16) & 31u; }
static inline uint32_t f_rd(uint32_t i)    { return (i >> 11) & 31u; }
static inline uint32_t f_shamt(uint32_t i) { return (i >> 6) & 31u; }
static inline uint32_t f_funct(uint32_t i) { return i & 63u; }
static inline int32_t  f_simm(uint32_t i)  { return (int32_t)(int16_t)(i & 0xFFFFu); }

/* ------------------------------------------------------------------------- */
/* Scalar tier helpers                                                        */
/* ------------------------------------------------------------------------- */

static inline void sc_kill(PGXPScalar *s) { s->gen = 0; }
static inline void sc_set(PGXPScalar *s, double v, uint32_t value) {
    s->v = v; s->value = value; s->gen = s_gen;
}
static inline int sc_valid(const PGXPScalar *s, uint32_t value) {
    return s->gen == s_gen && s->value == value;
}

/* A GPR write that produces no scalar ends whatever scalar the register had. */
static inline void gpr_written(uint32_t r) { if (r != 0) sc_kill(&s_gpr_s[r]); }

/* A register holding one 16-bit half, sign- or zero-extended (LH / LHU, or an
 * MTC2 / CTC2 of such a register), whose half shadow is tracked: its precise
 * value as a number. */
static inline int half_scalar(const PGXPValue *pv, uint32_t value, double *out) {
    if (!pv || pv->gen != s_gen || pv->value != value) return 0;
    if ((pv->flags & PGXP_F_VXY) != PGXP_F_VXY) return 0;
    const int32_t sx = (int32_t)(int16_t)(value & 0xFFFFu);
    const int32_t zx = (int32_t)(value & 0xFFFFu);
    if ((int32_t)value != sx && (int32_t)value != zx) return 0;
    if (pv->y16 != (((int32_t)value >> 16) << 16)) return 0;  /* exact extension */
    double v = pv->x16 / 65536.0;                 /* the half read as int16       */
    if ((int32_t)value == zx && zx != sx) v += 65536.0;   /* LHU of a negative half */
    *out = v;
    return 1;
}

/* The precise value of a register: its scalar, else its half shadow as a
 * number, else the exact integer (*precise = 0). */
static inline double reg_scalar(const PGXPScalar *s, const PGXPValue *pv,
                                uint32_t value, int is_unsigned, int *precise) {
    double v;
    if (s && sc_valid(s, value)) { *precise = 1; return s->v; }
    if (pv && half_scalar(pv, value, &v)) { *precise = 1; return v; }
    *precise = 0;
    return is_unsigned ? (double)value : (double)(int32_t)value;
}

static inline double gpr_scalar(uint32_t r, uint32_t value, int is_unsigned,
                                int *precise) {
    if (r == 0) { *precise = 0; return 0.0; }
    return reg_scalar(&s_gpr_s[r], &s_gpr[r], value, is_unsigned, precise);
}

/* A scalar that is about to become a 16-bit coordinate half: accept it when
 * it describes `half` (the stored integer) within the derived window, and
 * say whether the guest integer is its floor (plain) or a rounding of it
 * (derived). Returns the 16.16 value, or 0 with *ok = 0. */
static inline int32_t scalar_to_half(double v, int32_t half, int *ok, int *derived) {
    const double d = v - (double)half;
    *ok = 0; *derived = 0;
    if (!(d > -1.0 && d < 2.0)) return 0;          /* also rejects NaN            */
    *ok = 1;
    *derived = std::floor(v) != (double)half;
    return (int32_t)std::floor(v * 65536.0);
}

/* ------------------------------------------------------------------------- */
/* Memory-mode hooks: loads / stores                                          */
/* ------------------------------------------------------------------------- */

/* SXY2 and SXYP are one register seen at two addresses: whichever the hook
 * just filled, the other describes the same word. */
static inline void gte_sxy2_mirror(uint32_t reg) {
    if (reg == 14)      s_gte[15] = s_gte[14];
    else if (reg == 15) s_gte[14] = s_gte[15];
}

extern "C" void psx_pgxp_load(struct CPUState *cpu, uint32_t instr,
                              uint32_t addr, uint32_t value) {
    (void)cpu;
    if (!g_pgxp_active) return;
    uint32_t rt = f_rt(instr);
    if (rt == 0) return;
    PGXPValue *dst = &s_gpr[rt];
    gpr_written(rt);                           /* memory carries no scalars   */

    uint32_t lop = f_op(instr);
    /* LWL at byte 3 and LWR at byte 0 of a word each load the WHOLE word
     * (little-endian R3000A): the unaligned-copy idiom lwl/lwr on an aligned
     * address is a plain word load, so it carries the word's shadow. */
    if ((lop == 0x22 && (addr & 3u) == 3u) || (lop == 0x26 && (addr & 3u) == 0u))
        lop = 0x23;
    switch (lop) {
    case 0x23: {                               /* LW                          */
        PGXPValue *src = pgxp_ptr_w(addr);     /* validation may mutate it    */
        if (src && src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        return;
    }
    case 0x21:                                 /* LH                          */
    case 0x25: {                               /* LHU                         */
        PGXPValue *src = pgxp_ptr(addr);
        int hi_half = (addr >> 1) & 1;
        pv_reset(dst, value);
        /* The high half of the extended GPR is a known-exact constant
         * (0/-1 for LH, 0 for LHU): track it so re-packing via sll/or in
         * cpu-mode keeps working. */
        dst->y16 = ((int32_t)value >> 16) << 16;
        dst->flags = PGXP_F_VY;
        if (src && src->gen == s_gen) {
            uint32_t actual_half = (value & 0xFFFFu) << (hi_half ? 16 : 0);
            uint32_t mask = hi_half ? 0xFFFF0000u : 0x0000FFFFu;
            uint16_t want = hi_half ? PGXP_F_VY : PGXP_F_VX;
            if ((src->flags & want) && ((src->value ^ actual_half) & mask) == 0) {
                dst->x16 = hi_half ? src->y16 : src->x16;
                dst->flags |= PGXP_F_VX;
                if (src->flags & (hi_half ? PGXP_F_DY : PGXP_F_DX))
                    dst->flags |= PGXP_F_DX;
            }
        }
        return;
    }
    default:                                   /* LB/LBU/LWL/LWR: untrackable */
        pv_reset(dst, value);
        return;
    }
}

extern "C" void psx_pgxp_store(struct CPUState *cpu, uint32_t instr,
                               uint32_t addr, uint32_t value) {
    (void)cpu;
    if (!g_pgxp_active) return;
    PGXPValue *dst = pgxp_ptr_w(addr);
    if (!dst) return;
    uint32_t rt = f_rt(instr);
    PGXPValue *src = (rt != 0) ? &s_gpr[rt] : nullptr;

    uint32_t sop = f_op(instr);
    /* SWL at byte 3 and SWR at byte 0 each store the WHOLE word (the
     * unaligned-copy idiom on an aligned address): a plain word store. */
    if ((sop == 0x2A && (addr & 3u) == 3u) || (sop == 0x2E && (addr & 3u) == 0u))
        sop = 0x2B;
    switch (sop) {
    case 0x2B: {                               /* SW                          */
        PGXPValue before;
        std::memset(&before, 0, sizeof before);
        if (src) before = *src;
        if (src && src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        /* A register holding one coordinate as a full word (a scalar that
         * fits a half, e.g. a CPU-divided position) stores as that half,
         * with an exact sign extension above it. */
        if (s_cpu_mode && rt != 0 && sc_valid(&s_gpr_s[rt], value) &&
            (int32_t)value == (int32_t)(int16_t)(value & 0xFFFFu)) {
            int ok, derived;
            int32_t v16 = scalar_to_half(s_gpr_s[rt].v, (int16_t)(value & 0xFFFFu),
                                         &ok, &derived);
            if (ok) {
                pv_reset(dst, value);
                dst->x16 = v16;
                dst->y16 = ((int32_t)value >> 16) << 16;
                dst->flags = (uint16_t)(PGXP_F_VXY | (derived ? PGXP_F_DX : 0));
            }
        }
        note_store(0x2B, rt, src ? &before : nullptr, addr, value, dst);
        return;
    }
    case 0x29: {                               /* SH                          */
        int hi_half = (addr >> 1) & 1;
        uint32_t half = value & 0xFFFFu;
        if (dst->gen != s_gen) pv_reset(dst, half << (hi_half ? 16 : 0));
        /* Patch the stored half into the tracked word; the other half's
         * validity (if any) survives untouched. Depth never survives a
         * half-write — the vertex it described no longer exists whole. */
        if (hi_half) dst->value = (dst->value & 0x0000FFFFu) | (half << 16);
        else         dst->value = (dst->value & 0xFFFF0000u) | half;
        dst->flags &= (uint16_t)~((hi_half ? (PGXP_F_VY | PGXP_F_DY) : (PGXP_F_VX | PGXP_F_DX)) |
                                  PGXP_F_VZ | PGXP_F_PROJECTION);
        dst->z = 0;
        int ok = 0, derived = 0;
        int32_t v16 = 0;
        /* Hooks pass either the full register or just the stored half (the
         * interpreters), so the scalar is matched on the stored half. */
        if (s_cpu_mode && rt != 0 && s_gpr_s[rt].gen == s_gen &&
            (s_gpr_s[rt].value & 0xFFFFu) == half)
            v16 = scalar_to_half(s_gpr_s[rt].v, (int16_t)half, &ok, &derived);
        if (ok) {
            /* A scalar (e.g. a midpoint computed by MVMVA + DIV) becomes a
             * coordinate half here. */
            if (hi_half) { dst->y16 = v16; dst->flags |= (uint16_t)(PGXP_F_VY | (derived ? PGXP_F_DY : 0)); }
            else         { dst->x16 = v16; dst->flags |= (uint16_t)(PGXP_F_VX | (derived ? PGXP_F_DX : 0)); }
        } else if (src && src->gen == s_gen && (src->flags & PGXP_F_VX) &&
                   ((src->value ^ value) & 0xFFFFu) == 0) {
            const uint16_t d = (src->flags & PGXP_F_DX) ? (hi_half ? PGXP_F_DY : PGXP_F_DX) : 0;
            if (hi_half) { dst->y16 = src->x16; dst->flags |= (uint16_t)(PGXP_F_VY | d); }
            else         { dst->x16 = src->x16; dst->flags |= (uint16_t)(PGXP_F_VX | d); }
        }
        note_store(0x29, rt, src, addr, half, dst);
        return;
    }
    case 0x28: {                               /* SB                          */
        if (dst->gen != s_gen) return;         /* nothing tracked: stay dead  */
        uint32_t shift = (addr & 3u) * 8u;
        dst->value = (dst->value & ~(0xFFu << shift)) |
                     ((value & 0xFFu) << shift);
        dst->flags &= (uint16_t)~(((addr & 2u) ? (PGXP_F_VY | PGXP_F_DY) : (PGXP_F_VX | PGXP_F_DX)) |
                                  PGXP_F_VZ | PGXP_F_PROJECTION);
        dst->z = 0;
        return;
    }
    default:                                   /* SWL/SWR: forget the word    */
        pv_kill(dst);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Memory-mode hooks: COP2 transfers                                          */
/* ------------------------------------------------------------------------- */

extern "C" void psx_pgxp_cop2(struct CPUState *cpu, uint32_t instr,
                              uint32_t value, uint32_t addr) {
    (void)cpu;
    if (!g_pgxp_active) return;

    switch (f_op(instr)) {
    case 0x32: {                               /* LWC2: gte[rt] <- [addr]     */
        PGXPValue *src = pgxp_ptr_w(addr);     /* validation may mutate it    */
        PGXPValue *dst = &s_gte[f_rt(instr)];
        if (src && src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        sc_kill(&s_gte_s[f_rt(instr)]);        /* memory carries no scalars   */
        gte_sxy2_mirror(f_rt(instr));
        return;
    }
    case 0x3A: {                               /* SWC2: [addr] <- gte[rt]     */
        PGXPValue *dst = pgxp_ptr_w(addr);
        if (!dst) return;
        PGXPValue *src = &s_gte[f_rt(instr)];
        const PGXPValue before = *src;
        if (src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        note_store(0x3A, f_rt(instr), &before, addr, value, dst);
        return;
    }
    case 0x12: {                               /* COP2 register transfers     */
        switch (f_rs(instr)) {
        case 0x00: {                           /* MFC2: gpr[rt] <- gte[rd]    */
            uint32_t rt = f_rt(instr);
            if (rt == 0) return;
            PGXPValue *src = &s_gte[f_rd(instr)];
            if (src->gen == s_gen) {
                pv_validate(src, value);
                s_gpr[rt] = *src;
            } else {
                pv_reset(&s_gpr[rt], value);
            }
            /* MFC2 of MAC / IR carries an MVMVA result's scalar. */
            if (s_cpu_mode && sc_valid(&s_gte_s[f_rd(instr)], value))
                s_gpr_s[rt] = s_gte_s[f_rd(instr)];
            else
                gpr_written(rt);
            return;
        }
        case 0x04: {                           /* MTC2: gte[rd] <- gpr[rt]    */
            uint32_t rt = f_rt(instr);
            uint32_t rd = f_rd(instr);
            PGXPValue *dst = &s_gte[rd];
            if (rt != 0 && s_gpr[rt].gen == s_gen) {
                pv_validate(&s_gpr[rt], value);
                *dst = s_gpr[rt];
            } else {
                pv_reset(dst, value);
            }
            /* IR0..IR3 hold the low half sign-extended: a scalar that fits
             * a half moves in as the register's precise value. */
            sc_kill(&s_gte_s[rd]);
            if (s_cpu_mode && rd >= 8 && rd <= 11 && rt != 0 &&
                sc_valid(&s_gpr_s[rt], value) &&
                (int32_t)value == (int32_t)(int16_t)(value & 0xFFFFu))
                sc_set(&s_gte_s[rd], s_gpr_s[rt].v, value);
            /* An SXYP write (rd==15) already shifted the FIFO shadows
             * (pgxp_gte_reg_written, from the GTE register write). */
            gte_sxy2_mirror(rd);
            return;
        }
        case 0x02: {                           /* CFC2: gpr[rt] <- gtc[rd]    */
            uint32_t rt = f_rt(instr);
            if (rt == 0) return;
            uint32_t rd = f_rd(instr);
            if (s_cpu_mode && s_gtc[rd].gen == s_gen) {
                pv_validate(&s_gtc[rd], value);
                s_gpr[rt] = s_gtc[rd];
            } else {
                pv_reset(&s_gpr[rt], value);
            }
            if (s_cpu_mode && sc_valid(&s_gtc_s[rd], value))
                s_gpr_s[rt] = s_gtc_s[rd];
            else
                gpr_written(rt);
            return;
        }
        case 0x06: {                           /* CTC2: gtc[rd] <- gpr[rt]    */
            /* Control registers are written only by CTC2, so their shadows
             * stay exact until the next CTC2: matrix rows packed from two
             * tracked halves (the GTE-as-multiplier idiom) and translation
             * words carrying a scalar. */
            uint32_t rt = f_rt(instr);
            uint32_t rd = f_rd(instr);
            if (s_cpu_mode && rt != 0 && s_gpr[rt].gen == s_gen) {
                pv_validate(&s_gpr[rt], value);
                s_gtc[rd] = s_gpr[rt];
            } else {
                pv_reset(&s_gtc[rd], value);
            }
            if (s_cpu_mode && rt != 0 && sc_valid(&s_gpr_s[rt], value))
                s_gtc_s[rd] = s_gpr_s[rt];
            else
                sc_kill(&s_gtc_s[rd]);
            return;
        }
        default:
            return;
        }
    }
    default:
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* CPU-mode hooks: arithmetic (tier 2, default off)                           */
/* ------------------------------------------------------------------------- */

/* One 16-bit operand component in 16.16: a tracked half contributes its
 * sub-pixel value; an untracked half contributes its exact integer value
 * (adding an exact offset to a tracked coordinate keeps the fraction). */
static inline int32_t comp16(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (pv && pv->gen == s_gen) {
        if (!hi_half && (pv->flags & PGXP_F_VX) &&
            ((pv->value ^ value) & 0x0000FFFFu) == 0)
            return pv->x16;
        if (hi_half && (pv->flags & PGXP_F_VY) &&
            ((pv->value ^ value) & 0xFFFF0000u) == 0)
            return pv->y16;
    }
    return ((int32_t)(int16_t)(hi_half ? (value >> 16) : value)) << 16;
}

/* The derived-half marks (PGXP_F_DX / DY) an operand contributes through
 * comp16: a tracked derived half keeps its rounding window in the sum. */
static inline uint16_t derived_bits(const PGXPValue *pv, uint32_t value) {
    if (!pv || pv->gen != s_gen) return 0;
    uint16_t d = 0;
    if ((pv->flags & PGXP_F_DX) && (pv->flags & PGXP_F_VX) &&
        ((pv->value ^ value) & 0x0000FFFFu) == 0) d |= PGXP_F_DX;
    if ((pv->flags & PGXP_F_DY) && (pv->flags & PGXP_F_VY) &&
        ((pv->value ^ value) & 0xFFFF0000u) == 0) d |= PGXP_F_DY;
    return d;
}

static inline int comp_tracked(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (!pv || pv->gen != s_gen) return 0;
    if (!hi_half)
        return (pv->flags & PGXP_F_VX) &&
               ((pv->value ^ value) & 0x0000FFFFu) == 0;
    return (pv->flags & PGXP_F_VY) &&
           ((pv->value ^ value) & 0xFFFF0000u) == 0;
}

/* Half-wise add/sub is only meaningful when the guest result shows no carry
 * crossed the half boundary; otherwise the halves did not combine
 * independently and the shadow must not pretend they did. */
static inline int halves_independent(uint32_t a, uint32_t b, uint32_t r, int sub) {
    uint32_t lo = sub ? ((a & 0xFFFFu) - (b & 0xFFFFu))
                      : ((a & 0xFFFFu) + (b & 0xFFFFu));
    return ((lo ^ r) & 0xFFFFu) == 0 &&
           ((((sub ? a - b : a + b)) ^ r) == 0) &&
           ((lo >> 16) == 0);                  /* no carry/borrow out of low  */
}

/* Bitwise ops (AND/OR/XOR/NOR and ANDI/ORI/XORI) move bits; they do not
 * compute coordinates. GP0 decodes only the 11-bit field of each vertex half
 * (bits 0..10 of X, 16..26 of Y), and engines pack clip / outcode bits into
 * the rest of the word after projecting (Spider-Man ORs off-screen flags into
 * bits 14/15 of each half of SXY2 before storing it). A result half whose GPU
 * field equals the field of a tracked operand half therefore still describes
 * that vertex. Carrying it is exact - no arithmetic, like a move - so it runs
 * in both tiers. The carried value keeps the shadow invariant (integer part
 * == the half read as int16): the fraction rides along, the integer becomes
 * the result half. The GPU consumer rebases onto the field it decodes. */
static inline int half_exact(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (!comp_tracked(pv, value, hi_half)) return 0;
    int32_t v16 = hi_half ? pv->y16 : pv->x16;
    const int32_t half = (int32_t)(int16_t)(hi_half ? (value >> 16) : value);
    /* A derived half keeps its rounding window around the integer. */
    if (pv->flags & (hi_half ? PGXP_F_DY : PGXP_F_DX)) {
        const int64_t d = (int64_t)v16 - (int64_t)half * 65536;
        return d > -65536 && d < 2 * 65536;
    }
    /* A saturated projection shadow sits beyond the clamped word; it is not
     * the value of that half and must not be carried as one. */
    if (s_preserve_projection) {
        const int32_t field = (int32_t)((uint32_t)half << 21) >> 21;
        const int64_t d = (int64_t)v16 - (int64_t)half * 65536;
        // Clip metadata uses bits 13..15. Bits 11/12 must still be the
        // coordinate's sign fill; a real wrapped coordinate stays strict.
        const uint16_t sign_fill = field < 0 ? 0x1800u : 0;
        if (field > -0x400 && field < 0x3FF &&
            (uint16_t(half) & 0x1800u) == sign_fill)
            return d > -(int64_t)PGXP_PPP_AGREE_BELOW * 65536 &&
                   d < (int64_t)PGXP_PPP_AGREE_ABOVE * 65536;
    }
    return (v16 >> 16) == half;
}

static inline int carry_field(PGXPValue *dst, const PGXPValue *src,
                              uint32_t sval, uint32_t result, int hi_half) {
    if (!half_exact(src, sval, hi_half)) return 0;
    uint32_t sh = hi_half ? 16u : 0u;
    if ((((sval ^ result) >> sh) & 0x7FFu) != 0) return 0;
    /* Keep the precise offset from the operand's integer and rebase it on
     * the result half's integer (equal field, flag bits may differ). */
    const int32_t from = (int32_t)(int16_t)(sval >> sh);
    const int32_t to = (int32_t)(int16_t)(result >> sh);
    const int32_t v = (int32_t)((int64_t)(hi_half ? src->y16 : src->x16) +
                                ((int64_t)to - from) * 65536);
    const uint16_t d = src->flags & (hi_half ? PGXP_F_DY : PGXP_F_DX);
    if (hi_half) { dst->y16 = v; dst->flags |= (uint16_t)(PGXP_F_VY | d); }
    else         { dst->x16 = v; dst->flags |= (uint16_t)(PGXP_F_VX | d); }
    return 1;
}

static inline int has_depth(const PGXPValue *pv) {
    return pv && pv->gen == s_gen && (pv->flags & PGXP_F_VZ) && pv->z != 0;
}

/* Depth belongs to a whole vertex: it survives an op only when both halves of
 * the result came from one operand that carried it, and the other operand is
 * not itself a vertex (a constant, a mask, an offset). */
static inline void keep_depth(PGXPValue *dst, const PGXPValue *p,
                              const PGXPValue *other) {
    if ((dst->flags & PGXP_F_VXY) == PGXP_F_VXY && has_depth(p) &&
        !has_depth(other)) {
        dst->z = p->z;
        dst->flags |= PGXP_F_VZ;
    }
}

static void alu_bitwise(PGXPValue *dst, const PGXPValue *a, uint32_t s1,
                        const PGXPValue *b, uint32_t s2, uint32_t result) {
    pv_reset(dst, result);
    /* Prefer the operand that is a whole vertex, so a vertex ORed with flags
     * keeps its depth even when the flag word happens to share a field. */
    const PGXPValue *p = a; uint32_t pval = s1;
    const PGXPValue *q = b; uint32_t qval = s2;
    if (!has_depth(a) && has_depth(b)) { p = b; pval = s2; q = a; qval = s1; }
    const PGXPValue *from[2] = { nullptr, nullptr };
    for (int hi = 0; hi < 2; hi++) {
        if (carry_field(dst, p, pval, result, hi))      from[hi] = p;
        else if (carry_field(dst, q, qval, result, hi)) from[hi] = q;
        else if (((hi ? result >> 16 : result) & 0xFFFFu) == 0) {
            /* An exactly-zero half is an exact coordinate (the repack
             * pattern ORs a shifted half into a zero half). */
            if (hi) { dst->y16 = 0; dst->flags |= PGXP_F_VY; }
            else    { dst->x16 = 0; dst->flags |= PGXP_F_VX; }
        }
    }
    if (from[0] && from[0] == from[1])
        keep_depth(dst, from[0], from[0] == p ? q : p);
}

static const PGXPValue kNoShadow = { 0, 0, 0, 0, 0, 0 };

/* Snapshot a source GPR shadow: the destination may alias a source (in-place
 * ops such as `or t2, t2, t9` or `addiu t0, t0, 4`), and it is reset before
 * the sources are read. */
static inline const PGXPValue *gpr_shadow(uint32_t r, PGXPValue *copy) {
    *copy = (r != 0) ? s_gpr[r] : kNoShadow;
    return copy;
}

/* Scalar tier for one ALU op (cpu-mode): the destination register and the
 * precise value of the result, when an operand carries a precise value and the
 * op is exact arithmetic on it (no 32-bit wrap). Operands follow the hook
 * convention: ADD-type s1 = rs, s2 = rt; immediate s1 = rs, s2 = imm; shifts
 * s1 = rt, s2 = shamt (or rs for the variable forms); MF/MT HI/LO s1 = the
 * moved value. Computed before the half tier resets the destination, which
 * may alias a source. */
static int alu_scalar(uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2,
                      int *dst_reg, double *out) {
    const uint32_t op = f_op(instr);
    int pa = 0, pb = 0;
    double a, b;
    *dst_reg = -1;
    if (op == 0) {
        const uint32_t funct = f_funct(instr);
        const uint32_t rs = f_rs(instr), rt = f_rt(instr), rd = f_rd(instr);
        switch (funct) {
        case 0x10: case 0x12: {                /* MFHI / MFLO                 */
            const PGXPScalar *hl = &s_gpr_s[funct == 0x10 ? PGXP_REG_HI : PGXP_REG_LO];
            *dst_reg = (int)rd;
            if (!sc_valid(hl, result)) return 0;
            *out = hl->v;
            return 1;
        }
        case 0x11: case 0x13:                  /* MTHI / MTLO                 */
            *dst_reg = funct == 0x11 ? PGXP_REG_HI : PGXP_REG_LO;
            a = gpr_scalar(rs, result, 0, &pa);
            if (!pa) return 0;
            *out = a;
            return 1;
        case 0x00: case 0x02: case 0x03:       /* SLL / SRL / SRA             */
        case 0x04: case 0x06: case 0x07: {     /* SLLV / SRLV / SRAV          */
            *dst_reg = (int)rd;
            const uint32_t sh = s2 & 31u;      /* shamt, or rs for the V forms */
            a = gpr_scalar(rt, s1, 0, &pa);
            if (!pa) return 0;
            const int left = (funct & 3u) == 0;
            const int logical = (funct & 3u) == 2;
            if (left) {
                const int64_t exact = (int64_t)(int32_t)s1 * ((int64_t)1 << sh);
                if (exact != (int64_t)(int32_t)result) return 0;   /* wrapped     */
                *out = std::ldexp(a, (int)sh);
                return 1;
            }
            if (logical && (int32_t)s1 < 0) return 0;  /* not a division      */
            *out = std::ldexp(a, -(int)sh);    /* floor(a/2^sh) is the guest  */
            return 1;
        }
        case 0x20: case 0x21: case 0x22: case 0x23: {  /* ADD(U) / SUB(U)     */
            *dst_reg = (int)rd;
            const int sub = funct >= 0x22;
            a = gpr_scalar(rs, s1, 0, &pa);
            b = gpr_scalar(rt, s2, 0, &pb);
            if (!pa && !pb) return 0;
            const int64_t exact = sub ? (int64_t)(int32_t)s1 - (int64_t)(int32_t)s2
                                      : (int64_t)(int32_t)s1 + (int64_t)(int32_t)s2;
            if (exact != (int64_t)(int32_t)result) return 0;
            *out = sub ? a - b : a + b;
            return 1;
        }
        case 0x25:                             /* OR: only the MOVE idiom     */
            *dst_reg = (int)rd;
            if (rs != 0 && rt != 0) return 0;
            a = gpr_scalar(rs != 0 ? rs : rt, result, 0, &pa);
            if (!pa) return 0;
            *out = a;
            return 1;
        default:
            *dst_reg = (int)rd;
            return 0;
        }
    }
    *dst_reg = (int)f_rt(instr);
    if (op == 0x08 || op == 0x09) {            /* ADDI / ADDIU                */
        a = gpr_scalar(f_rs(instr), s1, 0, &pa);
        if (!pa) return 0;
        const int64_t exact = (int64_t)(int32_t)s1 + (int64_t)f_simm(instr);
        if (exact != (int64_t)(int32_t)result) return 0;
        *out = a + (double)f_simm(instr);
        return 1;
    }
    return 0;
}

static void alu_halves(uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);

extern "C" void psx_pgxp_alu(struct CPUState *cpu, uint32_t instr,
                             uint32_t result, uint32_t s1, uint32_t s2) {
    (void)cpu;
    if (!g_pgxp_active) return;
    int dst = -1;
    double v = 0.0;
    const int has = s_cpu_mode && alu_scalar(instr, result, s1, s2, &dst, &v);
    alu_halves(instr, result, s1, s2);
    if (dst > 0 && dst < 34) {
        if (has) sc_set(&s_gpr_s[dst], v, result);
        else     sc_kill(&s_gpr_s[dst]);
    }
}

static void alu_halves(uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2) {
    uint32_t op = f_op(instr);
    PGXPValue ca, cb;

    if (op == 0) {                             /* SPECIAL                     */
        uint32_t dst_reg = f_rd(instr);
        uint32_t rs = f_rs(instr), rt = f_rt(instr);
        uint32_t funct = f_funct(instr);
        if (funct == 0x11 || funct == 0x13) {  /* MTHI / MTLO                 */
            PGXPValue *hl = &s_gpr[funct == 0x11 ? PGXP_REG_HI : PGXP_REG_LO];
            if (s_cpu_mode && rs != 0 && s_gpr[rs].gen == s_gen) {
                pv_validate(&s_gpr[rs], result);
                *hl = s_gpr[rs];
            } else pv_reset(hl, result);
            return;
        }
        if (dst_reg == 0) return;
        PGXPValue *dst = &s_gpr[dst_reg];

        switch (funct) {
        case 0x10:                             /* MFHI                        */
        case 0x12: {                           /* MFLO                        */
            PGXPValue *hl = &s_gpr[funct == 0x10 ? PGXP_REG_HI : PGXP_REG_LO];
            if (s_cpu_mode && hl->gen == s_gen) {
                pv_validate(hl, result);
                *dst = *hl;
            } else pv_reset(dst, result);
            return;
        }
        case 0x00: case 0x02: case 0x03:       /* SLL / SRL / SRA             */
        case 0x04: case 0x06: case 0x07: {     /* SLLV / SRLV / SRAV          */
            uint32_t sh = (funct < 4) ? f_shamt(instr) : (s2 & 31u);
            const PGXPValue *src = gpr_shadow(rt, &ca);
            int right = (funct & 3u) != 0;
            pv_reset(dst, result);
            if (!s_cpu_mode || sh != 16 || src->gen != s_gen)
                return;
            if (!right) {                      /* << 16: low comp -> Y        */
                if (comp_tracked(src, s1, 0)) {
                    dst->y16 = src->x16;
                    dst->flags |= PGXP_F_VY;
                    if (src->flags & PGXP_F_DX) dst->flags |= PGXP_F_DY;
                }
                dst->x16 = 0;
                dst->flags |= PGXP_F_VX;       /* low half exactly zero       */
            } else {                           /* >> 16: high comp -> X       */
                if (comp_tracked(src, s1, 1)) {
                    dst->x16 = src->y16;
                    dst->flags |= PGXP_F_VX;
                    if (src->flags & PGXP_F_DY) dst->flags |= PGXP_F_DX;
                }
                dst->y16 = ((int32_t)result >> 16) << 16;  /* 0 or sign fill  */
                dst->flags |= PGXP_F_VY;
            }
            return;
        }
        case 0x24: case 0x25: case 0x26: case 0x27: {  /* AND/OR/XOR/NOR     */
            const PGXPValue *a = gpr_shadow(rs, &ca);
            const PGXPValue *b = gpr_shadow(rt, &cb);
            /* OR with $zero is the MOVE idiom: the whole shadow, depth too. */
            if (funct == 0x25 && (rs == 0 || rt == 0)) {
                const PGXPValue *m = (rt == 0) ? a : b;
                if (m->gen == s_gen) {
                    *dst = *m;
                    pv_validate(dst, result);
                } else pv_reset(dst, result);
                return;
            }
            alu_bitwise(dst, a, s1, b, s2, result);
            return;
        }
        case 0x20: case 0x21:                  /* ADD / ADDU                  */
        case 0x22: case 0x23: {                /* SUB / SUBU                  */
            const PGXPValue *a = gpr_shadow(rs, &ca);
            const PGXPValue *b = gpr_shadow(rt, &cb);
            int is_sub = (funct == 0x22 || funct == 0x23);
            /* MOVE idioms first - exact, so they run in both tiers. */
            if (rt == 0 || (!is_sub && rs == 0)) {
                const PGXPValue *m = (rt == 0) ? a : b;
                if (m->gen == s_gen) {
                    *dst = *m;
                    pv_validate(dst, result);
                } else pv_reset(dst, result);
                return;
            }
            if (!s_cpu_mode) { pv_reset(dst, result); return; }
            /* add/sub: require at least one tracked component and halves
             * that combined independently (no cross-half carry). */
            if (!halves_independent(s1, s2, result, is_sub) ||
                (!comp_tracked(a, s1, 0) && !comp_tracked(a, s1, 1) &&
                 !comp_tracked(b, s2, 0) && !comp_tracked(b, s2, 1))) {
                pv_reset(dst, result);
                return;
            }
            pv_reset(dst, result);
            dst->x16 = is_sub ? comp16(a, s1, 0) - comp16(b, s2, 0)
                              : comp16(a, s1, 0) + comp16(b, s2, 0);
            dst->y16 = is_sub ? comp16(a, s1, 1) - comp16(b, s2, 1)
                              : comp16(a, s1, 1) + comp16(b, s2, 1);
            dst->flags = (uint16_t)(PGXP_F_VXY | derived_bits(a, s1) | derived_bits(b, s2));
            /* A vertex plus / minus an offset is still that vertex. */
            if (comp_tracked(a, s1, 0) && comp_tracked(a, s1, 1))
                keep_depth(dst, a, b);
            else if (!is_sub && comp_tracked(b, s2, 0) && comp_tracked(b, s2, 1))
                keep_depth(dst, b, a);
            return;
        }
        default:                               /* SLT/SLTU/...: not a vertex  */
            pv_reset(dst, result);
            return;
        }
    }

    /* immediates */
    uint32_t dst_reg = f_rt(instr);
    if (dst_reg == 0) return;
    PGXPValue *dst = &s_gpr[dst_reg];
    const PGXPValue *a = gpr_shadow(f_rs(instr), &ca);

    switch (op) {
    case 0x0F: {                               /* LUI: both halves exact      */
        pv_reset(dst, result);
        dst->x16 = 0;
        dst->y16 = ((int32_t)result >> 16) << 16;
        dst->flags = PGXP_F_VXY;
        return;
    }
    case 0x08: case 0x09: {                    /* ADDI / ADDIU                */
        int32_t imm = f_simm(instr);
        if (imm == 0) {                        /* MOVE idiom                  */
            if (a->gen == s_gen) {
                *dst = *a;
                pv_validate(dst, result);
            } else pv_reset(dst, result);
            return;
        }
        /* A negative immediate is a subtraction of its magnitude: checking
         * carry on the sign-extended form would reject nearly every -N. */
        int neg = imm < 0;
        uint32_t mag = (uint32_t)(neg ? -imm : imm);
        if (!s_cpu_mode ||
            !halves_independent(s1, mag, result, neg) ||
            (!comp_tracked(a, s1, 0) && !comp_tracked(a, s1, 1))) {
            pv_reset(dst, result);
            return;
        }
        int32_t dx = (int32_t)(mag & 0xFFFFu) << 16;
        pv_reset(dst, result);
        dst->x16 = comp16(a, s1, 0) + (neg ? -dx : dx);
        dst->y16 = comp16(a, s1, 1);           /* no carry crossed: Y untouched */
        dst->flags = (uint16_t)(PGXP_F_VXY | derived_bits(a, s1));
        if (comp_tracked(a, s1, 0) && comp_tracked(a, s1, 1))
            keep_depth(dst, a, nullptr);
        return;
    }
    case 0x0C: case 0x0D: case 0x0E: {         /* ANDI / ORI / XORI           */
        uint32_t imm = instr & 0xFFFFu;
        alu_bitwise(dst, a, s1, nullptr, imm, result);
        /* ORI into a zero low half writes an exact constant there (the
         * lui/ori and repack idioms). */
        if (op == 0x0D && !(dst->flags & PGXP_F_VX) && (s1 & 0xFFFFu) == 0) {
            dst->x16 = ((int32_t)(int16_t)imm) << 16;
            dst->flags |= PGXP_F_VX;
        }
        return;
    }
    default:                                   /* SLTI/SLTIU, MFC0/CFC0: reset */
        pv_reset(dst, result);
        return;
    }
}

extern "C" void psx_pgxp_muldiv(struct CPUState *cpu, uint32_t instr,
                                uint32_t hi, uint32_t lo,
                                uint32_t s1, uint32_t s2) {
    (void)cpu;
    if (!g_pgxp_active) return;
    /* Products/quotients of screen coordinates are not screen coordinates:
     * the half shadows of HI/LO are known-but-imprecise. The scalar tier
     * (cpu-mode) does carry a product or quotient when an operand is precise:
     * a weighted sum divided by its weight total IS a screen coordinate (the
     * perspective-correct midpoint of an edge), and it turns back into a
     * half when stored. */
    pv_reset(&s_gpr[PGXP_REG_HI], hi);
    pv_reset(&s_gpr[PGXP_REG_LO], lo);
    sc_kill(&s_gpr_s[PGXP_REG_HI]);
    sc_kill(&s_gpr_s[PGXP_REG_LO]);
    if (!s_cpu_mode) return;
    const uint32_t funct = f_funct(instr);
    const int is_unsigned = (funct & 1u) != 0;
    if (is_unsigned && ((int32_t)s1 < 0 || (int32_t)s2 < 0))
        return;                                /* stay within the signed range */
    int pa = 0, pb = 0;
    const double a = gpr_scalar(f_rs(instr), s1, 0, &pa);
    const double b = gpr_scalar(f_rt(instr), s2, 0, &pb);
    if (!pa && !pb) return;
    if (funct == 0x18 || funct == 0x19) {      /* MULT / MULTU                */
        const int64_t exact = (int64_t)(int32_t)s1 * (int64_t)(int32_t)s2;
        if (exact != (int64_t)(int32_t)lo) return;   /* HI is not a sign fill  */
        sc_set(&s_gpr_s[PGXP_REG_LO], a * b, lo);
        return;
    }
    if (funct == 0x1A || funct == 0x1B) {      /* DIV / DIVU                  */
        if (s2 == 0 || b == 0.0) return;       /* divide by zero: no quotient */
        if (s1 == 0x80000000u && s2 == 0xFFFFFFFFu) return;   /* overflow     */
        sc_set(&s_gpr_s[PGXP_REG_LO], a / b, lo);
    }
}

/* ------------------------------------------------------------------------- */
/* GTE producer                                                               */
/* ------------------------------------------------------------------------- */

/* A tracked half as the precise value of the 16-bit integer the GTE used:
 * the shadow must describe that integer (its half of `value`), and the
 * precise value must floor to it, or lie in the derived window when the half
 * came out of rounding guest arithmetic. */
static inline int half_precise(const PGXPValue *pv, int hi, int16_t used, double *out) {
    if (!pv || pv->gen != s_gen) return 0;
    if (!(pv->flags & (hi ? PGXP_F_VY : PGXP_F_VX))) return 0;
    const uint16_t half = hi ? (uint16_t)(pv->value >> 16) : (uint16_t)(pv->value & 0xFFFFu);
    if ((int16_t)half != used) return 0;
    const int32_t v16 = hi ? pv->y16 : pv->x16;
    const double v = v16 / 65536.0, d = v - (double)used;
    if (pv->flags & (hi ? PGXP_F_DY : PGXP_F_DX)) {
        if (!(d > -1.0 && d < 2.0)) return 0;
    } else if ((v16 >> 16) != (int32_t)used) {
        return 0;
    }
    *out = v;
    return 1;
}

/* After a GTE command: every command except NCLIP / AVSZ3 / AVSZ4 rewrites
 * IR0..3 / MAC1..3, so their scalars end there. MVMVA settles its own
 * results (pgxp_gte_mvmva, before this runs). MAC0 never carries one. */
extern "C" void pgxp_gte_op_end(uint32_t func) {
    sc_kill(&s_gte_s[24]);
    if (func == 0x06 || func == 0x2D || func == 0x2E || func == 0x12) return;
    for (int r = 8; r <= 11; r++)  sc_kill(&s_gte_s[r]);
    for (int r = 25; r <= 27; r++) sc_kill(&s_gte_s[r]);
}

static void mvmva_results_reset(const PGXPMvmva *op) {
    /* The data-register shadows of the results describe the old values. */
    for (int r = 0; r < 3; r++) {
        pv_reset(&s_gte[25 + r], (uint32_t)op->mac[r]);
        pv_reset(&s_gte[9 + r], (uint32_t)op->ir[r]);
        sc_kill(&s_gte_s[25 + r]);
        sc_kill(&s_gte_s[9 + r]);
    }
}

extern "C" void pgxp_gte_mvmva(const PGXPMvmva *op) {
    if (!g_pgxp_active) return;
    if (!s_cpu_mode || op->mx > 2 || op->tv == 2) {        /* reserved / FC bug */
        mvmva_results_reset(op);
        return;
    }
    static const int kBase[3] = { 0, 8, 16 };             /* RT, LLM, LCM     */
    double m[3][3], v[3], t[3];
    int any = 0;
    for (int k = 0; k < 9; k++) {
        const int r = k / 3, c = k % 3;
        const PGXPValue *pv = &s_gtc[kBase[op->mx] + (k >> 1)];
        double p;
        if (half_precise(pv, k & 1, op->m[r][c], &p)) { m[r][c] = p; any = 1; }
        else m[r][c] = (double)op->m[r][c];
    }
    for (int c = 0; c < 3; c++) {
        double p;
        int pr = 0;
        if (op->vv < 3) {
            const PGXPValue *pv = &s_gte[2 * op->vv + (c == 2 ? 1 : 0)];
            pr = half_precise(pv, c == 1, op->v[c], &p);
        } else {
            p = reg_scalar(&s_gte_s[9 + c], &s_gte[9 + c],
                           (uint32_t)(int32_t)op->v[c], 0, &pr);
        }
        v[c] = pr ? p : (double)op->v[c];
        any |= pr;
    }
    for (int r = 0; r < 3; r++) {
        t[r] = (double)op->t[r];
        if (op->tv > 1) continue;
        const int reg = (op->tv == 0 ? 5 : 13) + r;      /* TR / BK words     */
        int pr = 0;
        const double p = reg_scalar(&s_gtc_s[reg], &s_gtc[reg],
                                    (uint32_t)(int32_t)(op->t[r] >> 12), 0, &pr);
        if (pr) { t[r] = p * 4096.0; any = 1; }
    }
    /* Operands are read (the vector may be IR itself); now the results. */
    mvmva_results_reset(op);
    if (!any) return;
    /* FLAG: MAC1/2/3 positive overflow bits 30/29/28, negative 27/26/25;
     * IR1/2/3 saturation bits 24/23/22 (psx-spx GTE FLAG). */
    static const uint32_t kMacOverflow[3] = { (1u << 30) | (1u << 27),
                                              (1u << 29) | (1u << 26),
                                              (1u << 28) | (1u << 25) };
    static const uint32_t kIrSat[3] = { 1u << 24, 1u << 23, 1u << 22 };
    for (int r = 0; r < 3; r++) {
        if (op->flag & kMacOverflow[r]) continue;
        /* The integers reported must reproduce the guest MAC: proves the
         * operands handed over are the ones the GTE used. */
        const int64_t exact = op->t[r] + (int64_t)op->m[r][0] * op->v[0] +
                              (int64_t)op->m[r][1] * op->v[1] +
                              (int64_t)op->m[r][2] * op->v[2];
        if ((exact >> op->shift) != (int64_t)op->mac[r]) continue;
        const double p = (t[r] + m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2]) /
                         (op->shift ? 4096.0 : 1.0);
        sc_set(&s_gte_s[25 + r], p, (uint32_t)op->mac[r]);
        if (!(op->flag & kIrSat[r]) && op->ir[r] == op->mac[r])
            sc_set(&s_gte_s[9 + r], p, (uint32_t)op->ir[r]);
    }
}

extern "C" int pgxp_project_precise(int64_t mac1, int64_t mac2, int64_t mac3,
                                    int shift, int32_t ir1, int32_t ir2,
                                    uint32_t sz3, uint32_t h,
                                    int32_t ofx, int32_t ofy,
                                    int64_t x_num, int64_t x_den,
                                    int32_t *x16, int32_t *y16) {
    /* Qualify exactly the vertices whose guest projection is the plain
     * IR * (H / SZ3) of the continuous one: sf=1, no IR1/IR2 clamp (the
     * stored IR equals MAC >> 12), SZ3 unclamped and nonzero, and no UNR
     * divide overflow (the GTE saturates the quotient when H >= 2*SZ3). */
    if (shift != 12 || x_den <= 0) return 0;
    if ((mac1 >> 12) != (int64_t)ir1 || (mac2 >> 12) != (int64_t)ir2) return 0;
    const int64_t z_int = mac3 >> 12;
    if (z_int <= 0 || z_int > 0xFFFF || (uint32_t)z_int != sz3) return 0;
    if ((int64_t)h >= 2 * z_int) return 0;
    /* (MAC/4096) * H / (MAC3/4096): the 4096s cancel. Double keeps ~52 bits,
     * far below the 1/65536 px the shadow carries. Host-only and visual-only,
     * so cross-platform bit equality of this value is not required. */
    const double k = (double)h * 65536.0 / (double)mac3;
    double fx = (double)ofx +
                (double)mac1 * k * (double)x_num / (double)x_den;
    double fy = (double)ofy + (double)mac2 * k;
    /* Same transport bound as the IR path (gte.cpp): an extreme value must
     * not wrap an int32 and masquerade as an on-screen shadow. */
    const double lim = 4096.0 * 65536.0;
    if (fx < -lim) fx = -lim;
    if (fx > lim - 1.0) fx = lim - 1.0;
    if (fy < -lim) fy = -lim;
    if (fy > lim - 1.0) fy = lim - 1.0;
    *x16 = (int32_t)std::floor(fx);
    *y16 = (int32_t)std::floor(fy);
    return 1;
}

/* Defined below with the GPU consumer. */
static inline int pgxp_agrees(int32_t p16, int32_t native, int16_t half,
                              int derived);

extern "C" int pgxp_ppp_accept(int32_t x16, int32_t y16, uint32_t packed) {
    const int16_t hx = (int16_t)(packed & 0xFFFFu);
    const int16_t hy = (int16_t)(packed >> 16);
    if (pgxp_agrees(x16, hx, hx, 0) && pgxp_agrees(y16, hy, hy, 0)) {
        s_stats.ppp_produced++;
        return 1;
    }
    s_stats.ppp_window_fallback++;
    return 0;
}

extern "C" void pgxp_gte_push_sxy(int32_t x16, int32_t y16, uint16_t sz3,
                                  uint32_t packed) {
    if (!g_pgxp_active) return;
    s_stats.produced++;
    s_gte[12] = s_gte[13];
    s_gte[13] = s_gte[14];
    PGXPValue *pv = &s_gte[14];
    pv->x16 = x16;
    pv->y16 = y16;
    pv->z = sz3;
    pv->flags = (uint16_t)(PGXP_F_VXY | (sz3 != 0 ? PGXP_F_VZ : 0));
    pv->value = packed;
    pv->gen = s_gen;
    s_gte[15] = *pv;                           /* SXYP mirrors SXY2           */
}

extern "C" int pgxp_get_gte_sxy(uint32_t index, int32_t *x16, int32_t *y16) {
    return pgxp_get_gte_sxy_checked(index, 0u, 0, x16, y16);
}

extern "C" void pgxp_gte_set_projection(const PGXPProjection *projection) {
    if (!g_pgxp_active || !s_projection_tracking || !projection) return;
    s_gte[14].projection = *projection;
    s_gte[14].flags |= PGXP_F_PROJECTION;
    s_gte[15] = s_gte[14];
}
static int projection_read(const PGXPValue *pv, uint32_t packed, PGXPProjection *out) {
    const uint16_t flags = PGXP_F_VXY | PGXP_F_PROJECTION;
    if (!s_enabled || !s_projection_tracking || !pv || pv->gen != s_gen || pv->value != packed ||
        (pv->flags & flags) != flags) return 0;
    if (out) *out = pv->projection;
    return 1;
}
extern "C" int pgxp_load_projection(uint32_t addr, uint32_t packed, PGXPProjection *out) {
    return projection_read(pgxp_ptr(addr), packed, out);
}
extern "C" int pgxp_get_gte_projection(uint32_t index, uint32_t packed, PGXPProjection *out) {
    return index < 4 ? projection_read(&s_gte[12+index], packed, out) : 0;
}

extern "C" int pgxp_get_gte_sxy_checked(uint32_t index, uint32_t expect,
                                        int check, int32_t *x16, int32_t *y16) {
    if (index >= 4) return 0;
    const PGXPValue *pv = &s_gte[12 + index];
    if (!pv_live(pv) || (pv->flags & PGXP_F_VXY) != PGXP_F_VXY)
        return 0;
    if (check && pv->value != expect)
        return 0;
    if (x16) *x16 = pv->x16;
    if (y16) *y16 = pv->y16;
    return 1;
}

/* Defined below with the GPU consumer. */
static inline int pgxp_accept(int32_t px, int32_t py, int32_t int_x,
                              int32_t int_y, uint32_t word, uint16_t flags);

extern "C" int pgxp_gte_nclip_precise(const uint32_t sxy[3], int64_t *cross) {
    if (!g_pgxp_active) return 0;
    int64_t x[3], y[3];
    for (int i = 0; i < 3; i++) {
        const PGXPValue *pv = &s_gte[12 + i];
        if (!pv_live(pv) || (pv->flags & PGXP_F_VXY) != PGXP_F_VXY ||
            pv->value != sxy[i])
            return 0;
        /* The GPU parses the packet half as the same signed integer for every
         * value the GTE can produce (its saturation limits are -0x400 and
         * 0x3FF), so the register half stands in for the native parse. */
        if (pgxp_accept(pv->x16, pv->y16, (int16_t)(sxy[i] & 0xFFFFu),
                        (int16_t)(sxy[i] >> 16), sxy[i], pv->flags) != 0)
            return 0;
        x[i] = pv->x16;
        y[i] = pv->y16;
    }
    /* NCLIP's determinant, sx0*(sy1-sy2) + sx1*(sy2-sy0) + sx2*(sy0-sy1),
     * on 16.16 positions. Positions are bounded to +-2^28 (the transport
     * clamp), so every product fits in 64 bits. */
    *cross = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
    s_stats.nclip_precise++;
    return 1;
}

extern "C" void pgxp_gte_reg_written(int reg, uint32_t value) {
    /* Invalidation-class bookkeeping: runs even with the engine disarmed so
     * seeded/leftover shadows can never outlive a guest register write. Only
     * the suppression bracket skips it (the speculative pass rolls the
     * machine state back, so its writes must not stick to the shadows). */
    if (s_suppress != 0) return;
    if (reg < 0 || reg > 31) return;
    if (reg == 15) {
        /* SXYP is a FIFO push: SXY1 moves to SXY0, SXY2 to SXY1, the value
         * lands in SXY2 (psx-spx GTE "SXYP"). Engines draw quads from a
         * projected-vertex table this way - load three corners, NCLIP, push
         * the fourth through SXYP, NCLIP, store SXY0..2 into the packet - so
         * the shadows shift with the registers. The MTC2 / LWC2 hook then
         * copies the source shadow into the pushed slot. */
        s_gte[12] = s_gte[13];
        s_gte[13] = s_gte[14];
        reg = 14;
    }
    pv_kill(&s_gte[reg]);
    pv_reset(&s_gte[reg], value);
    gte_sxy2_mirror(reg);                      /* SXY2 and SXYP: one register */
}

/* ------------------------------------------------------------------------- */
/* GPU consumer                                                               */
/* ------------------------------------------------------------------------- */

/* Truncation agreement for one axis. `native` is the GPU's parse of the
 * packet half, `half` the raw 16-bit half in the word. Exact (integer part
 * equals the native parse) unless preserve-projection is on; then the precise
 * position may sit a bounded distance from it (PGXP_PPP_AGREE_*), except
 * when the decoded coordinate is saturated or the word is a genuinely
 * wrapped coordinate. Known outcode bits 13..15 alone are not saturation;
 * bits 11/12 must still match the decoded coordinate's sign fill. */
static inline int pgxp_agrees(int32_t p16, int32_t native, int16_t half,
                              int derived) {
    const int64_t d = (int64_t)p16 - (int64_t)native * 65536;
    /* A derived half (PGXP_F_DX/DY): the guest integer is the rounding its
     * own arithmetic chose, so the value may sit one pixel either side of
     * the truncation window - still bounded, still the same vertex. */
    if (derived) return d > -65536 && d < 2 * 65536;
    const uint16_t sign_fill = native < 0 ? 0x1800u : 0;
    if (!s_preserve_projection || native <= -0x400 || native >= 0x3FF ||
        (uint16_t(half) & 0x1800u) != sign_fill)
        return (p16 >> 16) == native;
    return d > -(int64_t)PGXP_PPP_AGREE_BELOW * 65536 &&
           d < (int64_t)PGXP_PPP_AGREE_ABOVE * 65536;
}

/* Tolerance-clamp distance of one axis: how far the precise value moved the
 * vertex from the guest integer. For a derived half the first pixel above
 * the integer is the guest arithmetic's own rounding, not movement. */
static inline float pgxp_moved(int32_t p16, int32_t native, int derived) {
    const float d = (float)((int64_t)p16 - (int64_t)native * 65536) * (1.0f / 65536.0f);
    if (!derived) return std::fabs(d);
    return d < 0.0f ? -d : (d > 1.0f ? d - 1.0f : 0.0f);
}

/* The consumer-side safeguards on one candidate position: truncation
 * agreement on both axes, then the tolerance clamp. Returns 0 when accepted,
 * 1 for a truncation reject, 2 for a tolerance reject. Shared by the GPU
 * lookup and the precise NCLIP so culling and drawing believe exactly the
 * same vertices. `flags` are the shadow's (derived halves). */
static inline int pgxp_accept(int32_t px, int32_t py, int32_t int_x,
                              int32_t int_y, uint32_t word, uint16_t flags) {
    const int dx_derived = (flags & PGXP_F_DX) != 0;
    const int dy_derived = (flags & PGXP_F_DY) != 0;
    if (!pgxp_agrees(px, int_x, (int16_t)(word & 0xFFFFu), dx_derived) ||
        !pgxp_agrees(py, int_y, (int16_t)(word >> 16), dy_derived))
        return 1;
    if (s_tolerance >= 0.0f) {
        if (pgxp_moved(px, int_x, dx_derived) > s_tolerance ||
            pgxp_moved(py, int_y, dy_derived) > s_tolerance)
            return 2;
    }
    return 0;
}

/* Rebase a shadow that describes the int16 half onto GP0's 11-bit field when
 * the half carries flag bits ABOVE the field (Spider-Man packs clip flags in
 * bits 14/15). A half whose bits 11-15 are a plain sign extension holds an
 * ordinary number; if it overflowed the field (e.g. 0x0402 parses as -1022),
 * that is CPU arithmetic the GPU wraps, which pgxp_agrees() only believes on
 * exact integer agreement, so it is left to that check unchanged. */
static inline int32_t field_rebase(int32_t v16, uint32_t half, int32_t parsed) {
    int32_t as16 = (int16_t)half;
    int32_t as11 = ((int32_t)(half << 21)) >> 21;
    if (parsed != as11 || parsed == as16) return v16;
    const uint32_t above = (half >> 11) & 0x1Fu;
    if (above == ((half & 0x8000u) ? 0x1Fu : 0u)) return v16;
    return (int32_t)((int64_t)v16 + ((int64_t)as11 - as16) * 65536);
}

static int pgxp_get_precise_vertex_impl(uint32_t addr, uint32_t packet_word,
                                        int32_t int_x, int32_t int_y,
                                        int32_t *x16, int32_t *y16,
                                        uint16_t *sz, int probe) {
    /* A probe asks the same question without recording it (the rectangle
     * shortcut runs one per corner before drawing): it skips every counter
     * instead of copying the whole statistics record around the call. */
    const int count = !probe;
    if (count) s_stats.lookups++;

    int32_t px = 0, py = 0;
    uint16_t pz = 0;
    uint16_t pflags = 0;
    int have = 0;

    if (s_enabled && addr != 0xFFFFFFFFu) {
        PGXPValue *pv = pgxp_ptr(addr);
        if (pv && pv->gen == s_gen && (pv->flags & PGXP_F_VXY) != 0) {
            if (pv->value == packet_word &&
                (pv->flags & PGXP_F_VXY) == PGXP_F_VXY) {
                px = pv->x16;
                py = pv->y16;
                pz = (pv->flags & PGXP_F_VZ) ? pv->z : 0;
                pflags = pv->flags;
                have = PGXP_SRC_DATAFLOW;
            } else if (count) {
                s_stats.value_mismatch++;
            }
        }
    }

    const int fallback_hit = !have && s_position_fallback &&
        (probe ? gte_geometry_correction_lookup_probe(packet_word, &px, &py)
               : gte_geometry_correction_lookup(packet_word, &px, &py));
    if (fallback_hit) {
        pz = 0;                                /* fallback never carries depth */
        have = PGXP_SRC_FALLBACK;
    }

    if (have) {
        /* A shadow describes each half read as int16; GP0 decodes only its
         * 11-bit field (engines keep clip / outcode bits above it). When the
         * caller parsed the field, rebase the precise value onto it - the
         * fraction is unchanged. Any other integer is caught below. */
        px = field_rebase(px, packet_word & 0xFFFFu, int_x);
        py = field_rebase(py, packet_word >> 16, int_y);
        /* Truncation agreement: the GPU parsed 11-bit integers out of the
         * packet; a precise position whose integer part disagrees (a
         * wrapped/CPU-modified coordinate) must not be believed. Then the
         * tolerance clamp. */
        const int why = pgxp_accept(px, py, int_x, int_y, packet_word, pflags);
        if (why == 1) {
            if (count) s_stats.trunc_reject++;
            have = 0;
        } else if (why == 2) {
            if (count) s_stats.tolerance_reject++;
            have = 0;
        }
    }

    if (!have) {
        if (count) s_stats.native++;
        *x16 = int_x << 16;
        *y16 = int_y << 16;
        *sz = 0;
        return PGXP_SRC_NATIVE;
    }

    if (count) {
        if (have == PGXP_SRC_DATAFLOW) s_stats.dataflow_hit++;
        else                           s_stats.fallback_hit++;
        if (pz != 0) s_stats.w_valid++;
    }
    *x16 = px;
    *y16 = py;
    *sz = pz;
    return have;
}

extern "C" int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                                       int32_t int_x, int32_t int_y,
                                       int32_t *x16, int32_t *y16,
                                       uint16_t *sz) {
    return pgxp_get_precise_vertex_impl(addr, packet_word, int_x, int_y,
                                        x16, y16, sz, 0);
}

extern "C" int pgxp_probe_precise_vertex(uint32_t addr, uint32_t packet_word,
                                         int32_t int_x, int32_t int_y) {
    int32_t x16, y16;
    uint16_t sz;
    return pgxp_get_precise_vertex_impl(addr, packet_word, int_x, int_y,
                                        &x16, &y16, &sz, 1);
}

/* ------------------------------------------------------------------------- */
/* Legacy v14 SWC2 tracker — kept as the base-flavour feed                    */
/* ------------------------------------------------------------------------- */

/* Emitted at every swc2 since ABI v14 (all flavours, all backends, overlay
 * DLLs). In the pgxp flavour psx_pgxp_cop2 supersedes it for the same swc2 —
 * the double write is idempotent (same source shadow, same destination). */
extern "C" void pgxp_store_gte_reg(uint32_t addr, uint8_t reg) {
    if (!g_pgxp_active) return;
    PGXPValue *dst = pgxp_ptr_w(addr);
    if (!dst) return;
    const PGXPValue *src = &s_gte[reg & 31u];
    if (src->gen != s_gen) return;
    s_stats.swc2_stores++;
    *dst = *src;
}

extern "C" int pgxp_store_flagged_gte_sxy(uint32_t addr, uint8_t reg,
                                          uint32_t source_word, uint32_t stored_word) {
    if (!g_pgxp_active || (addr & 3u) || reg < 12 || reg > 15 ||
        ((source_word ^ stored_word) & 0x07FF07FFu)) return 0;
    const PGXPValue* src = &s_gte[reg];
    if (src->gen != s_gen || src->value != source_word || !src->z ||
        (src->flags & (PGXP_F_VXY | PGXP_F_VZ)) != (PGXP_F_VXY | PGXP_F_VZ)) return 0;
    const int64_t x = (int64_t)src->x16 +
        ((int64_t)(int16_t)stored_word - (int16_t)source_word) * 65536;
    const int64_t y = (int64_t)src->y16 +
        ((int64_t)(int16_t)(stored_word >> 16) - (int16_t)(source_word >> 16)) * 65536;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return 0;
    PGXPValue* dst = pgxp_ptr_w(addr);
    if (!dst) return 0;
    *dst = *src;
    dst->value = stored_word;
    dst->x16 = (int32_t)x;
    dst->y16 = (int32_t)y;
    return 1;
}

extern "C" int pgxp_debug_shadow(int space, uint32_t key, int *live,
                                 uint32_t *value, uint32_t *flags,
                                 int32_t *x16, int32_t *y16, uint16_t *z) {
    const PGXPValue *pv = nullptr;
    if (space == 0)                pv = pgxp_ptr(key);
    else if (space == 1 && key < 34) pv = &s_gpr[key];
    else if (space == 2 && key < 32) pv = &s_gte[key];
    if (!pv) return 0;
    *live = pv->gen == s_gen;
    *value = pv->value;
    *flags = pv->flags;
    *x16 = pv->x16;
    *y16 = pv->y16;
    *z = pv->z;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Test accessors                                                             */
/* ------------------------------------------------------------------------- */

extern "C" void pgxp_test_seed_gte_sxy(uint32_t index, uint32_t packed,
                                       int32_t x16, int32_t y16, uint16_t z,
                                       int valid) {
    if (index >= 4) return;
    PGXPValue *pv = &s_gte[12 + index];
    pv->x16 = x16;
    pv->y16 = y16;
    pv->z = z;
    pv->value = packed;
    pv->flags = (uint16_t)(PGXP_F_VXY | (z != 0 ? PGXP_F_VZ : 0));
    pv->gen = valid ? s_gen : 0;
}

extern "C" void pgxp_test_get_gte_sxy(uint32_t index, uint32_t *packed,
                                      int32_t *x16, int32_t *y16, uint16_t *z,
                                      uint8_t *valid) {
    if (index >= 4) return;
    const PGXPValue *pv = &s_gte[12 + index];
    if (packed) *packed = pv->value;
    if (x16) *x16 = pv->x16;
    if (y16) *y16 = pv->y16;
    if (z) *z = pv->z;
    if (valid) *valid = pv_live(pv) ? 1 : 0;
}

extern "C" uint32_t pgxp_test_generation(void) { return s_gen; }

extern "C" uint32_t pgxp_test_suppress_depth(void) { return s_suppress; }
extern "C" int pgxp_test_active(void) { return g_pgxp_active; }

extern "C" void pgxp_test_set_generation(uint32_t gen) { s_gen = gen; }

/* Address-keyed depth lookup for the shipped perspective-texturing path
 * (gpu.c prepare_texture_triangle). Same contract as the retired hashed
 * table: hit only when the tracked word matches the packet word exactly. */
/* Always-on ring of refused precise-word lookups: which packet word, what
 * the shadow at its address held, and why it was refused. Join it with
 * wtrace_dump (writer PC per address) to find the guest code path that drops
 * provenance. */
static PGXPWordMiss s_miss_ring[PGXP_MISS_RING_CAP];
static uint64_t     s_miss_seq = 0;

static void note_word_miss(uint32_t addr, uint32_t packed, const PGXPValue *pv,
                           uint8_t reason) {
    PGXPWordMiss *m = &s_miss_ring[s_miss_seq % PGXP_MISS_RING_CAP];
    m->seq = s_miss_seq++;
    m->addr = addr;
    m->packet = packed;
    m->shadow_value = pv ? pv->value : 0;
    m->shadow_flags = pv ? (uint8_t)pv->flags : 0;
    m->live = (pv && pv->gen == s_gen) ? 1 : 0;
    m->reason = reason;
}

extern "C" uint64_t pgxp_word_miss_ring(const PGXPWordMiss **ring,
                                        uint32_t *cap) {
    *ring = s_miss_ring;
    *cap = PGXP_MISS_RING_CAP;
    return s_miss_seq;
}

extern "C" int pgxp_load_precise_word(uint32_t addr, uint32_t packed,
                                      int32_t *x16, int32_t *y16, uint16_t *z) {
    if (!s_enabled) return 0;
    s_stats.word_lookups++;
    PGXPValue *pv = pgxp_ptr(addr);
    if (!pv || pv->gen != s_gen) {
        s_stats.word_untracked++;
        note_word_miss(addr, packed, pv, PGXP_MISS_UNTRACKED);
        return 0;
    }
    if (pv->value != packed) {
        s_stats.word_mismatch++;
        note_word_miss(addr, packed, pv, PGXP_MISS_MISMATCH);
        return 0;
    }
    if ((pv->flags & PGXP_F_VXY) != PGXP_F_VXY) {
        s_stats.word_partial++;
        note_word_miss(addr, packed, pv, PGXP_MISS_PARTIAL);
        return 0;
    }
    if (x16) *x16 = pv->x16;
    if (y16) *y16 = pv->y16;
    if (z) *z = (pv->flags & PGXP_F_VZ) ? pv->z : 0;
    if (!(pv->flags & PGXP_F_VZ) || pv->z == 0) {
        s_stats.word_no_z++;
        note_word_miss(addr, packed, pv, PGXP_MISS_NO_Z);
        return 0;
    }
    s_stats.word_hit++;
    return 1;
}
