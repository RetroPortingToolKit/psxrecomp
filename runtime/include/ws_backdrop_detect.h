#ifndef PSX_WS_BACKDROP_DETECT_H
#define PSX_WS_BACKDROP_DETECT_H
/* ============================================================================
 * Generic widescreen BACKDROP-PRELOAD detector ([widescreen.cull] auto_backdrop)
 *
 * Self-contained (depends only on <stdint.h>) so ONE implementation is shared
 * verbatim by the recompiler (C++) and the runtime interpreter
 * (C). Having a single source of truth matters here: a false positive corrupts
 * unrelated overlay codegen (see BACKDROP_PRELOAD.md §risk), so the gate must be
 * identical on every path.
 *
 * WHAT IT FINDS
 * -------------
 * PSX scrollers generate a far-background tile row one CAMERA-WINDOWED column
 * range per frame. The generator divides the camera world-X by 80 to a column
 * index and emits a small window [START, END] of columns around it:
 *
 *     lui  M,0x6666 ; ori M,M,0x6667     ; M = 0x66666667 (/80 reciprocal)
 *     lh/lhu D,0x176(base)               ; D = camera world-X  (offset 0x176)
 *     [addiu/addu D, D, Koff]            ; optional per-layer parallax bias
 *     mult D, M
 *     mfhi H ; sra Q,H,5 ; subu/addu Q,Q,sign   ; Q = camX / 80  (the quotient)
 *     move  rStart, Q                    ; START finalize (offset 0)
 *     addiu rEnd,  Q, N                  ; END   delta     (offset N)
 *     <low clamp on START to a floor>  <high clamp on END to the finite extent>
 *     for (col = START; col <= END; col++) emit one tile from a finite table
 *
 * In 16:9 the window slides with the camera, so the revealed margin shows void.
 * The fix WIDENS the camera-tracked window by exactly the 16:9 reveal: pull the
 * START bound a little left and push the END bound a little right, by a margin
 * proportional to the window width (the revealed fraction of the screen). This
 * draws only the now-visible columns — NOT the whole finite row — so there is no
 * overdraw (an early "force START=0 / END=full-row" version lagged on long rows
 * and capped short on rows > the fixed cap). The generator's own low/high clamps
 * still bound the widened window at the level edges.
 *
 * THE TWO REWRITE SITES (per window)
 * ----------------------------------
 * Two independent affine consumers finalize bounds: `move rX,Q` (offset 0)
 * or `addiu rX,Q,bias`. Both may be biased when a table packs multiple strips.
 * Which is START and which is END is NOT
 * fixed by move-vs-addiu — it is decided by the generator's clamp tail and is
 * captured generically by OFFSET ORDERING: the smaller signed offset is the
 * START (left) bound, the larger is the END (right) bound. Verified on Tomba's
 * village (FUN_80116a28: a2=START/a3=END, +8) and flower-field (s2=START/s1=END;
 * one window has the addiu at offset -18 acting as START with the move as END).
 * The window width in columns is the DIFFERENCE between offsets — recorded so
 * the runtime sizes the widen margin to the reveal.
 *
 * The actual value substitution is done at runtime by psx_ws_backdrop_value()
 * (gpu.c): identity unless native-wide widescreen is engaged, so 4:3 stays
 * byte-identical. It returns orig-margin for a START site and orig+margin for an
 * END site. This detector only locates the addresses + the window width.
 *
 * VALIDATION IS PER-MULT AND LOCAL (backward), not forward-accumulated: the /96
 * magic is sometimes established in a shared branch-delay slot, so register
 * state from one branch leg would bleed into another under a forward scan. Each
 * candidate `mult` is gated by a bounded backward scan for (a) the exact magic
 * load into one operand and (b) a 0x176 camera load into the other operand.
 * ========================================================================== */
#include <stdint.h>

enum {
    WS_BD_NONE  = 0,
    WS_BD_START = 1,   /* the window's left (smaller-offset) bound  -> orig - margin */
    WS_BD_END   = 2     /* the window's right (larger-offset) bound  -> orig + margin */
};

typedef struct {
    uint32_t pc;          /* absolute guest address of the instruction to rewrite */
    int      kind;        /* WS_BD_START | WS_BD_END */
    int      window_cols; /* window width in columns (|addiu offset|), drives the widen margin */
} WsBackdropSite;

/* Backward def/use proof for the dividend. Some strips reuse a camera value
 * loaded for a preceding strip, or keep &scratchpad.camera_x in a saved reg.
 * Follow copies/biases, not just a matching load somewhere in the lookback. */
