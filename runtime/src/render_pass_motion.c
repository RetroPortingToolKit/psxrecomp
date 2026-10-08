/* Motion kit for render-pass plugins: capture, pair and blend the game state
 * a pass redraws from. See include/render_pass_motion.h. */
#include "render_pass_motion.h"
#include "mod_plugins.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MOTION_MAX_TICKS 8u
#define MOTION_PI 3.14159265358979323846

typedef struct MotionEntry {
    uint32_t addr, identity, seq;
    uint8_t  kind;
    int32_t  v[12];   /* MATRIX: r[0..8], t at [9..11]; others from v[0] */
} MotionEntry;

typedef struct MotionPair { uint32_t prev, cur; } MotionPair;

struct PSXMotionSet {
    uint32_t cap;
    MotionEntry* cap_buf[2];
    uint32_t n[2];
    uint32_t tick[2];
    int began[2];          /* a capture was made into this buffer */
    int sorted[2];
    int cur;               /* buffer the current capture writes */
    int history;           /* the other buffer holds the capture just before */
    MotionPair* pairs;
    uint32_t npairs;
    uint32_t ticks;
};

uint32_t psx_motion_identity(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = 2166136261u;
    const uint32_t w[3] = {a, b, c};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++) h = (h ^ ((w[i] >> (8 * k)) & 0xFFu)) * 16777619u;
    return h;
}

