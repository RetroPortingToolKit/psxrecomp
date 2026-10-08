#ifndef PSX_WS_BACKDROP_EXTENT_H
#define PSX_WS_BACKDROP_EXTENT_H
#include <stdint.h>
#include <string.h>

/* Finite 2D backdrop extent (native-wide scene classifier input).
 *
 * Pre-rendered 2D scenes draw their whole background as a few large textured
 * RECTANGLES at the back of the frame, before any polygon. Rect primitives are
 * screen-space blits by definition: nothing projects them, so the authored
 * image is exactly as wide as those rects and no wider. When that image covers
 * the canonical display but stops short of the native-wide reveal, the reveal
 * can only show void (black) with world props floating over it. Polygon
 * overhang cannot tell that apart from a real 3D world: widened 2D actor culls
 * legitimately place sprite quads past the canonical edge (Tomba's overhead
 * village: a 384x256 ground image, fence/sign quads out to x=428).
 *
 * Per frame: the leading run of draw primitives that are rectangles (closed by
 * the first polygon or line) contributes its untagged, unstretched textured
 * rects to a coarse coverage grid spanning [-reveal, W + reveal). A frame whose
 * run covers >= 90% of the canonical display is a full-screen backdrop frame;
 * if any reveal cell in the rows that backdrop occupies is uncovered, the
 * frame is SHORT. Two short frames within the sustain gap arm a veto that
 * presents the scene at native 4:3 until short frames stop arriving.
 * Host-derived presentation evidence: reset on load, never serialized. */

#define WS_BDX_CELL      8      /* column width, drawing-space pixels */
#define WS_BDX_COLS      1024   /* analysable span: 8192 px */
#define WS_BDX_ROWS      32     /* row bands across the display height */
#define WS_BDX_FULL_PCT  90     /* canonical coverage that makes a backdrop */
#define WS_BDX_SUSTAIN   6u     /* max frames between two short verdicts */

typedef struct {
    /* accumulating frame */
    uint32_t frame;
    int started, open;
    int32_t W, H, R, x_lo, ncols, row_h, nrows;
    int32_t min_x, max_x;          /* union x-extent of candidates */
    uint32_t rects;
    uint32_t mask[WS_BDX_COLS];    /* bit r = row band r covered */
    /* newest verdict (observability) */
    uint32_t eval_frame, eval_rects;
    int32_t eval_min_x, eval_max_x, eval_reveal;
    int eval_full, eval_short;
    uint32_t canon_pct, left_pct, right_pct;
    /* veto stamps */
    int has_short, has_sust;
    uint32_t last_short, sust_short;
    uint64_t evaluations, short_frames;
} WsBackdropExtent;

static inline void ws_bdx_reset(WsBackdropExtent* s) { memset(s, 0, sizeof *s); }