static inline int psx_ws_bd_written_reg(uint32_t w) {
    unsigned op = w >> 26, fn = w & 63u;
    if (!op) {
        if (fn <= 7u || fn == 9u || fn == 0x10u || fn == 0x12u ||
            (fn >= 0x20u && fn <= 0x2Bu)) return (int)((w >> 11) & 31u);
    } else if ((op >= 8u && op <= 15u) || (op >= 0x20u && op <= 0x26u) ||
               ((op == 0x10u || op == 0x12u) && ((w >> 21) & 31u) < 3u)) {
        return (int)((w >> 16) & 31u);
    } else if (op == 3u) return 31;
    return -1;
}

/* An unconditional jump before a block means the lexically preceding arm
 * does not reach it. Follow a unique local conditional predecessor (including
 * its delay slot), never borrow definitions from the skipped arm. */
static inline int psx_ws_bd_predecessor(const uint32_t* words, int boundary, int lo) {
    int found = -1;
    for (int b = boundary - 3; b >= lo; --b) {
        unsigned op = words[b] >> 26;
        if ((op == 1u || (op >= 4u && op <= 7u)) &&
            b + 1 + (int)(int16_t)words[b] == boundary) {
            if (found >= 0) return -1;
            found = b;
        }
    }
    return found;
}

static inline int psx_ws_bd_constant(const uint32_t* words, int before, int lo,
                                    unsigned reg, unsigned depth, uint32_t* value) {
    if (!reg) { *value = 0; return 1; }
    if (!depth) return 0;
    for (int b = before - 1; b >= lo; --b) {
        if (b > lo && (words[b - 1] >> 26) == 2u) {
            int pred = psx_ws_bd_predecessor(words, b + 1, lo);
            return pred >= 0 && psx_ws_bd_constant(words, pred + 2, lo,
                                                   reg, depth - 1, value);
        }
        uint32_t w = words[b], op = w >> 26, fn = w & 63u;
        unsigned rs = (w >> 21) & 31u, rt = (w >> 16) & 31u;
        /* A call may clobber every caller-saved register. */
        if ((op == 3u || (!op && fn == 9u)) && (reg < 16u || reg > 23u)) return 0;
        if (psx_ws_bd_written_reg(w) != (int)reg) continue;
        if (op == 15u) { *value = w << 16; return 1; }
        uint32_t source;
        if ((op == 9u || op == 13u) &&
            psx_ws_bd_constant(words, b, lo, rs, depth - 1, &source)) {
            *value = op == 13u ? source | (w & 0xFFFFu)
                              : source + (uint32_t)(int32_t)(int16_t)w;
            return 1;
        }
        if (!op && (fn == 0x21u || fn == 0x25u) && (!rs || !rt))
            return psx_ws_bd_constant(words, b, lo, rs ? rs : rt, depth - 1, value);
        return 0;
    }
    return 0;
}

static inline int psx_ws_bd_camera(const uint32_t* words, int before, int lo,
                                  unsigned reg, unsigned depth) {
    if (!reg || !depth) return 0;
    for (int b = before - 1; b >= lo; --b) {
        if (b > lo && (words[b - 1] >> 26) == 2u) {
            int pred = psx_ws_bd_predecessor(words, b + 1, lo);
            return pred >= 0 && psx_ws_bd_camera(words, pred + 2, lo, reg, depth - 1);
        }
        uint32_t w = words[b], op = w >> 26, fn = w & 63u;
        unsigned rs = (w >> 21) & 31u, rt = (w >> 16) & 31u;
        if ((op == 3u || (!op && fn == 9u)) && (reg < 16u || reg > 23u)) return 0;
        if (psx_ws_bd_written_reg(w) != (int)reg) continue;
        if (op == 0x21u || op == 0x25u) {
            uint32_t base;
            return psx_ws_bd_constant(words, b, lo, rs, 4, &base) &&
                base + (uint32_t)(int32_t)(int16_t)w == 0x1F800176u;
        }
        if (op == 9u || op == 8u)
            return psx_ws_bd_camera(words, b, lo, rs, depth - 1);
        if (!op && (fn == 0x21u || fn == 0x25u || fn == 0x23u)) {
            /* OR is only a copy. SUBU permits camera - bias, never bias - camera. */
            if (fn == 0x25u && rs && rt) return 0;
            return psx_ws_bd_camera(words, b, lo, rs, depth - 1) ||
                (fn != 0x23u && psx_ws_bd_camera(words, b, lo, rt, depth - 1));
        }
        return 0; /* nearest definition is not a proven camera-derived value */
    }
    return 0;
}

/* Scan `n` instruction words starting at guest address `base_pc` for backdrop
 * column-window generators and record the START/END rewrite sites. Returns the
 * number of sites written to `out` (capped at `max_sites`). Pure: the result
 * depends only on the instruction bytes, so the recompiler (full-function
 * words) and the runtime (a window around a PC) derive identical sites. */