PSXMotionSet* psx_motion_set_create(uint32_t capacity) {
    PSXMotionSet* s;
    if (capacity == 0) return NULL;
    s = (PSXMotionSet*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cap = capacity;
    s->cap_buf[0] = (MotionEntry*)calloc(capacity, sizeof(MotionEntry));
    s->cap_buf[1] = (MotionEntry*)calloc(capacity, sizeof(MotionEntry));
    s->pairs = (MotionPair*)calloc(capacity, sizeof(MotionPair));
    if (!s->cap_buf[0] || !s->cap_buf[1] || !s->pairs) {
        psx_motion_set_destroy(s);
        return NULL;
    }
    return s;
}

void psx_motion_set_destroy(PSXMotionSet* s) {
    if (!s) return;
    free(s->cap_buf[0]);
    free(s->cap_buf[1]);
    free(s->pairs);
    free(s);
}

void psx_motion_invalidate(PSXMotionSet* s) {
    if (!s) return;
    s->history = 0;
    s->began[0] = s->began[1] = 0;
    s->n[0] = s->n[1] = 0;
    s->npairs = 0;
    s->ticks = 0;
}

void psx_motion_begin(PSXMotionSet* s, uint32_t tick) {
    if (!s) return;
    s->history = s->began[s->cur];
    s->cur ^= 1;
    s->n[s->cur] = 0;
    s->tick[s->cur] = tick;
    s->began[s->cur] = 1;
    s->sorted[s->cur] = 0;
    s->npairs = 0;
    s->ticks = 0;
}

/* ---- guest RAM --------------------------------------------------------- */

void psx_motion_read_matrix(uint32_t addr, PSXMotionMatrix* m) {
    for (int i = 0; i < 9; i++) m->r[i] = (int16_t)psx_mod_read_half(addr + 2u * (uint32_t)i);
    for (int i = 0; i < 3; i++) m->t[i] = (int32_t)psx_mod_read_word(addr + 0x14u + 4u * (uint32_t)i);
}

void psx_motion_write_matrix(uint32_t addr, const PSXMotionMatrix* m, int rotation) {
    if (rotation)
        for (int i = 0; i < 9; i++) psx_mod_write_half(addr + 2u * (uint32_t)i, (uint16_t)m->r[i]);
    for (int i = 0; i < 3; i++) psx_mod_write_word(addr + 0x14u + 4u * (uint32_t)i, (uint32_t)m->t[i]);
}

static void read_entry(MotionEntry* e) {
    switch (e->kind) {
    case PSX_MOTION_MATRIX:
    case PSX_MOTION_MATRIX_TRANSLATION: {
        PSXMotionMatrix m;
        psx_motion_read_matrix(e->addr, &m);
        for (int i = 0; i < 9; i++) e->v[i] = m.r[i];
        for (int i = 0; i < 3; i++) e->v[9 + i] = m.t[i];
        break;
    }
    case PSX_MOTION_ROTATION:
        for (int i = 0; i < 9; i++) e->v[i] = (int16_t)psx_mod_read_half(e->addr + 2u * (uint32_t)i);
        break;
    case PSX_MOTION_VECTOR:
        for (int i = 0; i < 3; i++) e->v[i] = (int32_t)psx_mod_read_word(e->addr + 4u * (uint32_t)i);
        break;
    case PSX_MOTION_SVECTOR:
    case PSX_MOTION_ANGLES:
        for (int i = 0; i < 3; i++) e->v[i] = (int16_t)psx_mod_read_half(e->addr + 2u * (uint32_t)i);
        break;
    case PSX_MOTION_SCALAR:
        e->v[0] = (int32_t)psx_mod_read_word(e->addr);
        break;
    }
}

int psx_motion_track(PSXMotionSet* s, uint32_t kind, uint32_t addr, uint32_t identity) {
    MotionEntry* e;
    if (!s || kind < PSX_MOTION_MATRIX || kind > PSX_MOTION_ROTATION) return 0;
    if (!s->began[s->cur] || s->n[s->cur] >= s->cap) return 0;
    e = &s->cap_buf[s->cur][s->n[s->cur]];
    memset(e, 0, sizeof *e);
    e->addr = addr;
    e->kind = (uint8_t)kind;
    e->identity = identity;
    e->seq = s->n[s->cur]++;
    s->sorted[s->cur] = 0;
    read_entry(e);
    return 1;
}

/* ---- math ---------------------------------------------------------------- */

static int orthonormal(const int16_t* r) {
    for (int i = 0; i < 3; i++) {
        double len = 0;
        for (int j = 0; j < 3; j++) len += (double)r[3 * i + j] * r[3 * i + j];
        if (fabs(sqrt(len) - 4096.0) > 4096.0 * 0.04) return 0;
        for (int k = i + 1; k < 3; k++) {
            double dot = 0;
            for (int j = 0; j < 3; j++) dot += (double)r[3 * i + j] * r[3 * k + j];
            if (fabs(dot) > 4096.0 * 4096.0 * 0.04) return 0;
        }
    }
    return 1;
}

static void to_quat(const int16_t* r, double q[4]) {
    double a[9], n;
    for (int i = 0; i < 9; i++) a[i] = r[i] / 4096.0;
    const double tr = a[0] + a[4] + a[8];
    if (tr > 0) {
        double s = sqrt(tr + 1.0) * 2;
        q[0] = 0.25 * s; q[1] = (a[7] - a[5]) / s; q[2] = (a[2] - a[6]) / s; q[3] = (a[3] - a[1]) / s;
    } else if (a[0] > a[4] && a[0] > a[8]) {
        double s = sqrt(1.0 + a[0] - a[4] - a[8]) * 2;
        q[0] = (a[7] - a[5]) / s; q[1] = 0.25 * s; q[2] = (a[1] + a[3]) / s; q[3] = (a[2] + a[6]) / s;
    } else if (a[4] > a[8]) {
        double s = sqrt(1.0 + a[4] - a[0] - a[8]) * 2;
        q[0] = (a[2] - a[6]) / s; q[1] = (a[1] + a[3]) / s; q[2] = 0.25 * s; q[3] = (a[5] + a[7]) / s;
    } else {
        double s = sqrt(1.0 + a[8] - a[0] - a[4]) * 2;
        q[0] = (a[3] - a[1]) / s; q[1] = (a[2] + a[6]) / s; q[2] = (a[5] + a[7]) / s; q[3] = 0.25 * s;
    }
    n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
}

static double det3(const int16_t* r) {
    return (double)r[0] * ((double)r[4] * r[8] - (double)r[5] * r[7]) -
           (double)r[1] * ((double)r[3] * r[8] - (double)r[5] * r[6]) +
           (double)r[2] * ((double)r[3] * r[7] - (double)r[4] * r[6]);
}

/* Orthonormal bases may be mirrored (determinant -1: a game that keeps a
 * flipped axis, as THPS2's skater basis does). A mirrored basis is a rotation
 * after one fixed reflection, so both ends are reflected the same way (third
 * row negated), blended as rotations and reflected back. Returns the number
 * of mirrored ends (0, 1 or 2), and fills the proper versions. A basis that
 * changes handedness between frames was placed, not moved. */
static int proper_pair(const int16_t* a, const int16_t* b, int16_t* pa, int16_t* pb) {
    const int ma = det3(a) < 0, mb = det3(b) < 0;
    memcpy(pa, a, 9 * sizeof *pa);
    memcpy(pb, b, 9 * sizeof *pb);
    if (ma) for (int i = 6; i < 9; i++) pa[i] = (int16_t)-pa[i];
    if (mb) for (int i = 6; i < 9; i++) pb[i] = (int16_t)-pb[i];
    return ma + mb;
}

static int16_t fixed12(double v) {
    double x = v * 4096.0;
    if (x > 32767.0) x = 32767.0;
    if (x < -32768.0) x = -32768.0;
    return (int16_t)lround(x);
}

static double turn_limit(double turn) { return turn > 0 ? turn : MOTION_PI; }

/* Angle between two orientations, or -1 when either is not a rotation. */
static double rotation_angle(const int16_t* a, const int16_t* b) {
    double p[4], q[4], d;
    int16_t pa[9], pb[9];
    if (!orthonormal(a) || !orthonormal(b)) return -1;
    if (proper_pair(a, b, pa, pb) == 1) return MOTION_PI * 2;   /* handedness flipped */
    to_quat(pa, p);
    to_quat(pb, q);
    d = fabs(p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3]);
    if (d > 1.0) d = 1.0;
    return 2.0 * acos(d);
}

