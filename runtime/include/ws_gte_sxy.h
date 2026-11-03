#ifndef PSXRECOMP_WS_GTE_SXY_H
#define PSXRECOMP_WS_GTE_SXY_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which screen positions did the GTE project recently?
 *
 * Under projection-and-stretch widescreen the GTE squashes every projected X
 * about the display centre. A title that builds HUD from GTE-projected
 * geometry (vector digits and lettering run through its model renderer) then
 * writes already-squashed X into its packets, whereas screen-space HUD packets
 * carry authored X. The two need different corrections, and the packet alone
 * does not say which it is. The GTE stamps every SXY it produces here with the
 * vblank it happened in; a packet vertex that matches a recent stamp was
 * projected.
 *
 * "Recent" is WS_GTE_SXY_WINDOW vblanks: games commonly rebuild their packets
 * every few vblanks and resubmit the same ordering table in between (Army Men
 * rebuilds every fourth), so a retained packet is still younger than its
 * projection plus one rebuild period. The cell grid is 1024 x 512, covering
 * the signed 11-bit GP0 vertex range. A hit is evidence, not proof: with ~1000
 * projections a frame the chance that a whole polygon matches by accident is
 * negligible, so require every vertex of a primitive to hit. */
#define WS_GTE_SXY_W 1024u
#define WS_GTE_SXY_H 512u
#define WS_GTE_SXY_CELLS (WS_GTE_SXY_W * WS_GTE_SXY_H)
#define WS_GTE_SXY_WINDOW 8u

typedef struct {
    uint16_t stamp[WS_GTE_SXY_CELLS];   /* 0x8000 | vblank (15 bits); 0 = never */
} WsGteSxy;

static inline uint32_t ws_gte_sxy_cell(int32_t x, int32_t y) {
    return (((uint32_t)(y + 256) & (WS_GTE_SXY_H - 1u)) * WS_GTE_SXY_W) +
           ((uint32_t)(x + 512) & (WS_GTE_SXY_W - 1u));
}

static inline void ws_gte_sxy_note(WsGteSxy *s, uint32_t frame,
                                   int32_t x, int32_t y) {
    s->stamp[ws_gte_sxy_cell(x, y)] = (uint16_t)(0x8000u | (frame & 0x7FFFu));
}

static inline int ws_gte_sxy_recent(const WsGteSxy *s, uint32_t frame,
                                    int32_t x, int32_t y) {
    const uint16_t st = s->stamp[ws_gte_sxy_cell(x, y)];
    if (!st) return 0;
    return ((frame - st) & 0x7FFFu) <= WS_GTE_SXY_WINDOW;
}

#ifdef __cplusplus
}
#endif

#endif
