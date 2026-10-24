#ifndef PSX_WS_CULL_EDGE_H
#define PSX_WS_CULL_EDGE_H
/* ============================================================================
 * Exact screen-X edge widening for explicit [widescreen.cull] site kinds
 *
 * Self-contained (depends only on <stdint.h>) so one implementation is shared
 * by the runtime helpers in gpu.c (native code and the dirty-RAM interpreter
 * both call those) and by the recompiler's unit test.
 *
 * `m` is the live per-side reveal, psx_ws_x_margin(): zero at 4:3, so every
 * function is the identity there and faithful builds are unchanged.
 *
 * bgez_sites -- `bgez SX, keep` in a signed per-vertex left-edge chain:
 *
 *     bgez  x0, keep      ; any vertex with SX >= 0 keeps the primitive
 *     bgez  x1, keep
 *     bgez  x2, keep
 *     bltz  x3, reject    ; only reached when x0..x2 < 0
 *
 * The final `bltz` is the existing bltz_sites kind (reject iff SX < -m). On
 * its own it is not exact: a quad with x0 in [-m, 0) and x3 < -m is still
 * rejected, although vertex 0 lies in the revealed band. Widening every
 * `bgez` to SX >= -m restores "reject iff every vertex is left of -m".
 *
 * clip_edge_x_load_sites -- a load of a screen-X clip bound (lh/lhu/lw) that
 * a renderer compares its projected vertices against:
 *
 *     lh   xmax, 0x70(a0) ; e.g. a scratchpad clip rectangle
 *     slt  at, xmax, SX   ; ... reject if every SX > xmax
 *
 * A bound equal to the screen's left edge (0) moves to -m and one equal to
 * its right edge (W) moves to W+m. Any other value is an interior viewport
 * edge (a rear-view mirror, one half of a split screen) and stays unchanged,
 * so interior viewports keep their vanilla culls while full-width viewports
 * widen on each side that touches the display edge.
 *
 * screen_x_sites on `lui` and `bltz` -- a screen X kept in the high half of
 * a register (`SX << 16`), as a renderer does when it tests the sign of
 * differences and masks whole points at once:
 *
 *     lui   edge, W       ; right screen edge as W << 16
 *     sll   x, sx, 16
 *     sub   t, x, edge    ; t >= 0: the point is right of the screen
 *     ...
 *     bltz  x, outside    ; the point is left of the screen
 *
 * The `lui` moves to (W + m) << 16 and the `bltz` is taken only while
 * SX < -m. bltz_sites does not fit this `bltz`: it compares the whole
 * register with -m, and every negative `SX << 16` is far below that.
 * `slti rt, x, 0` on the same `SX << 16` value is the same sign test as a
 * flag instead of a branch, and screen_x_sites widens it identically.
 *
 * [[widescreen.cull.sxy]] -- the all-vertices trivial reject over packed GTE
 * SXY words (Y in the high half, X in the low half), folded with AND:
 *
 *     and   r, v0, mask   ; mask holds the sign bits 0x80008000
 *     and   r, v1, r
 *     and   r, v2, r      ; (and r, v3, r for a quad)
 *     bne   r, zero, reject
 *
 * Bit 31 of r survives only if every Y is negative (all above the screen)
 * and bit 15 only if every X is negative (all left of it). Each AND of the
 * chain becomes psx_ws_sxy_and_value(acc, v, m): the vanilla AND, except that
 * bit 15 also needs this vertex's X < -m. The fold then rejects to the left
 * only when every vertex is left of the revealed edge. Every other bit,
 * including the vertical bit 31, is the vanilla AND, and at m == 0 the whole
 * word is the vanilla AND. Each step reads its operands when the guest's
 * own AND does, so the chain stays exact whatever the guest does between
 * steps.
 * ========================================================================== */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bgez predicate (1 = branch taken = keep) for a signed screen X. */
static inline int psx_ws_cull_bgez_value(int32_t v, int32_t m)
{
    return v >= -(m > 0 ? m : 0);
}

/* Widened clip bound. `v` is the loaded value as the guest sees it
 * (sign-extended for lh, zero-extended for lhu). The result is a signed
 * 32-bit screen X in two's complement. */
static inline uint32_t psx_ws_clip_edge_x_value(uint32_t v, uint32_t w,
                                                int32_t m)
{
    if (m <= 0) return v;
    if (v == 0u) return (uint32_t)(-m);
    if (v == w) return (uint32_t)((int32_t)w + m);
    return v;
}

/* Widened value of `lui rt, imm` when imm is a screen-X edge in pixels: a
 * right edge (imm > 0) moves right by m, a left edge (imm < 0) moves left by
 * m. `imm` is the 16-bit immediate as encoded. */
static inline uint32_t psx_ws_cull_lui_hi_value(uint32_t imm, int32_t m)
{
    int32_t edge = (int32_t)(int16_t)(uint16_t)imm;
    if (m > 0) {
        if (edge > 0) edge += m;
        else if (edge < 0) edge -= m;
    }
    return (uint32_t)edge << 16;
}

/* bltz predicate (1 = branch taken = outside) for a screen X in the high
 * half of `v`: taken while SX < -m. */
static inline int psx_ws_cull_bltz_hi_value(uint32_t v, int32_t m)
{
    return (int32_t)v < -((m > 0 ? m : 0) * 65536);
}

/* One AND of a [[widescreen.cull.sxy]] fold: `acc & sxy`, keeping bit 15 (X
 * sign) only while this vertex's X (low half of `sxy`) is left of -m. */
static inline uint32_t psx_ws_sxy_and_value(uint32_t acc, uint32_t sxy,
                                            int32_t m)
{
    const uint32_t r = acc & sxy;
    if (m <= 0 || !(r & 0x8000u)) return r;
    if ((int32_t)(int16_t)(uint16_t)sxy < -m) return r;
    return r & ~0x8000u;
}

#ifdef __cplusplus
}
#endif
#endif /* PSX_WS_CULL_EDGE_H */
