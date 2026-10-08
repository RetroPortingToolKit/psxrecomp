/* Frame generation: matching, interpolation, planning (include/frame_gen.h,
 * docs/FRAME_GENERATION.md). */
#include "frame_gen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void fg_prims_reset(FgPrimList *l) { l->n = 0; }

void fg_prims_free(FgPrimList *l) {
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

int fg_prims_add(FgPrimList *l, const FgPrim *p) {
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2u : 1024u;
        FgPrim *nv = (FgPrim *)realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv) return 0;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = *p;
    return 1;
}

uint32_t fg_hash(uint32_t h, const int32_t *w, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t x = (uint32_t)w[i];
        for (int b = 0; b < 4; b++) {
            h ^= (x >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

void fg_cam_defaults(FgCamParams *p) {
    p->iters = 64;
    p->tol_px = 1.0f;
    p->min_inliers = 0.5f;
    p->min_pairs = 24;
    p->max_angle = 0.35f;
    p->max_shift = 0.5f;
    p->max_obj = 0.25f;
    p->keep_partial = 0;
}

/* ---- rigid motion ---- */
typedef struct { double q[4], t[3]; } Rigid;

static void q_to_m(const double q[4], double m[9]) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - w * z);     m[2] = 2 * (x * z + w * y);
    m[3] = 2 * (x * y + w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - w * x);
    m[6] = 2 * (x * z - w * y);     m[7] = 2 * (y * z + w * x);     m[8] = 1 - 2 * (x * x + y * y);
}

static void m_mul_v(const double m[9], const double v[3], double o[3]) {
    for (int i = 0; i < 3; i++) o[i] = m[3 * i] * v[0] + m[3 * i + 1] * v[1] + m[3 * i + 2] * v[2];
}

/* Least-squares rigid motion a -> b (Horn's quaternion method; the largest
 * eigenvector by Jacobi rotations). */
static int fit_rigid(const float (*a)[3], const float (*b)[3], const uint32_t *idx, uint32_t n, Rigid *r) {
    if (n < 3) return 0;
    double ca[3] = { 0, 0, 0 }, cb[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) { ca[k] += a[idx[i]][k]; cb[k] += b[idx[i]][k]; }
    for (int k = 0; k < 3; k++) { ca[k] /= n; cb[k] /= n; }
    double S[3][3] = { { 0 } };
    for (uint32_t i = 0; i < n; i++)
        for (int u = 0; u < 3; u++)
            for (int v = 0; v < 3; v++)
                S[u][v] += (a[idx[i]][u] - ca[u]) * (b[idx[i]][v] - cb[v]);
    const double N[4][4] = {
        { S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
        { S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
        { S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
        { S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] } };
    /* Cyclic Jacobi: the eigenvector of the largest eigenvalue, exactly. */
    double A[4][4], V[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) { A[i][j] = N[i][j]; V[i][j] = i == j; }
    for (int sweep = 0; sweep < 50; sweep++) {
        double off = 0;
        for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) off += A[i][j] * A[i][j];
        if (off < 1e-30) break;
        for (int pp = 0; pp < 4; pp++)
            for (int qq = pp + 1; qq < 4; qq++) {
                if (fabs(A[pp][qq]) < 1e-300) continue;
                const double th = (A[qq][qq] - A[pp][pp]) / (2.0 * A[pp][qq]);
                const double tt = (th >= 0 ? 1.0 : -1.0) / (fabs(th) + sqrt(th * th + 1.0));
                const double c = 1.0 / sqrt(tt * tt + 1.0), sn = tt * c;
                for (int k = 0; k < 4; k++) {
                    const double akp = A[k][pp], akq = A[k][qq];
                    A[k][pp] = c * akp - sn * akq; A[k][qq] = sn * akp + c * akq;
                }
                for (int k = 0; k < 4; k++) {
                    const double apk = A[pp][k], aqk = A[qq][k];
                    A[pp][k] = c * apk - sn * aqk; A[qq][k] = sn * apk + c * aqk;
                }
                for (int k = 0; k < 4; k++) {
                    const double vkp = V[k][pp], vkq = V[k][qq];
                    V[k][pp] = c * vkp - sn * vkq; V[k][qq] = sn * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 4; i++) if (A[i][i] > A[best][best]) best = i;
    double q[4], len = 0;
    for (int i = 0; i < 4; i++) { q[i] = V[i][best]; len += q[i] * q[i]; }
    len = sqrt(len);
    if (!(len > 0)) return 0;
    for (int i = 0; i < 4; i++) q[i] /= len;
    if (q[0] < 0) for (int i = 0; i < 4; i++) q[i] = -q[i];
    memcpy(r->q, q, sizeof q);
    double m[9], rc[3];
    q_to_m(q, m);
    m_mul_v(m, ca, rc);
    for (int k = 0; k < 3; k++) r->t[k] = cb[k] - rc[k];
    return 1;
}

static int is_inlier(const double m[9], const double t[3], const float a[3], const float b[3],
                     float h, const FgCamParams *p) {
    const double av[3] = { a[0], a[1], a[2] };
    double o[3];
    m_mul_v(m, av, o);
    double e = 0;
    for (int k = 0; k < 3; k++) { const double d = o[k] + t[k] - b[k]; e += d * d; }
    /* In screen pixels at the point's depth. */
    const double tol = p->tol_px * fmax(fabs(b[2]), 1.0) / fmax(h, 1.0);
    return e < tol * tol;
}

/* ---- pairing ---- */
typedef struct { uint32_t id; float p[3], h; } Src;

static int src_cmp(const void *x, const void *y) {
    const Src *a = (const Src *)x, *b = (const Src *)y;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    for (int k = 0; k < 3; k++) if (a->p[k] != b->p[k]) return a->p[k] < b->p[k] ? -1 : 1;
    return 0;
}

/* The distinct sources of one view, sorted. */
static uint32_t gather(const FgPrimList *l, uint32_t view, Src *out) {
    uint32_t n = 0;
    for (uint32_t j = 0; j < l->n; j++) {
        const FgPrim *pr = &l->v[j];
        if (pr->view != view) continue;
        for (int k = 0; k < 3; k++) {
            if (!pr->vid[k]) continue;
            out[n].id = pr->vid[k];
            memcpy(out[n].p, pr->p[k], sizeof out[n].p);
            out[n].h = pr->h[k];
            n++;
        }
    }
    qsort(out, n, sizeof *out, src_cmp);
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!w || src_cmp(&out[w - 1], &out[i]) != 0) out[w++] = out[i];
    return w;
}

static uint32_t lower(const Src *s, uint32_t n, uint32_t id) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { const uint32_t m = (lo + hi) / 2; if (s[m].id < id) lo = m + 1; else hi = m; }
    return lo;
}

static float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static double median_z(const Src *s, uint32_t n) {
    if (!n) return 0;
    /* A coarse median: the depths in 64 buckets. */
    double lo = 1e30, hi = -1e30;
    for (uint32_t i = 0; i < n; i++) { if (s[i].p[2] < lo) lo = s[i].p[2]; if (s[i].p[2] > hi) hi = s[i].p[2]; }
    if (hi <= lo) return lo;
    uint32_t b[64] = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        int k = (int)((s[i].p[2] - lo) / (hi - lo) * 63.999);
        b[k]++;
    }
    uint32_t c = 0;
    for (int k = 0; k < 64; k++) { c += b[k]; if (c * 2 >= n) return lo + (hi - lo) * (k + 0.5) / 64.0; }
    return hi;
}

static int fit_view(const FgPrimList *older, const FgPrimList *newer, const FgCamParams *p,
                    FgView *v, Src **pa_out, Src **pb_out, uint8_t **in_out, uint32_t *np_out,
                    Src **nb_out, uint32_t *nnb_out) {
    Src *sa = (Src *)malloc(((size_t)older->n * 3 + 1) * sizeof *sa);
    Src *sb = (Src *)malloc(((size_t)newer->n * 3 + 1) * sizeof *sb);
    if (!sa || !sb) { free(sa); free(sb); v->why = "out of memory"; return 0; }
    const uint32_t na = gather(older, v->view, sa), nb = gather(newer, v->view, sb);
    v->sources = nb;
    /* Pairs: the same identity; among several (instances of one mesh), the
     * nearest, when clearly the nearest. */
    float (*a)[3] = (float (*)[3])malloc(((size_t)nb + 1) * sizeof *a);
    float (*b)[3] = (float (*)[3])malloc(((size_t)nb + 1) * sizeof *b);
    float *hb = (float *)malloc(((size_t)nb + 1) * sizeof *hb);
    uint32_t *bi = (uint32_t *)malloc(((size_t)nb + 1) * sizeof *bi);
    uint32_t np = 0;
    for (uint32_t i = 0; a && b && bi && hb && i < nb; i++) {
        uint32_t j = lower(sa, na, sb[i].id);
        float best = -1, second = -1;
        uint32_t bj = 0;
        for (; j < na && sa[j].id == sb[i].id; j++) {
            const float d = dist3(sa[j].p, sb[i].p);
            if (best < 0 || d < best) { second = best; best = d; bj = j; }
            else if (second < 0 || d < second) second = d;
        }
        if (best < 0) continue;
        if (second >= 0 && best > 0.5f * second) continue;
        memcpy(a[np], sa[bj].p, sizeof a[np]);
        memcpy(b[np], sb[i].p, sizeof b[np]);
        hb[np] = sb[i].h;
        bi[np++] = i;
    }
    v->pairs = np;
    uint8_t *in = (uint8_t *)calloc((size_t)np + 1, 1);
    uint32_t *idx = (uint32_t *)malloc(((size_t)np + 1) * sizeof *idx);
    int ok = 0;
    if (!a || !b || !bi || !hb || !in || !idx) { v->why = "out of memory"; goto done; }
    if (np < p->min_pairs) { v->why = "few pairs"; goto done; }
    {
        Rigid best_r;
        uint32_t best_n = 0, seed = 0x9E3779B9u ^ np;
        double m[9];
        for (int it = 0; it < p->iters; it++) {
            uint32_t s3[3];
            for (int k = 0; k < 3; k++) { seed = seed * 1664525u + 1013904223u; s3[k] = (seed >> 8) % np; }
            if (s3[0] == s3[1] || s3[1] == s3[2] || s3[0] == s3[2]) continue;
            Rigid r;
            if (!fit_rigid((const float (*)[3])a, (const float (*)[3])b, s3, 3, &r)) continue;
            q_to_m(r.q, m);
            uint32_t c = 0;
            for (uint32_t i = 0; i < np; i++) c += (uint32_t)is_inlier(m, r.t, a[i], b[i], hb[i], p);
            if (c > best_n) { best_n = c; best_r = r; }
        }
        if (best_n < 3) { v->why = "no consistent camera"; goto done; }
        /* Refine on the inliers, twice. */
        for (int pass = 0; pass < 2; pass++) {
            q_to_m(best_r.q, m);
            uint32_t c = 0;
            for (uint32_t i = 0; i < np; i++)
                if ((in[i] = (uint8_t)is_inlier(m, best_r.t, a[i], b[i], hb[i], p))) idx[c++] = i;
            Rigid r;
            if (c >= 3 && fit_rigid((const float (*)[3])a, (const float (*)[3])b, idx, c, &r)) best_r = r;
        }
        q_to_m(best_r.q, m);
        uint32_t c = 0;
        for (uint32_t i = 0; i < np; i++) c += (in[i] = (uint8_t)is_inlier(m, best_r.t, a[i], b[i], hb[i], p));
        v->inliers = c;
        memcpy(v->q, best_r.q, sizeof v->q);
        memcpy(v->t, best_r.t, sizeof v->t);
        const double ang = 2.0 * acos(fmin(1.0, fabs(best_r.q[0])));
        const double mz = median_z(sb, nb);
        const double sh = sqrt(best_r.t[0] * best_r.t[0] + best_r.t[1] * best_r.t[1] + best_r.t[2] * best_r.t[2]);
        if ((float)c < p->min_inliers * (float)np || c < p->min_pairs) v->why = "few inliers";
        else if (ang > p->max_angle) v->why = "camera turned too far";
        else if (mz > 0 && sh > p->max_shift * mz) v->why = "camera moved too far";
        else ok = 1;
    }
done:
    v->ok = ok;
    free(idx);
    /* Pairs out (for the object placement), newer sources kept. */
    if (pa_out) {
        Src *pa = (Src *)malloc(((size_t)np + 1) * sizeof *pa);
        if (pa && a) for (uint32_t i = 0; i < np; i++) { pa[i] = sb[bi[i]]; memcpy(pa[i].p, a[i], sizeof pa[i].p); }
        *pa_out = pa;
    }
    if (pb_out) {
        Src *pb = (Src *)malloc(((size_t)np + 1) * sizeof *pb);
        if (pb && bi) for (uint32_t i = 0; i < np; i++) pb[i] = sb[bi[i]];
        *pb_out = pb;
    }
    *in_out = in; *np_out = np;
    *nb_out = sb; *nnb_out = nb;
    free(sa); free(a); free(b); free(bi); free(hb);
    return ok;
}

int fg_cam_fit(const FgPrimList *older, const FgPrimList *newer, const FgCamParams *p,
               FgCamFit *fit, FgVert *verts) {
    memset(fit, 0, sizeof *fit);
    fit->prims = newer->n;
    for (uint32_t j = 0; j < newer->n * 3u; j++) {
        verts[j].mode = FG_PLACE_UNCHANGED; verts[j].view = -1; verts[j].paired = 0;
    }
    /* The views with projections, most first. */
    uint32_t vk[FG_MAX_VIEWS], vc[FG_MAX_VIEWS], nv = 0;
    for (uint32_t j = 0; j < newer->n; j++) {
        const FgPrim *pr = &newer->v[j];
        if (!pr->vid[0] && !pr->vid[1] && !pr->vid[2]) continue;
        uint32_t i = 0;
        while (i < nv && vk[i] != pr->view) i++;
        if (i == nv) {
            if (nv == FG_MAX_VIEWS) { fit->why = "too many views"; return 0; }
            vk[nv] = pr->view; vc[nv++] = 0;
        }
        vc[i]++;
    }
    if (!nv) { fit->why = "no projections"; return 0; }
    fit->nviews = nv;
    int all = 1;
    for (uint32_t vi = 0; vi < nv; vi++) {
        FgView *v = &fit->v[vi];
        v->view = vk[vi];
        for (uint32_t j = 0; j < newer->n; j++)
            if (newer->v[j].view == v->view) { memcpy(v->area, newer->v[j].area, sizeof v->area); break; }
        Src *pa = NULL, *pb = NULL, *sb = NULL;
        uint8_t *in = NULL;
        uint32_t np = 0, nb = 0;
        const int ok = fit_view(older, newer, p, v, &pa, &pb, &in, &np, &sb, &nb);
        /* A view of a few projections (a 3D icon, a stray lookup) without a
         * camera stays as it is; a real view without one rejects the frame. */
        if (!ok && vc[vi] >= p->min_pairs) { all = 0; if (!fit->why) fit->why = v->why; }
        if (ok && pa && pb && in) {
            /* Pairs are in newer-source order (pb sorted): look vertices up. */
            for (uint32_t j = 0; j < newer->n; j++) {
                const FgPrim *pr = &newer->v[j];
                if (pr->view != v->view) continue;
                for (int k = 0; k < 3; k++) {
                    if (!pr->vid[k]) continue;
                    FgVert *fv = &verts[3 * j + k];
                    fv->view = (int8_t)vi;
                    fv->mode = FG_PLACE_CAMERA;
                    Src key; key.id = pr->vid[k]; memcpy(key.p, pr->p[k], sizeof key.p);
                    uint32_t lo = 0, hi = np;
                    while (lo < hi) { const uint32_t m = (lo + hi) / 2; if (src_cmp(&pb[m], &key) < 0) lo = m + 1; else hi = m; }
                    fv->paired = lo < np && src_cmp(&pb[lo], &key) == 0;
                    if (fv->paired && !in[lo] &&
                        dist3(pa[lo].p, pr->p[k]) < p->max_obj * fabsf(pr->p[k][2]) + 1.0f) {
                        fv->mode = FG_PLACE_OBJECT;
                        memcpy(fv->a, pa[lo].p, sizeof fv->a);
                    }
                }
            }
        }
        free(pa); free(pb); free(in); free(sb);
    }
    fit->ok = all;
    if (!all && p->keep_partial) {
        int any = 0;
        for (uint32_t vi = 0; vi < fit->nviews; vi++) any |= fit->v[vi].ok;
        if (any) {
            for (uint32_t j = 0; j < newer->n * 3u; j++) {
                const int vi = verts[j].view;
                if (vi >= 0 && (uint32_t)vi < fit->nviews && !fit->v[vi].ok) {
                    verts[j].mode = FG_PLACE_UNCHANGED; verts[j].view = -1;
                }
            }
            fit->ok = all = 1;
        }
    }
    if (!all) {
        for (uint32_t j = 0; j < newer->n * 3u; j++) verts[j].mode = FG_PLACE_UNCHANGED;
        return 0;
    }
    /* An unpaired vertex of a moving object (a camera-attached sky dome
     * whose vertex was culled last frame) moves with the object, not with
     * the world. */
    for (uint32_t j = 0; j < newer->n; j++) {
        int obj = 0;
        for (int k = 0; k < 3; k++) obj |= verts[3 * j + k].mode == FG_PLACE_OBJECT;
        if (!obj) continue;
        for (int k = 0; k < 3; k++)
            if (verts[3 * j + k].mode == FG_PLACE_CAMERA && !verts[3 * j + k].paired)
                verts[3 * j + k].mode = FG_PLACE_NEIGHBOUR;
    }
    /* Vertices without a projection in triangles with placed ones. */
    for (uint32_t j = 0; j < newer->n; j++) {
        int placed = 0;
        for (int k = 0; k < 3; k++) placed += verts[3 * j + k].mode != FG_PLACE_UNCHANGED;
        if (!placed || placed == 3) continue;
        int8_t vw = -1;
        for (int k = 0; k < 3; k++) if (verts[3 * j + k].view >= 0) vw = verts[3 * j + k].view;
        for (int k = 0; k < 3; k++)
            if (verts[3 * j + k].mode == FG_PLACE_UNCHANGED && !newer->v[j].vid[k]) {
                verts[3 * j + k].mode = FG_PLACE_NEIGHBOUR;
                verts[3 * j + k].view = vw;
            }
    }
    for (uint32_t j = 0; j < newer->n * 3u; j++)
        switch (verts[j].mode) {
        case FG_PLACE_CAMERA: fit->camera++; break;
        case FG_PLACE_OBJECT: fit->object++; break;
        case FG_PLACE_NEIGHBOUR: fit->neighbour++; break;
        default: fit->unchanged++; break;
        }
    return 1;
}

/* Projection of a camera-space point (0 when behind the near plane). */
static int proj(const double v[3], double h, double *sx, double *sy) {
    if (v[2] < 1.0) return 0;
    *sx = h * v[0] / v[2];
    *sy = h * v[1] / v[2];
    return 1;
}

typedef struct { int32_t qx, qy; float dx, dy; uint32_t n; } NAcc;

void fg_cam_place(const FgPrimList *newer, FgCamFit *fit, const FgVert *verts,
                  double t, float *x, float *y, float margin[][4]) {
    const uint32_t n = newer->n;
    if (margin) memset(margin, 0, sizeof(float) * 4 * FG_MAX_VIEWS);
    for (uint32_t j = 0; j < n; j++)
        for (int k = 0; k < 3; k++) { x[3 * j + k] = newer->v[j].x[k]; y[3 * j + k] = newer->v[j].y[k]; }
    if (!fit->ok || t >= 1.0) return;
    if (t < 0.0) t = 0.0;
    /* Per view: P_t = R_t (R^T (P - T)) + t T, R_t = the rotation's fraction t. */
    double M[FG_MAX_VIEWS][9], Rt[FG_MAX_VIEWS][9], T[FG_MAX_VIEWS][3], Tt[FG_MAX_VIEWS][3];
    for (uint32_t vi = 0; vi < fit->nviews; vi++) {
        const FgView *v = &fit->v[vi];
        q_to_m(v->q, M[vi]);
        const double w = fmin(1.0, fabs(v->q[0]));
        const double ang = 2.0 * acos(w), s = sqrt(fmax(0.0, 1.0 - w * w));
        double qt[4] = { 1, 0, 0, 0 };
        if (s > 1e-9) {
            const double sg = v->q[0] < 0 ? -1.0 : 1.0;
            qt[0] = cos(0.5 * ang * t);
            for (int k = 0; k < 3; k++) qt[k + 1] = sg * v->q[k + 1] / s * sin(0.5 * ang * t);
        }
        q_to_m(qt, Rt[vi]);
        for (int k = 0; k < 3; k++) { T[vi][k] = v->t[k]; Tt[vi][k] = v->t[k] * t; }
    }
    uint32_t clamped = 0, guessed = 0;
    float *dxs = (float *)malloc((size_t)n * 3 * sizeof *dxs);
    float *dys = (float *)malloc((size_t)n * 3 * sizeof *dys);
    if (!dxs || !dys) { free(dxs); free(dys); return; }
    for (uint32_t j = 0; j < n; j++) {
        const FgPrim *pr = &newer->v[j];
        for (int k = 0; k < 3; k++) {
            const uint32_t i = 3 * j + k;
            dxs[i] = dys[i] = 0.0f;
            const FgVert *fv = &verts[i];
            if (fv->mode != FG_PLACE_CAMERA && fv->mode != FG_PLACE_OBJECT) continue;
            const double pn[3] = { pr->p[k][0], pr->p[k][1], pr->p[k][2] };
            double pt[3];
            if (fv->mode == FG_PLACE_OBJECT) {
                for (int c = 0; c < 3; c++) pt[c] = fv->a[c] + (pn[c] - fv->a[c]) * t;
            } else {
                const int vi = fv->view;
                double d[3], o[3];
                for (int c = 0; c < 3; c++) d[c] = pn[c] - T[vi][c];
                /* R^T d */
                for (int c = 0; c < 3; c++) o[c] = M[vi][c] * d[0] + M[vi][3 + c] * d[1] + M[vi][6 + c] * d[2];
                m_mul_v(Rt[vi], o, pt);
                for (int c = 0; c < 3; c++) pt[c] += Tt[vi][c];
            }
            double ax, ay, bx, by;
            if (!proj(pn, pr->h[k], &bx, &by)) continue;
            /* Near geometry the in-between camera is passing: projected at a
             * clamped depth. A function of the vertex alone, so triangles
             * sharing it stay closed (no cracks onto what is behind). */
            const double zmin = fmax(16.0, 0.25 * pn[2]);
            if (pt[2] < zmin) { pt[2] = zmin; clamped++; }
            if (!proj(pt, pr->h[k], &ax, &ay)) continue;
            dxs[i] = (float)(ax - bx); dys[i] = (float)(ay - by);
        }
    }
    /* A vertex without a projection lying on an edge between two placed
     * vertices (the game split a polygon there: near-plane clipping,
     * subdivision) moves as that point of the edge does, so the T-junction
     * stays closed. */
    uint8_t *done = (uint8_t *)calloc((size_t)n * 3, 1);
    {
        uint32_t ne = 0;
        for (uint32_t i = 0; i < n * 3u; i++) ne += verts[i].mode == FG_PLACE_NEIGHBOUR;
        if (ne && done) {
            enum { G = 16 };   /* grid cell, native px */
            int32_t gx0 = INT32_MAX, gy0 = INT32_MAX, gx1 = INT32_MIN, gy1 = INT32_MIN;
            for (uint32_t i = 0; i < n * 3u; i++) {
                const int32_t cx = (int32_t)floorf(newer->v[i / 3].x[i % 3] / G), cy = (int32_t)floorf(newer->v[i / 3].y[i % 3] / G);
                if (cx < gx0) gx0 = cx; if (cx > gx1) gx1 = cx; if (cy < gy0) gy0 = cy; if (cy > gy1) gy1 = cy;
            }
            if (gx0 < -64) gx0 = -64; if (gy0 < -64) gy0 = -64; if (gx1 > 128) gx1 = 128; if (gy1 > 128) gy1 = 128;
            const int32_t gw = gx1 - gx0 + 1, gh = gy1 - gy0 + 1;
            /* Cells hold neighbour vertices; edges look them up. */
            int32_t *head = gw > 0 && gh > 0 ? (int32_t *)malloc((size_t)gw * gh * sizeof *head) : NULL;
            int32_t *nxt = (int32_t *)malloc((size_t)n * 3 * sizeof *nxt);
            float *best = (float *)malloc((size_t)n * 3 * sizeof *best);
            if (head && nxt && best) {
                for (int32_t c = 0; c < gw * gh; c++) head[c] = -1;
                for (uint32_t i = 0; i < n * 3u; i++) {
                    best[i] = 0.75f;
                    if (verts[i].mode != FG_PLACE_NEIGHBOUR) continue;
                    const int32_t cx = (int32_t)floorf(newer->v[i / 3].x[i % 3] / G) - gx0;
                    const int32_t cy = (int32_t)floorf(newer->v[i / 3].y[i % 3] / G) - gy0;
                    if (cx < 0 || cy < 0 || cx >= gw || cy >= gh) continue;
                    nxt[i] = head[cy * gw + cx]; head[cy * gw + cx] = (int32_t)i;
                }
                for (uint32_t j = 0; j < n; j++) {
                    const FgPrim *pr = &newer->v[j];
                    for (int e = 0; e < 3; e++) {
                        const uint32_t i0 = 3 * j + e, i1 = 3 * j + (e + 1) % 3;
                        const uint8_t m0 = verts[i0].mode, m1 = verts[i1].mode;
                        if ((m0 != FG_PLACE_CAMERA && m0 != FG_PLACE_OBJECT) ||
                            (m1 != FG_PLACE_CAMERA && m1 != FG_PLACE_OBJECT)) continue;
                        const float ax = pr->x[e], ay = pr->y[e], bx = pr->x[(e + 1) % 3], by = pr->y[(e + 1) % 3];
                        const float ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
                        if (l2 < 1.0f) continue;
                        int32_t cx0 = (int32_t)floorf(fminf(ax, bx) / G) - gx0, cx1 = (int32_t)floorf(fmaxf(ax, bx) / G) - gx0;
                        int32_t cy0 = (int32_t)floorf(fminf(ay, by) / G) - gy0, cy1 = (int32_t)floorf(fmaxf(ay, by) / G) - gy0;
                        if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0; if (cx1 >= gw) cx1 = gw - 1; if (cy1 >= gh) cy1 = gh - 1;
                        if ((int64_t)(cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 4096) continue;
                        for (int32_t cy = cy0; cy <= cy1; cy++)
                            for (int32_t cx = cx0; cx <= cx1; cx++)
                                for (int32_t v = head[cy * gw + cx]; v >= 0; v = nxt[v]) {
                                    const float px = newer->v[v / 3].x[v % 3], py = newer->v[v / 3].y[v % 3];
                                    const float u = ((px - ax) * ex + (py - ay) * ey) / l2;
                                    if (u <= 0.0f || u >= 1.0f) continue;
                                    const float dx = ax + u * ex - px, dy = ay + u * ey - py, d = sqrtf(dx * dx + dy * dy);
                                    if (d >= best[v]) continue;
                                    best[v] = d;
                                    dxs[v] = dxs[i0] + (dxs[i1] - dxs[i0]) * u;
                                    dys[v] = dys[i0] + (dys[i1] - dys[i0]) * u;
                                    done[v] = 1;
                                }
                    }
                }
            }
            free(head); free(nxt); free(best);
        }
    }
    /* Neighbours: the mean motion of the placed vertices of the triangles
     * that share the position (1/16 px). */
    uint32_t nn = 0;
    for (uint32_t i = 0; i < n * 3u; i++) nn += verts[i].mode == FG_PLACE_NEIGHBOUR;
    if (nn) {
        uint32_t cap = 64;
        while (cap < nn * 2u) cap <<= 1;
        NAcc *tab = (NAcc *)calloc(cap, sizeof *tab);
        if (tab) {
            for (uint32_t j = 0; j < n; j++) {
                float sx = 0, sy = 0; int c = 0;
                for (int k = 0; k < 3; k++) {
                    const uint8_t m = verts[3 * j + k].mode;
                    if ((m == FG_PLACE_CAMERA || m == FG_PLACE_OBJECT) && !isnan(dxs[3 * j + k])) {
                        sx += dxs[3 * j + k]; sy += dys[3 * j + k]; c++;
                    }
                }
                if (!c) continue;
                /* The mean stands in for a motion that varies across the
                 * triangle with depth; where it varies by more than a pixel
                 * the guess opens cracks (near walls split by the game). */
                float spread = 0;
                for (int k = 0; k < 3; k++) {
                    const uint8_t m = verts[3 * j + k].mode;
                    if (m != FG_PLACE_CAMERA && m != FG_PLACE_OBJECT) continue;
                    const float ex = dxs[3 * j + k] - sx / c, ey = dys[3 * j + k] - sy / c;
                    if (ex * ex + ey * ey > spread) spread = ex * ex + ey * ey;
                }
                for (int k = 0; k < 3; k++) {
                    if (verts[3 * j + k].mode != FG_PLACE_NEIGHBOUR || (done && done[3 * j + k])) continue;
                    if (spread > 1.0f) guessed++;
                    const int32_t qx = (int32_t)lrintf(newer->v[j].x[k] * 16.0f), qy = (int32_t)lrintf(newer->v[j].y[k] * 16.0f);
                    uint32_t h = ((uint32_t)qx * 73856093u ^ (uint32_t)qy * 19349663u) & (cap - 1);
                    while (tab[h].n && (tab[h].qx != qx || tab[h].qy != qy)) h = (h + 1) & (cap - 1);
                    tab[h].qx = qx; tab[h].qy = qy;
                    tab[h].dx += sx / c; tab[h].dy += sy / c; tab[h].n++;
                }
            }
            for (uint32_t j = 0; j < n; j++)
                for (int k = 0; k < 3; k++) {
                    const uint32_t i = 3 * j + k;
                    if (verts[i].mode != FG_PLACE_NEIGHBOUR || (done && done[i])) continue;
                    const int32_t qx = (int32_t)lrintf(newer->v[j].x[k] * 16.0f), qy = (int32_t)lrintf(newer->v[j].y[k] * 16.0f);
                    uint32_t h = ((uint32_t)qx * 73856093u ^ (uint32_t)qy * 19349663u) & (cap - 1);
                    while (tab[h].n && (tab[h].qx != qx || tab[h].qy != qy)) h = (h + 1) & (cap - 1);
                    if (tab[h].n) { dxs[i] = tab[h].dx / tab[h].n; dys[i] = tab[h].dy / tab[h].n; }
                }
            free(tab);
        }
    }
    /* Weld: every placed vertex at one screen position (1/16 px) of one view
     * moves alike. The same corner reaches the GPU from several projections
     * (another function, another model vertex, an object and the world); if
     * they moved apart, the shared edge would crack open onto whatever was
     * drawn behind it (sky through a tunnel ceiling). */
    {
        uint32_t cap = 64;
        while (cap < n * 6u) cap <<= 1;
        NAcc *tab = (NAcc *)calloc(cap, sizeof *tab);
        uint32_t *slot = (uint32_t *)malloc((size_t)n * 3 * sizeof *slot);
        if (tab && slot) {
            for (uint32_t i = 0; i < n * 3u; i++) {
                slot[i] = UINT32_MAX;
                if (verts[i].mode == FG_PLACE_UNCHANGED) continue;
                const FgPrim *pr = &newer->v[i / 3];
                const int32_t qx = (int32_t)lrintf(pr->x[i % 3] * 16.0f) ^ (verts[i].view << 24);
                const int32_t qy = (int32_t)lrintf(pr->y[i % 3] * 16.0f);
                uint32_t h = ((uint32_t)qx * 73856093u ^ (uint32_t)qy * 19349663u) & (cap - 1);
                while (tab[h].n && (tab[h].qx != qx || tab[h].qy != qy)) h = (h + 1) & (cap - 1);
                tab[h].qx = qx; tab[h].qy = qy;
                tab[h].dx += dxs[i]; tab[h].dy += dys[i]; tab[h].n++;
                slot[i] = h;
            }
            for (uint32_t i = 0; i < n * 3u; i++)
                if (slot[i] != UINT32_MAX) {
                    dxs[i] = tab[slot[i]].dx / tab[slot[i]].n;
                    dys[i] = tab[slot[i]].dy / tab[slot[i]].n;
                }
        }
        free(tab); free(slot);
    }
    free(done);
    fit->clamped = clamped;
    fit->guessed = guessed;
    /* Small views (a rear-view mirror inside the main view) stay as the
     * real frame drew them: their camera is fitted from a few dozen
     * vertices, and a wrong fit threw their lane marks across the screen as
     * a dashed line. At this size their 30 Hz motion does not show. */
    {
        float big = 0.0f;
        for (uint32_t vi = 0; vi < fit->nviews; vi++) {
            const float *ar = fit->v[vi].area;
            const float a = (ar[2] - ar[0]) * (ar[3] - ar[1]);
            if (a > big) big = a;
        }
        for (uint32_t i = 0; i < n * 3u; i++) {
            const int vi = verts[i].view;
            if (vi < 0 || (uint32_t)vi >= fit->nviews) continue;
            const float *ar = fit->v[vi].area;
            if ((ar[2] - ar[0]) * (ar[3] - ar[1]) < 0.15f * big) { dxs[i] = 0.0f; dys[i] = 0.0f; }
        }
    }
    for (uint32_t i = 0; i < n * 3u; i++) { x[i] += dxs[i]; y[i] += dys[i]; }
    /* Margins: vertices on or past a view edge that moved inward. */
    if (margin)
        for (uint32_t j = 0; j < n; j++)
            for (int k = 0; k < 3; k++) {
                const uint32_t i = 3 * j + k;
                const int vi = verts[i].view;
                if (vi < 0 || verts[i].mode == FG_PLACE_UNCHANGED || isnan(dxs[i])) continue;
                const float *ar = fit->v[vi].area, bx = newer->v[j].x[k], by = newer->v[j].y[k];
                /* Geometry that reached past an edge now ends this far inside it. */
                float m;
                if (bx <= ar[0] + 1.0f && (m = bx + dxs[i] - ar[0]) > margin[vi][0]) margin[vi][0] = m;
                if (by <= ar[1] + 1.0f && (m = by + dys[i] - ar[1]) > margin[vi][1]) margin[vi][1] = m;
                if (bx >= ar[2] - 1.0f && (m = ar[2] - (bx + dxs[i])) > margin[vi][2]) margin[vi][2] = m;
                if (by >= ar[3] - 1.0f && (m = ar[3] - (by + dys[i])) > margin[vi][3]) margin[vi][3] = m;
            }
    free(dxs); free(dys);
}

int fg_plan(double flip_s, double refresh_hz, double real_cost_s,
            double gen_cost_s, double budget, int max_gens) {
    if (flip_s <= 0.0 || refresh_hz <= 0.0 || max_gens <= 0) return 0;
    int slots = (int)floor(flip_s * refresh_hz + 0.5);
    if (slots < 2) return 0;
    int n = slots - 1;
    if (n > max_gens) n = max_gens;
    const double room = budget * flip_s - (real_cost_s > 0.0 ? real_cost_s : 0.0);
    if (room <= 0.0) return 0;
    if (gen_cost_s <= 0.0) return room >= 0.5 * budget * flip_s ? 1 : 0;
    int fit = (int)floor(room / gen_cost_s);
    return fit < n ? (fit > 0 ? fit : 0) : n;
}

double fg_step_s(double flip_s, double refresh_hz, int n) {
    if (refresh_hz > 0.0) return 1.0 / refresh_hz;
    if (n <= 0 || flip_s <= 0.0) return flip_s > 0.0 ? flip_s : 0.0;
    return flip_s / (double)(n + 1);
}

double fg_plan_hz(double flip_s, double refresh_hz) {
    if (flip_s <= 0.0 || refresh_hz <= 0.0) return refresh_hz;
    /* Room for every display interval a game frame can hold (3.34 at
     * 100 Hz -> 4 slots); the clock drops the one that does not fit. */
    double slots = ceil(flip_s * refresh_hz - 1e-6);
    return slots / flip_s;
}

uint64_t fg_next_due(uint64_t due, uint64_t now, uint64_t step_ns) {
    /* On the grid while close to it; a late present restarts it, so frames
     * are never closer together than one interval. */
    if (due != 0u && now >= due && now - due < step_ns / 2u) return due + step_ns;
    return now + step_ns;
}

double fg_clock_phase(uint64_t since_real_ns, uint64_t step_ns, uint64_t flip_ns) {
    if (flip_ns == 0u) return 0.0;
    double t = ((double)since_real_ns + (double)step_ns) / (double)flip_ns;
    /* At (or past) the next real frame's time it is the real one. The
     * phase is where the camera is when this frame is seen, one interval
     * from now, so the last in-between frame of a game frame lands close
     * to 1 (a quarter-interval margin here dropped it at every refresh). */
    (void)step_ns;
    if (t >= 0.985) return 0.0;
    return t;
}

void fg_breaker_init(FgBreaker *b, double base_hold, double max_hold, double repeat_s) {
    memset(b, 0, sizeof *b);
    b->base_hold = base_hold;
    b->max_hold = max_hold;
    b->repeat_s = repeat_s;
    b->last_end = -1e30;
    b->until = -1e30;
}

void fg_breaker_trip(FgBreaker *b, double now, const char *reason) {
    b->trips++;
    b->reason = reason;
    if (now < b->until) return;   /* already open: the hold stands */
    if (b->hold > 0.0 && now - b->last_end <= b->repeat_s) {
        b->hold *= 2.0;
        if (b->hold > b->max_hold) b->hold = b->max_hold;
    } else {
        b->hold = b->base_hold;
    }
    b->until = now + b->hold;
    b->last_end = b->until;
}

int fg_breaker_open(const FgBreaker *b, double now) { return now >= b->until; }

void fg_cost_init(FgCost *c, double probe_s, double max_probe_s) {
    memset(c, 0, sizeof *c);
    c->base_probe_s = c->probe_s = probe_s;
    c->max_probe_s = max_probe_s;
    c->blocked_since = -1.0;
}

void fg_cost_cold(FgCost *c, int n) { if (n > c->cold) c->cold = n; }

void fg_cost_add(FgCost *c, double cost_s, double fit_s) {
    if (cost_s <= 0.0) return;
    if (c->cold > 0) { c->cold--; c->discarded++; return; }
    c->samples++;
    if (c->probing || c->ema <= 0.0) {
        c->ema = cost_s;
        if (c->probing) {
            c->probing = 0;
            if (fit_s > 0.0 && cost_s > fit_s) {
                c->probe_s *= 2.0;
                if (c->probe_s > c->max_probe_s) c->probe_s = c->max_probe_s;
            } else {
                c->probe_s = c->base_probe_s;
            }
        }
    } else {
        /* One stall (an allocation, a driver hiccup) moves the estimate by
         * at most a few times itself; a lasting change still wins. */
        if (c->clamp_spikes && cost_s > 4.0 * c->ema) cost_s = 4.0 * c->ema;
        c->ema = c->ema * 0.8 + cost_s * 0.2;
    }
    if (fit_s <= 0.0 || c->ema <= fit_s) c->blocked_since = -1.0;
}

double fg_cost_estimate(FgCost *c, double now, double fit_s) {
    if (c->ema <= 0.0) return 0.0;
    if (c->probing) {
        /* A probe in flight; one never drawn (no room) is given up. */
        if (now - c->probe_at < c->probe_s) return c->ema;
        c->probing = 0;
        c->blocked_since = now;
    }
    if (fit_s > 0.0 && c->ema > fit_s) {
        if (c->blocked_since < 0.0) c->blocked_since = now;
        else if (now - c->blocked_since >= c->probe_s) {
            c->blocked_since = now;
            c->probing = 1;
            c->probe_at = now;
            c->probes++;
            return 0.0;
        }
    } else {
        c->blocked_since = -1.0;
    }
    return c->ema;
}

int fg_pace_note(FgPace *p, double now, double period_s, double slack_s) {
    if (period_s <= 0.0) return 0;
    if (!p->primed) { p->primed = 1; p->next = now + period_s; return 0; }
    int late = 0;
    if (now > p->next + slack_s) { late = 1; p->next = now; }
    else if (now < p->next - period_s) p->next = now;   /* early: pull in */
    p->next += period_s;
    return late;
}

void fg_ceiling_init(FgCeiling *c, int max, double recover_s) {
    c->cap = c->max = max;
    c->last = -1e30;
    c->recover_s = recover_s;
}

void fg_ceiling_trip(FgCeiling *c, int n_planned, double now) {
    if (n_planned <= 0) return;
    const int to = n_planned - 1 > 1 ? n_planned - 1 : 1;
    if (to < c->cap) c->cap = to;
    c->last = now;
}

int fg_ceiling_get(FgCeiling *c, double now) {
    while (c->cap < c->max && now - c->last >= c->recover_s) {
        c->cap++;
        c->last = c->last < -1e29 ? now : c->last + c->recover_s;
    }
    return c->cap;
}

/* HUD motion: 2D triangles (no GTE projection: gauges, needles) of the
 * redrawn list L that the other frame O draws too (same key: op, texture,
 * colour, UVs, in the same order among the 2D triangles) and that moved a
 * little (at most max_px per corner: a tachometer needle, a sliding panel)
 * are placed at fraction u of the way from L to O. Anything that changed
 * what it draws (digits) or jumped stays as L drew it. */
/* The 2D triangles of L that fg_hud_lerp moves (moved[j] = 1). */
void fg_hud_match(const FgPrimList *L, const FgPrimList *O, float max_px, uint8_t *moved) {
    if (!L || !moved) return;
    memset(moved, 0, L->n);
    if (!O) return;
    float *x = (float *)malloc((size_t)L->n * 3 * sizeof *x), *y = (float *)malloc((size_t)L->n * 3 * sizeof *y);
    if (!x || !y) { free(x); free(y); return; }
    for (uint32_t j = 0; j < L->n; j++)
        for (int k = 0; k < 3; k++) { x[3 * j + k] = L->v[j].x[k]; y[3 * j + k] = L->v[j].y[k]; }
    fg_hud_lerp(L, O, 0.5, x, y, max_px);
    for (uint32_t j = 0; j < L->n; j++) {
        const FgPrim *p = &L->v[j];
        /* Gauge needles are small; a 2D backdrop gradient that shifts with
         * the camera is not one. */
        const float bw = fmaxf(p->x[0], fmaxf(p->x[1], p->x[2])) - fminf(p->x[0], fminf(p->x[1], p->x[2]));
        const float bh = fmaxf(p->y[0], fmaxf(p->y[1], p->y[2])) - fminf(p->y[0], fminf(p->y[1], p->y[2]));
        if (bw > 48.0f || bh > 48.0f) continue;
        for (int k = 0; k < 3; k++)
            if (x[3 * j + k] != p->x[k] || y[3 * j + k] != p->y[k]) moved[j] = 1;
    }
    free(x); free(y);
}

void fg_hud_lerp(const FgPrimList *L, const FgPrimList *O, double u, float *x, float *y,
                 float max_px) {
    if (!L || !O || u <= 0.0 || u >= 1.0) return;
    uint32_t o = 0;
    for (uint32_t j = 0; j < L->n; j++) {
        const FgPrim *a = &L->v[j];
        if (a->vid[0] || a->vid[1] || a->vid[2]) continue;
        uint32_t m = o, seen = 0;
        for (; m < O->n && seen < 8u; m++) {
            const FgPrim *b = &O->v[m];
            if (b->vid[0] || b->vid[1] || b->vid[2]) continue;
            seen++;
            if (b->key == a->key && b->view == a->view) break;
        }
        if (m >= O->n || seen >= 8u) continue;
        const FgPrim *b = &O->v[m];
        o = m + 1;
        int ok = 1, moved = 0;
        for (int k = 0; k < 3; k++) {
            const float dx = b->x[k] - a->x[k], dy = b->y[k] - a->y[k];
            if (fabsf(dx) > max_px || fabsf(dy) > max_px) ok = 0;
            if (dx != 0.0f || dy != 0.0f) moved = 1;
        }
        if (!ok || !moved) continue;
        for (int k = 0; k < 3; k++) {
            x[3 * j + k] = a->x[k] + (b->x[k] - a->x[k]) * (float)u;
            y[3 * j + k] = a->y[k] + (b->y[k] - a->y[k]) * (float)u;
        }
    }
}

/* The in-between camera of view vi at phase t (fg_cam_place's motion) as an
 * affine map on camera-space points of the newer frame: P_t = A P + b. */
int fg_view_affine(const FgCamFit *fit, int vi, double t, float A[9], float b[3]) {
    if (!fit || vi < 0 || (uint32_t)vi >= fit->nviews) return 0;
    const FgView *v = &fit->v[vi];
    double M[9], Rt[9];
    q_to_m(v->q, M);
    const double w = fmin(1.0, fabs(v->q[0]));
    const double ang = 2.0 * acos(w), sn = sqrt(fmax(0.0, 1.0 - w * w));
    double qt[4] = { 1, 0, 0, 0 };
    if (sn > 1e-9) {
        const double sg = v->q[0] < 0 ? -1.0 : 1.0;
        qt[0] = cos(0.5 * ang * t);
        for (int k = 0; k < 3; k++) qt[k + 1] = sg * v->q[k + 1] / sn * sin(0.5 * ang * t);
    }
    q_to_m(qt, Rt);
    double Am[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)   /* (Rt M^T)[r][c] = sum_k Rt[r][k] M[c][k] */
            Am[3 * r + c] = Rt[3 * r] * M[3 * c] + Rt[3 * r + 1] * M[3 * c + 1] + Rt[3 * r + 2] * M[3 * c + 2];
    for (int k = 0; k < 9; k++) A[k] = (float)Am[k];
    for (int r = 0; r < 3; r++)
        b[r] = (float)(t * v->t[r] - (Am[3 * r] * v->t[0] + Am[3 * r + 1] * v->t[1] + Am[3 * r + 2] * v->t[2]));
    return 1;
}
