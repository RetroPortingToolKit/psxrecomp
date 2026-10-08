/* Finite 2D backdrop verdicts (ws_backdrop_extent.h). Rect layouts are the
 * measured GP0 streams: Tomba's overhead village (beads-eio.4.28), the dwarf
 * house single-screen image, and the Watch Tower field's sky/flower grid. */
#include "ws_backdrop_extent.h"
#undef NDEBUG /* the Release test target must execute its assertions */
#include <assert.h>
#include <stdio.h>

static WsBackdropExtent s;
static uint32_t frame = 1000;

typedef struct { int x0, y0, x1, y1; } R;

/* One frame: leading rects, then (optionally) the first polygon. */
static void submit(const R* r, int n, int R_px, int close) {
    ++frame;
    ws_bdx_frame(&s, frame, 320, 224, R_px);
    for (int i = 0; i < n; ++i) ws_bdx_rect(&s, r[i].x0, r[i].y0, r[i].x1, r[i].y1);
    if (close) ws_bdx_close(&s);
}
static int veto(void) { return ws_bdx_veto(&s, frame, 6); }

/* SCUS-94236 village, camera at its left stop: 256+128 px image at x=-2. */
static const R village[] = {{-2, -29, 254, 227}, {254, -29, 382, 227}};
/* Same image scrolled to the other stop. */
static const R village_right[] = {{-64, -29, 192, 227}, {192, -29, 320, 227}};
/* Dwarf house geometry: 256x224 + a 64-wide column of 64x32 tiles. In game
 * these rects are sprite-tagged (neutral); kept as a canonical-width case. */
static const R dwarf[] = {{256, 128, 320, 256}, {256, 96, 320, 128},
    {256, 64, 320, 96}, {256, 32, 320, 64}, {256, 0, 320, 32}, {0, 0, 256, 224}};

int main(void) {
    ws_bdx_reset(&s);

    /* Fit window at 600 px: reveal 140 per side. First short frame only
     * stamps; the second consecutive one arms the veto. */
    submit(village, 2, 140, 1);
    assert(s.eval_full && s.eval_short && s.canon_pct == 100);
    assert(s.left_pct == 0 && s.right_pct > 0 && s.right_pct < 100);
    assert(!veto());
    submit(village, 2, 140, 1);
    assert(veto());

    /* 16:9 (reveal 53): right edge is covered to 382, left void -> short. */
    ws_bdx_reset(&s);
    submit(village, 2, 53, 1); submit(village, 2, 53, 1);
    assert(s.right_pct == 100 && s.left_pct < 100 && veto());
    /* Other camera stop: left covered, right void -> still short. */
    ws_bdx_reset(&s);
    submit(village_right, 2, 53, 1); submit(village_right, 2, 53, 1);
    assert(s.left_pct == 100 && s.right_pct == 0 && veto());
    /* A 2-px gap on one side is still a visible void column. */
    ws_bdx_reset(&s);
    { const R g[] = {{-50, 0, 373, 224}};
      submit(g, 1, 53, 1); submit(g, 1, 53, 1); assert(veto()); }

    /* An image that spans the whole reveal stays wide. */
    ws_bdx_reset(&s);
    { const R wide[] = {{-200, -29, 200, 227}, {200, -29, 520, 227}};
      submit(wide, 2, 140, 1); submit(wide, 2, 140, 1);
      assert(s.eval_full && !s.eval_short && !veto()); }

    /* Dwarf house: exactly canonical, any reveal is void. A frame whose run
     * never closes (no polygon) is finalized by the next frame. */
    ws_bdx_reset(&s);
    submit(dwarf, 6, 8, 0); submit(dwarf, 6, 8, 0);
    ++frame; ws_bdx_frame(&s, frame, 320, 224, 8);
    assert(s.eval_full && s.eval_short && veto());

    /* Watch Tower field grid: 6x7 tiles spanning x -45..339 but only y 40..225
     * -> not a full-screen image, no verdict, never a veto. */
    ws_bdx_reset(&s);
    {
        R grid[42]; int n = 0;
        static const int ys[8] = {40, 72, 88, 102, 137, 177, 185, 225};
        for (int c = 0; c < 6; ++c)
            for (int r = 0; r < 7; ++r)
                grid[n++] = (R){-45 + 64 * c, ys[r], 19 + 64 * c, ys[r + 1]};
        for (int i = 0; i < 10; ++i) submit(grid, n, 140, 1);
        assert(!s.eval_full && !s.eval_short && !veto() && s.short_frames == 0);
    }

    /* Rects after the first polygon are not the back layer (dialog boxes,
     * fades, sprites): they never contribute. */
    ws_bdx_reset(&s);
    for (int i = 0; i < 4; ++i) {
        ++frame; ws_bdx_frame(&s, frame, 320, 224, 140);
        ws_bdx_close(&s);
        ws_bdx_rect(&s, -2, -29, 382, 227);
    }
    ++frame; ws_bdx_frame(&s, frame, 320, 224, 140);
    assert(s.evaluations == 0 && !veto());
    /* Small leading UI rects are not a full-screen backdrop. */
    { const R ui[] = {{61, 16, 301, 55}};
      submit(ui, 1, 140, 1); submit(ui, 1, 140, 1);
      assert(!s.eval_full && !veto()); }

    /* Letterboxed image: only the rows the image occupies need covering. */
    ws_bdx_reset(&s);
    { const R lb[] = {{-140, 0, 460, 210}};
      submit(lb, 1, 140, 1); submit(lb, 1, 140, 1);
      assert(s.eval_full && !s.eval_short && !veto()); }

    /* Veto lapses after the grace period once short frames stop (leaving the
     * village for a field scene that draws no such image). */
    ws_bdx_reset(&s);
    submit(village, 2, 140, 1); submit(village, 2, 140, 1);
    assert(veto());
    frame += 6; assert(veto());
    frame += 1; assert(!veto());
    /* An isolated short frame after the quiet spell does not re-arm it. */
    submit(village, 2, 140, 1); assert(!veto());
    submit(village, 2, 140, 1); assert(veto());
    /* Short frames further apart than the sustain gap never arm it. */
    ws_bdx_reset(&s);
    submit(village, 2, 140, 1); frame += 7; submit(village, 2, 140, 1);
    assert(!veto());
    /* Frame rewind (savestate/rewind) forgets every verdict. */
    ws_bdx_reset(&s);
    submit(village, 2, 140, 1); submit(village, 2, 140, 1); assert(veto());
    frame -= 100; ws_bdx_frame(&s, frame, 320, 224, 140);
    assert(!veto() && s.evaluations == 0);

    /* Reveal wider than the analysable grid: the union extent must still
     * reach the true edges. */
    ws_bdx_reset(&s);
    submit(village, 2, 5000, 1); submit(village, 2, 5000, 1);
    assert(s.eval_full && s.eval_short && veto());
    ws_bdx_reset(&s);
    { const R huge[] = {{-5100, 0, 5500, 224}};
      submit(huge, 1, 5000, 1); submit(huge, 1, 5000, 1);
      assert(s.eval_full && !s.eval_short && !veto()); }

    /* Native 4:3 configuration (no reveal) never evaluates. */
    ws_bdx_reset(&s);
    submit(village, 2, 0, 1); submit(village, 2, 0, 1);
    assert(s.evaluations == 0 && !veto());

    puts("PASS finite backdrop: village/dwarf short, field grid and wide image "
         "untouched, leading-run scope, sustain, grace, rewind, wide grid");
    return 0;
}