static int blend_rotation(const int16_t* a, const int16_t* b, double t, double turn, int16_t* out) {
    if (orthonormal(a) && orthonormal(b)) {
        double p[4], q[4], d, angle, wa, wb, r[4];
        int16_t pa[9], pb[9];
        const int mirrored = proper_pair(a, b, pa, pb);
        if (mirrored == 1) { memcpy(out, b, 9 * sizeof *out); return 0; }
        to_quat(pa, p);
        to_quat(pb, q);
        d = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];
        if (d < 0) { for (int i = 0; i < 4; i++) q[i] = -q[i]; d = -d; }
        if (d > 1.0) d = 1.0;
        angle = acos(d);
        if (2.0 * angle > turn_limit(turn)) { memcpy(out, b, 9 * sizeof *out); return 0; }
        wa = 1 - t; wb = t;
        if (angle > 1e-6) { wa = sin((1 - t) * angle) / sin(angle); wb = sin(t * angle) / sin(angle); }
        for (int i = 0; i < 4; i++) r[i] = wa * p[i] + wb * q[i];
        {
            const double w = r[0], x = r[1], y = r[2], z = r[3];
            const double m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                                 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                                 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
            for (int i = 0; i < 9; i++) out[i] = fixed12(m[i]);
            if (mirrored) for (int i = 6; i < 9; i++) out[i] = (int16_t)-out[i];
        }
        return 1;
    }
    /* Scaled or sheared: elementwise, refusing jumps no motion produces. */
    for (int i = 0; i < 9; i++)
        if (abs((int)a[i] - (int)b[i]) > 4096) { memcpy(out, b, 9 * sizeof *out); return 0; }
    for (int i = 0; i < 9; i++) out[i] = (int16_t)lround(a[i] + (b[i] - a[i]) * t);
    return 1;
}

int psx_motion_blend_matrix(const PSXMotionMatrix* a, const PSXMotionMatrix* b,
                            double t, double turn, PSXMotionMatrix* out) {
    const int ok = blend_rotation(a->r, b->r, t, turn, out->r);
    for (int i = 0; i < 3; i++)
        out->t[i] = ok ? (int32_t)llround(a->t[i] + ((double)b->t[i] - a->t[i]) * t) : b->t[i];
    return ok;
}

static double dist3(const int32_t* a, const int32_t* b) {
    double s = 0;
    for (int i = 0; i < 3; i++) { const double d = (double)b[i] - a[i]; s += d * d; }
    return sqrt(s);
}

static int32_t arc12(int32_t from, int32_t to) {   /* shortest signed delta */
    int32_t d = (to - from) & 4095;
    return d > 2048 ? d - 4096 : d;   /* an exact half turn goes forward */
}

/* ---- pairing ------------------------------------------------------------- */