static inline int32_t ws_bdx_floordiv(int32_t a, int32_t b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* Smallest cell index whose centre is >= x. */
static inline int32_t ws_bdx_col_at(const WsBackdropExtent* s, int32_t x) {
    return ws_bdx_floordiv(x - s->x_lo - WS_BDX_CELL / 2 + WS_BDX_CELL - 1,
                           WS_BDX_CELL);
}

static inline int ws_bdx_popcount(uint32_t v) {
    int n = 0;
    while (v) { v &= v - 1u; ++n; }
    return n;
}

static inline void ws_bdx_evaluate(WsBackdropExtent* s) {
    if (!s->started || !s->rects || s->R <= 0 || s->W <= 0 || s->H <= 0) return;
    int32_t c0 = ws_bdx_col_at(s, 0), c1 = ws_bdx_col_at(s, s->W);
    if (c0 < 0) c0 = 0;
    if (c1 > s->ncols) c1 = s->ncols;
    uint32_t all_rows = s->nrows >= 32 ? 0xFFFFFFFFu : ((1u << s->nrows) - 1u);
    uint32_t canon_cells = 0, canon_cov = 0;
    uint32_t row_hits[WS_BDX_ROWS] = {0};
    for (int32_t c = c0; c < c1; ++c) {
        uint32_t m = s->mask[c] & all_rows;
        canon_cells += (uint32_t)s->nrows;
        canon_cov += (uint32_t)ws_bdx_popcount(m);
        for (int r = 0; r < s->nrows; ++r) if (m & (1u << r)) ++row_hits[r];
    }
    s->evaluations++;
    s->eval_frame = s->frame;
    s->eval_rects = s->rects;
    s->eval_min_x = s->min_x; s->eval_max_x = s->max_x;
    s->eval_reveal = s->R;
    s->canon_pct = canon_cells ? canon_cov * 100u / canon_cells : 0;
    s->eval_full = canon_cells && canon_cov * 100u >= canon_cells * WS_BDX_FULL_PCT;
    s->eval_short = 0;
    s->left_pct = s->right_pct = 0;
    if (!s->eval_full) return;
    /* Rows the backdrop actually occupies: a letterboxed image does not demand
     * coverage in bands it leaves empty inside the canonical frame too. */
    uint32_t need = 0, span = (uint32_t)(c1 - c0);
    for (int r = 0; r < s->nrows; ++r)
        if (row_hits[r] * 100u >= span * WS_BDX_FULL_PCT) need |= 1u << r;
    uint32_t lc = 0, ln = 0, rc = 0, rn = 0;
    int nrow = ws_bdx_popcount(need);
    for (int32_t c = 0; c < s->ncols; ++c) {
        if (c >= c0 && c < c1) continue;
        uint32_t got = (uint32_t)ws_bdx_popcount(s->mask[c] & need);
        if (c < c0) { lc += got; ln += (uint32_t)nrow; }
        else        { rc += got; rn += (uint32_t)nrow; }
    }
    /* Reveal wider than the analysable grid: the union extent must at least
     * reach the true reveal edges. */
    int32_t grid_lo = s->x_lo, grid_hi = s->x_lo + s->ncols * WS_BDX_CELL;
    int beyond_short = (grid_lo > -s->R && s->min_x > -s->R) ||
                       (grid_hi < s->W + s->R && s->max_x < s->W + s->R);
    s->left_pct = ln ? lc * 100u / ln : 100u;
    s->right_pct = rn ? rc * 100u / rn : 100u;
    s->eval_short = lc < ln || rc < rn || beyond_short;
    if (!s->eval_short) return;
    s->short_frames++;
    if (s->has_short && s->frame - s->last_short <= WS_BDX_SUSTAIN) {
        s->sust_short = s->frame;
        s->has_sust = 1;
    }
    s->last_short = s->frame;
    s->has_short = 1;
}

/* Start (or continue) accumulating `frame`. W/H = canonical display size,
 * R = configured per-side native-wide reveal. A new frame finalizes the
 * previous one if its leading run never closed. */
static inline void ws_bdx_frame(WsBackdropExtent* s, uint32_t frame,
                                int32_t W, int32_t H, int32_t R) {
    if (s->started && s->frame == frame) return;
    if (s->started && (int32_t)(frame - s->frame) < 0) {
        ws_bdx_reset(s);                 /* frame rewind: forget verdicts */
    } else if (s->started && s->open) {
        ws_bdx_evaluate(s);
    }
    s->started = 1; s->open = 1; s->frame = frame;
    s->W = W; s->H = H; s->R = R < 0 ? 0 : R;
    int32_t span = W + 2 * s->R;
    int32_t ncols = (span + WS_BDX_CELL - 1) / WS_BDX_CELL;
    if (ncols > WS_BDX_COLS) {           /* clamp symmetrically */
        ncols = WS_BDX_COLS;
        s->x_lo = (W - ncols * WS_BDX_CELL) / 2;
    } else {
        s->x_lo = -s->R;
    }
    s->ncols = ncols < 0 ? 0 : ncols;
    s->row_h = H > 0 ? (H + WS_BDX_ROWS - 1) / WS_BDX_ROWS : 1;
    s->nrows = H > 0 ? (H + s->row_h - 1) / s->row_h : 0;
    s->rects = 0;
    s->min_x = INT32_MAX; s->max_x = INT32_MIN;
    memset(s->mask, 0, sizeof s->mask);
}

/* A backdrop candidate inside the leading run: [x0,x1) x [y0,y1). */
static inline void ws_bdx_rect(WsBackdropExtent* s, int32_t x0, int32_t y0,
                               int32_t x1, int32_t y1) {
    if (!s->started || !s->open || x1 <= x0 || y1 <= y0) return;
    s->rects++;
    if (x0 < s->min_x) s->min_x = x0;
    if (x1 > s->max_x) s->max_x = x1;
    int32_t ca = ws_bdx_col_at(s, x0), cb = ws_bdx_col_at(s, x1);
    if (ca < 0) ca = 0;
    if (cb > s->ncols) cb = s->ncols;
    /* Row band r is covered when its centre lies inside [y0, y1). */
    int32_t ra = ws_bdx_floordiv(y0 - s->row_h / 2 + s->row_h - 1, s->row_h);
    int32_t rb = ws_bdx_floordiv(y1 - s->row_h / 2 + s->row_h - 1, s->row_h);
    if (ra < 0) ra = 0;
    if (rb > s->nrows) rb = s->nrows;
    if (ca >= cb || ra >= rb) return;
    uint32_t bits = 0;
    for (int32_t r = ra; r < rb; ++r) bits |= 1u << r;
    for (int32_t c = ca; c < cb; ++c) s->mask[c] |= bits;
}

/* The first polygon or line ends the backdrop run for this frame. */
static inline void ws_bdx_close(WsBackdropExtent* s) {
    if (!s->started || !s->open) return;
    s->open = 0;
    ws_bdx_evaluate(s);
}

/* Veto native-wide: a sustained short full-screen backdrop, still recent. */
static inline int ws_bdx_veto(const WsBackdropExtent* s, uint32_t frame,
                              uint32_t grace) {
    if (!s->has_sust) return 0;
    /* The newest short frame must itself belong to a sustained pair; an
     * isolated short frame after a quiet spell does not re-arm the veto. */
    if (s->sust_short != s->last_short) return 0;
    if ((int32_t)(frame - s->sust_short) < 0) return 0;
    return frame - s->sust_short <= grace;
}
#endif