static inline int psx_ws_find_backdrop_windows(const uint32_t *words, int n,
                                               uint32_t base_pc,
                                               WsBackdropSite *out, int max_sites)
{
    int count = 0;
    if (!words || n <= 0 || !out || max_sites <= 0) return 0;

    for (int i = 0; i < n; i++) {
        uint32_t w = words[i];
        /* mult rs,rt : SPECIAL(op 0) funct 0x18 (signed multiply) */
        if ((w >> 26) != 0u || (w & 0x3Fu) != 0x18u) continue;
        uint32_t op_rs = (w >> 21) & 31u;
        uint32_t op_rt = (w >> 16) & 31u;

        /* One operand is the /96 magic, the other the camera-X dividend. Try
         * both assignments; validate each by a bounded local backward scan. */
        for (int swap = 0; swap < 2; swap++) {
            uint32_t M = swap ? op_rt : op_rs;   /* magic candidate    */
            uint32_t D = swap ? op_rs : op_rt;   /* dividend candidate  */
            if (M == D) continue;

            int blo = i - 48; if (blo < 0) blo = 0;
            uint32_t magic;
            if (!psx_ws_bd_constant(words, i, blo, M, 4, &magic) ||
                magic != 0x66666667u || !psx_ws_bd_camera(words, i, blo, D, 8)) continue;

            /* Forward divide tail: mfhi H -> sra Q,H,5. Bail on a control
             * transfer before the tail (the divide is straight-line). */
            int hi_reg = -1, quot = -1, jdiv = -1;
            int fhi = i + 1 + 16; if (fhi > n) fhi = n;
            for (int f = i + 1; f < fhi; f++) {
                uint32_t fw = words[f];
                uint32_t fop = fw >> 26;
                uint32_t ffn = fw & 0x3Fu;
                uint32_t frd = (fw >> 11) & 31u;
                uint32_t frt = (fw >> 16) & 31u;
                uint32_t fsa = (fw >> 6) & 31u;
                if (fop == 0u && ffn == 0x10u) { hi_reg = (int)frd; continue; }     /* mfhi rd */
                if (hi_reg >= 0 && fop == 0u && ffn == 0x03u && (int)frt == hi_reg && fsa == 5u) {
                    quot = (int)frd; jdiv = f; break;                               /* sra quot,hi,5 */
                }
                if (fop == 0x02u || fop == 0x03u || fop == 0x04u || fop == 0x05u ||
                    fop == 0x06u || fop == 0x07u || fop == 0x01u ||
                    (fop == 0u && (ffn == 0x08u || ffn == 0x09u))) break;           /* j/jal/branch/jr/jalr */
            }
            if (quot < 0) continue;

            /* Both bounds must independently read the intact quotient. A
             * dependent addiu can instead be a layer-table bias (Tomba's +28
             * after a clamp), NOT a window end. Independently shifting both
             * results is also wrong if one consumes the already-shifted other
             * bound. Leave those ambiguous shapes unchanged. */
            int cur = quot, move_dest = -1;
            uint32_t move_pc = 0, addiu_pc = 0;
            /* Historical names: move is the first affine bound, addiu the
             * second. Either may now have a nonzero table-segment bias. */
            int have_move = 0, have_addiu = 0, move_n = 0, addiu_n = 0;
            int fend = jdiv + 1 + 8; if (fend > n) fend = n;
            for (int f = jdiv + 1; f < fend && !(have_move && have_addiu); f++) {
                uint32_t fw = words[f];
                uint32_t fop = fw >> 26;
                uint32_t ffn = fw & 0x3Fu;
                uint32_t frd = (fw >> 11) & 31u;
                uint32_t frt = (fw >> 16) & 31u;
                uint32_t frs = (fw >> 21) & 31u;
                int      fsimm = (int)(int16_t)(uint16_t)(fw & 0xFFFFu);
                uint32_t pc = base_pc + (uint32_t)f * 4u;

                /* A jump's delay slot still executes in this block. Never
                 * scan beyond it into an unrelated branch leg. Conditional
                 * clamps are not part of the independent-bound pattern. */
                if (fop == 0x02u) {
                    if (fend > f + 2) fend = f + 2;
                    continue;
                }
                if (fop == 0x03u || (fop >= 0x04u && fop <= 0x07u) ||
                    fop == 0x01u || (fop == 0u && (ffn == 0x08u || ffn == 0x09u))) break;

                if (fop == 0u && (ffn == 0x21u || ffn == 0x25u)) {   /* addu/or */
                    int src = -1;
                    if (frt == 0u && frs != 0u) src = (int)frs;       /* move rD, rS  (rT == $0) */
                    else if (frs == 0u && frt != 0u) src = (int)frt;  /* move rD, rT  (rS == $0) */
                    if (src == cur) {
                        if ((int)frd == cur) break;
                        /* Some rows first clamp the signed quotient to zero
                         * and add a table-segment bias. Recognize the COMPLETE
                         * diamond, then match the real bounds after its join:
                         * move T,Q; sll Q,Q,16; bgez Q,+3; addiu Q,T,bias;
                         * move T,zero; addiu Q,T,bias; <bounds from Q>.
                         * Neither the temporary move nor bias is a bound. */
                        if (!have_move && !have_addiu && f + 6 < n &&
                            words[f + 1] == ((uint32_t)cur << 16 | (uint32_t)cur << 11 | 16u << 6) &&
                            words[f + 2] == (0x04010003u | (uint32_t)cur << 21) &&
                            (words[f + 3] & 0xFFFF0000u) ==
                                (0x24000000u | frd << 21 | (uint32_t)cur << 16) &&
                            words[f + 4] == (frd << 11 | 0x21u) &&
                            words[f + 5] == words[f + 3]) {
                            f += 5;
                            fend = f + 1 + 8;
                            if (fend > n) fend = n;
                            continue;
                        }
                        if (!have_move) {
                            move_pc = pc; move_dest = (int)frd; move_n = 0; have_move = 1;
                        } else if ((int)frd != move_dest) {
                            addiu_pc = pc; addiu_n = 0; have_addiu = 1;
                        }
                        continue;
                    }
                    if (ffn == 0x21u && frs != 0u && frt != 0u &&
                        ((int)frs == cur || (int)frt == cur)) {       /* addu sign correction */
                        if (have_move || have_addiu) break;
                        cur = (int)frd; continue;
                    }
                }
                if (fop == 0u && ffn == 0x23u &&
                    ((int)frs == cur || (int)frt == cur)) {           /* subu sign correction */
                    if (have_move || have_addiu) break;
                    cur = (int)frd; continue;
                }
                if ((fop == 0x09u || fop == 0x08u) &&                 /* addiu/addi rD, Q, N */
                    (int)frs == cur && (!have_move || (int)frt != move_dest)) {
                    if (!have_move) {
                        if ((int)frt == cur) break; /* quotient bias, not a bound */
                        move_pc = pc; move_dest = (int)frt; move_n = fsimm; have_move = 1;
                    } else if (!have_addiu) {
                        addiu_pc = pc; addiu_n = fsimm; have_addiu = 1;
                    }
                    continue;
                }
                /* An unrecognized overwrite invalidates the tracked value. */
                if ((fop == 0u && ((int)frd == cur || (int)frd == move_dest)) ||
                    ((fop >= 0x08u && fop <= 0x0Fu) &&
                     ((int)frt == cur || (int)frt == move_dest))) break;
            }
            if (!have_move || !have_addiu || addiu_n == move_n) continue;

            /* Role by relative offset, not the absolute table-segment bias. */
            int delta = addiu_n - move_n;
            int move_kind  = (delta > 0) ? WS_BD_START : WS_BD_END;
            int addiu_kind = (delta > 0) ? WS_BD_END   : WS_BD_START;
            int wcols      = delta < 0 ? -delta : delta;

            for (int s = 0; s < 2; s++) {
                uint32_t spc = s ? addiu_pc : move_pc;
                int      sk  = s ? addiu_kind : move_kind;
                int dup = 0;
                for (int q = 0; q < count; q++) if (out[q].pc == spc) { dup = 1; break; }
                if (!dup && count < max_sites) {
                    out[count].pc = spc; out[count].kind = sk;
                    out[count].window_cols = wcols; count++;
                }
            }
            break;   /* this mult handled; do not try the other operand assignment */
        }
    }
    return count;
}

/* Convenience: return the rewrite kind for a single guest PC by scanning a
 * window of words (the runtime passes a window around the PC), and output the
 * window width in columns via *out_cols. Returns WS_BD_NONE / WS_BD_START /
 * WS_BD_END. */
static inline int psx_ws_backdrop_kind_at(const uint32_t *words, int n,
                                          uint32_t base_pc, uint32_t pc,
                                          int *out_cols)
{
    WsBackdropSite sites[32];
    int ns = psx_ws_find_backdrop_windows(words, n, base_pc, sites, 32);
    for (int i = 0; i < ns; i++)
        if (sites[i].pc == pc) {
            if (out_cols) *out_cols = sites[i].window_cols;
            return sites[i].kind;
        }
    if (out_cols) *out_cols = 0;
    return WS_BD_NONE;
}

#endif /* PSX_WS_BACKDROP_DETECT_H */