static int entry_cmp(const void* x, const void* y) {
    const MotionEntry* a = (const MotionEntry*)x;
    const MotionEntry* b = (const MotionEntry*)y;
    if (a->addr != b->addr) return a->addr < b->addr ? -1 : 1;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

/* Sort by (addr, kind); of repeats the last tracked wins. */
static void sort_capture(PSXMotionSet* s, int b) {
    MotionEntry* e = s->cap_buf[b];
    uint32_t n = s->n[b], w = 0;
    if (s->sorted[b]) return;
    qsort(e, n, sizeof *e, entry_cmp);
    for (uint32_t i = 0; i < n; i++) {
        if (w && e[w - 1].addr == e[i].addr && e[w - 1].kind == e[i].kind) e[w - 1] = e[i];
        else e[w++] = e[i];
    }
    s->n[b] = w;
    s->sorted[b] = 1;
}

/* 1 when the pair moved within the limits over `ticks`. */
static int within_limits(const MotionEntry* a, const MotionEntry* b,
                         const PSXMotionLimits* lim, uint32_t ticks) {
    const double move = lim->move_per_tick > 0 ? lim->move_per_tick * ticks : HUGE_VAL;
    switch (a->kind) {
    case PSX_MOTION_MATRIX:
    case PSX_MOTION_ROTATION: {
        double ang;
        int16_t ra[9], rb[9];
        if (a->kind == PSX_MOTION_MATRIX && dist3(&a->v[9], &b->v[9]) > move) return 0;
        for (int i = 0; i < 9; i++) { ra[i] = (int16_t)a->v[i]; rb[i] = (int16_t)b->v[i]; }
        ang = rotation_angle(ra, rb);
        if (ang >= 0) return ang <= turn_limit(lim->turn_radians);
        for (int i = 0; i < 9; i++) if (abs(a->v[i] - b->v[i]) > 4096) return 0;
        return 1;
    }
    case PSX_MOTION_MATRIX_TRANSLATION:
        return dist3(&a->v[9], &b->v[9]) <= move;
    case PSX_MOTION_VECTOR:
    case PSX_MOTION_SVECTOR:
        return dist3(a->v, b->v) <= move;
    case PSX_MOTION_ANGLES: {
        const double lim12 = turn_limit(lim->turn_radians) * 4096.0 / (2.0 * MOTION_PI);
        for (int i = 0; i < 3; i++) if (fabs((double)arc12(a->v[i], b->v[i])) > lim12) return 0;
        return 1;
    }
    case PSX_MOTION_SCALAR:
        return fabs((double)b->v[0] - a->v[0]) <= move;
    }
    return 0;
}

uint32_t psx_motion_prepare(PSXMotionSet* s, const PSXMotionLimits* lim,
                            PSXMotionStats* stats) {
    PSXMotionStats st;
    const PSXMotionLimits none = {0, 0};
    const int c = s ? s->cur : 0, h = c ^ 1;
    memset(&st, 0, sizeof st);
    if (!lim) lim = &none;
    if (!s || !s->began[c]) { if (stats) *stats = st; return 0; }
    sort_capture(s, c);
    st.tracked = s->n[c];
    s->npairs = 0;
    s->ticks = 0;
    if (s->history && s->began[h]) {
        const uint32_t ticks = s->tick[c] - s->tick[h];
        if (ticks >= 1 && ticks <= MOTION_MAX_TICKS) s->ticks = ticks;
    }
    if (!s->ticks) {
        st.unmatched = st.tracked;
    } else {
        const MotionEntry* pe = s->cap_buf[h];
        const MotionEntry* ce = s->cap_buf[c];
        uint32_t i = 0;
        sort_capture(s, h);
        for (uint32_t j = 0; j < s->n[c]; j++) {
            while (i < s->n[h] && (pe[i].addr < ce[j].addr ||
                   (pe[i].addr == ce[j].addr && pe[i].kind < ce[j].kind)))
                i++;
            if (i >= s->n[h] || pe[i].addr != ce[j].addr || pe[i].kind != ce[j].kind ||
                pe[i].identity != ce[j].identity) {
                st.unmatched++;
            } else if (!within_limits(&pe[i], &ce[j], lim, s->ticks)) {
                st.placed++;
            } else {
                s->pairs[s->npairs].prev = i;
                s->pairs[s->npairs].cur = j;
                s->npairs++;
            }
        }
    }
    st.blended = s->npairs;
    st.ticks = s->ticks;
    if (stats) *stats = st;
    return s->npairs;
}

/* Blend one pair into `v` (entry layout). */
static void blend_entry(const MotionEntry* a, const MotionEntry* b, double t, int32_t* v) {
    switch (a->kind) {
    case PSX_MOTION_MATRIX:
    case PSX_MOTION_ROTATION: {
        int16_t ra[9], rb[9], ro[9];
        for (int i = 0; i < 9; i++) { ra[i] = (int16_t)a->v[i]; rb[i] = (int16_t)b->v[i]; }
        blend_rotation(ra, rb, t, MOTION_PI * 2, ro);   /* limits were applied at prepare */
        for (int i = 0; i < 9; i++) v[i] = ro[i];
        for (int i = 9; i < 12; i++) v[i] = (int32_t)llround(a->v[i] + ((double)b->v[i] - a->v[i]) * t);
        break;
    }
    case PSX_MOTION_MATRIX_TRANSLATION:
        for (int i = 0; i < 9; i++) v[i] = b->v[i];
        for (int i = 9; i < 12; i++) v[i] = (int32_t)llround(a->v[i] + ((double)b->v[i] - a->v[i]) * t);
        break;
    case PSX_MOTION_ANGLES:
        for (int i = 0; i < 3; i++)
            v[i] = (int32_t)lround(a->v[i] + arc12(a->v[i], b->v[i]) * t);
        break;
    default:
        for (int i = 0; i < 3; i++) v[i] = (int32_t)llround(a->v[i] + ((double)b->v[i] - a->v[i]) * t);
        break;
    }
}

static void write_entry(uint8_t kind, uint32_t addr, const int32_t* v) {
    switch (kind) {
    case PSX_MOTION_MATRIX:
    case PSX_MOTION_MATRIX_TRANSLATION: {
        PSXMotionMatrix m;
        for (int i = 0; i < 9; i++) m.r[i] = (int16_t)v[i];
        for (int i = 0; i < 3; i++) m.t[i] = v[9 + i];
        psx_motion_write_matrix(addr, &m, kind == PSX_MOTION_MATRIX);
        break;
    }
    case PSX_MOTION_VECTOR:
        for (int i = 0; i < 3; i++) psx_mod_write_word(addr + 4u * (uint32_t)i, (uint32_t)v[i]);
        break;
    case PSX_MOTION_SVECTOR:
        for (int i = 0; i < 3; i++) psx_mod_write_half(addr + 2u * (uint32_t)i, (uint16_t)v[i]);
        break;
    case PSX_MOTION_ANGLES:
        for (int i = 0; i < 3; i++) psx_mod_write_half(addr + 2u * (uint32_t)i, (uint16_t)(v[i] & 4095));
        break;
    case PSX_MOTION_SCALAR:
        psx_mod_write_word(addr, (uint32_t)v[0]);
        break;
    case PSX_MOTION_ROTATION:
        for (int i = 0; i < 9; i++) psx_mod_write_half(addr + 2u * (uint32_t)i, (uint16_t)v[i]);
        break;
    }
}

void psx_motion_apply(const PSXMotionSet* s, double t) {
    if (!s || !s->npairs) return;
    const MotionEntry* pe = s->cap_buf[s->cur ^ 1];
    const MotionEntry* ce = s->cap_buf[s->cur];
    for (uint32_t k = 0; k < s->npairs; k++) {
        int32_t v[12];
        const MotionEntry* a = &pe[s->pairs[k].prev];
        const MotionEntry* b = &ce[s->pairs[k].cur];
        blend_entry(a, b, t, v);
        write_entry(b->kind, b->addr, v);
    }
}

static void export_value(uint8_t kind, const int32_t* v, void* out) {
    if (kind == PSX_MOTION_MATRIX || kind == PSX_MOTION_MATRIX_TRANSLATION ||
        kind == PSX_MOTION_ROTATION) {
        PSXMotionMatrix* m = (PSXMotionMatrix*)out;
        for (int i = 0; i < 9; i++) m->r[i] = (int16_t)v[i];
        for (int i = 0; i < 3; i++) m->t[i] = v[9 + i];
    } else if (kind == PSX_MOTION_SCALAR) {
        *(int32_t*)out = v[0];
    } else {
        memcpy(out, v, 3 * sizeof(int32_t));
    }
}

int psx_motion_blend_at(const PSXMotionSet* s, uint32_t kind, uint32_t addr,
                        double t, void* out) {
    const MotionEntry* ce;
    uint32_t lo = 0, hi;
    if (!s || !out || !s->began[s->cur]) return -1;
    ce = s->cap_buf[s->cur];
    hi = s->n[s->cur];
    if (!s->sorted[s->cur]) {
        for (uint32_t j = hi; j-- > 0;)   /* unprepared: newest wins */
            if (ce[j].addr == addr && ce[j].kind == kind) { export_value(ce[j].kind, ce[j].v, out); return 0; }
        return -1;
    }
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (ce[mid].addr < addr || (ce[mid].addr == addr && ce[mid].kind < kind)) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= s->n[s->cur] || ce[lo].addr != addr || ce[lo].kind != kind) return -1;
    for (uint32_t k = 0; k < s->npairs; k++) {
        if (s->pairs[k].cur == lo) {
            int32_t v[12];
            blend_entry(&s->cap_buf[s->cur ^ 1][s->pairs[k].prev], &ce[lo], t, v);
            export_value(ce[lo].kind, v, out);
            return 1;
        }
    }
    export_value(ce[lo].kind, ce[lo].v, out);
    return 0;
}
