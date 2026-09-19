/* gpu_uv.h — the ONE shared PS1 uv-sampling model for all render backends.
 *
 * The PS1 DDA latches interpolants at each pixel's TOP-LEFT corner and
 * truncates, so along any axis-aligned 2D mapping the prim's exclusive
 * raster edge (+x right / +y bottom) never samples its end texel. Which uv
 * extreme that is depends on the mapping direction: forward (uv increasing
 * toward the exclusive edge) never samples max-uv; mirrored (uv decreasing
 * — X/Y-flipped 2D sprites) never samples min-uv. The helpers here encode
 * that model ONCE:
 *
 *   psx_uv_tri_limits / psx_uv_rect_limits
 *     Exact inclusive sampled texel range for the mapping direction, on the
 *     prim's ORIGINAL uv values: forward [lo, hi-1], mirrored [lo+1, hi].
 *     Crossing a 256 boundary means the prim relies on page wrapping — the
 *     bounds widen to the full page (clamp disabled). Used as sampling
 *     clamps (bilinear neighbours, S>1 interpolation overshoot).
 *
 *   psx_uv_tri_mirror_offset / psx_uv_rect_mirror_offset
 *     The Beetle-PSX / parallel-psx Calc_UVOffsets_Adjust_Verts model for
 *     CENTER-SAMPLED rasterizers (GL/VK). Their sample-grid shift
 *     (u_shift = 0.5/S - 1/64) makes floor(uv) land on the exact PS1 texel
 *     for forward mappings, but mirrored ones interpolate 1/64 SHORT of
 *     each integer and floor one texel LOW — painting the cel's
 *     never-sampled edge column/row as detached 1px slivers (MMX6 sprite
 *     slivers; pink line under rolling wheels). Compensate by bumping the
 *     prim's uv +1 along each decreasing axis. The software rasterizer's
 *     own DDA truncates exactly like the PS1 and must NOT apply this.
 *
 * Derivative direction is area2-normalized (winding-independent); diagonal
 * (3D-ish) mappings — both derivatives nonzero on an axis — get no
 * compensation and no back-off tightening on that axis. Like Beetle, a rare
 * 3D poly that happens to be axis-aligned accepts a one-texel shift in
 * exchange for correct 2D sprites.
 *
 * History: GL, VK and SW each carried a private copy of this math; the
 * copies drifted (the mirrored back-off bug lived only in the GPU backends)
 * and the fix then had to be hand-mirrored into five sites. One model, one
 * file, all backends. */

#ifndef PSXRECOMP_GPU_UV_H
#define PSXRECOMP_GPU_UV_H

/* Signed uv step along each screen axis, area2-scaled (only signs matter;
 * area2's sign normalizes winding). du/dv are the axis-aligned direction:
 * the nonzero-vs-zero derivative pair, or 0 when the mapping is diagonal. */
static inline void psx_uv_tri_dirs(const int *xs, const int *ys,
                                   const int *us, const int *vs,
                                   long *du, long *dv) {
    long dudx = -(long)(ys[1]-ys[0])*us[2] - (long)(ys[2]-ys[1])*us[0] - (long)(ys[0]-ys[2])*us[1];
    long dvdx = -(long)(ys[1]-ys[0])*vs[2] - (long)(ys[2]-ys[1])*vs[0] - (long)(ys[0]-ys[2])*vs[1];
    long dudy =  (long)(xs[1]-xs[0])*us[2] + (long)(xs[2]-xs[1])*us[0] + (long)(xs[0]-xs[2])*us[1];
    long dvdy =  (long)(xs[1]-xs[0])*vs[2] + (long)(xs[2]-xs[1])*vs[0] + (long)(xs[0]-xs[2])*vs[1];
    long area2 = (long)(xs[1]-xs[0])*(ys[2]-ys[0]) - (long)(xs[2]-xs[0])*(ys[1]-ys[0]);
    if (area2 == 0) { *du = 0; *dv = 0; return; }
    *du = dudx == 0 ? dudy : (dudy == 0 ? dudx : 0);
    *dv = dvdx == 0 ? dvdy : (dvdy == 0 ? dvdx : 0);
    if (area2 < 0) { *du = -*du; *dv = -*dv; }
}

/* Pre-wrap one axis' exact sampled range into lim_lo/lim_hi. dir > 0 =
 * forward (hi exclusive), dir < 0 = mirrored (lo exclusive), 0 = diagonal
 * (keep full min..max). */
static inline void psx_uv_axis_limits(int lo, int hi, long dir,
                                      int *lim_lo, int *lim_hi) {
    if (dir > 0 && hi > lo) hi--;
    if (dir < 0 && hi > lo) lo++;
    if ((lo >> 8) == (hi >> 8)) { lo &= 255; hi &= 255; }
    else                        { lo = 0; hi = 255; }
    *lim_lo = lo; *lim_hi = hi;
}

/* Inclusive sampled uv bounds for a textured triangle, from its ORIGINAL
 * (uncompensated) vertex uvs. lim = {lo_u, lo_v, hi_u, hi_v}. */
static inline void psx_uv_tri_limits(const int *xs, const int *ys,
                                     const int *us, const int *vs, int lim[4]) {
    int lo_u = us[0], hi_u = us[0], lo_v = vs[0], hi_v = vs[0];
    for (int i = 1; i < 3; i++) {
        if (us[i] < lo_u) lo_u = us[i]; if (us[i] > hi_u) hi_u = us[i];
        if (vs[i] < lo_v) lo_v = vs[i]; if (vs[i] > hi_v) hi_v = vs[i];
    }
    long du, dv;
    psx_uv_tri_dirs(xs, ys, us, vs, &du, &dv);
    psx_uv_axis_limits(lo_u, hi_u, du, &lim[0], &lim[2]);
    psx_uv_axis_limits(lo_v, hi_v, dv, &lim[1], &lim[3]);
}

/* Inclusive sampled uv bounds for a rect prim, from its ORIGINAL uv corners
 * (u0/v0 at the top-left raster corner; u1/v1 at the exclusive corner —
 * u1 < u0 / v1 < v0 means mirrored). */
static inline void psx_uv_rect_limits(int u0, int v0, int u1, int v1, int lim[4]) {
    int lo_u = u0 < u1 ? u0 : u1, hi_u = u0 < u1 ? u1 : u0;
    int lo_v = v0 < v1 ? v0 : v1, hi_v = v0 < v1 ? v1 : v0;
    psx_uv_axis_limits(lo_u, hi_u, (long)(u1 - u0), &lim[0], &lim[2]);
    psx_uv_axis_limits(lo_v, hi_v, (long)(v1 - v0), &lim[1], &lim[3]);
}

/* Mirrored-2D compensation for center-sampled rasterizers (GL/VK ONLY —
 * never the software DDA): bump the uv +1 along each decreasing axis.
 * Triangle form mutates the 3-vertex uv arrays in place. */
static inline void psx_uv_tri_mirror_offset(const int *xs, const int *ys,
                                            int *us, int *vs) {
    long du, dv;
    psx_uv_tri_dirs(xs, ys, us, vs, &du, &dv);
    if (du < 0) { us[0]++; us[1]++; us[2]++; }
    if (dv < 0) { vs[0]++; vs[1]++; vs[2]++; }
}

/* Rect form: bump each mirrored axis' corner pair. */
static inline void psx_uv_rect_mirror_offset(int *u0, int *v0, int *u1, int *v1) {
    if (*u1 < *u0) { (*u0)++; (*u1)++; }
    if (*v1 < *v0) { (*v0)++; (*v1)++; }
}

#endif /* PSXRECOMP_GPU_UV_H */
