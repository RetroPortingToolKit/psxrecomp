#include "ws_gte_sxy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    WsGteSxy *s = (WsGteSxy *)calloc(1, sizeof *s);
    assert(s);

    /* Nothing recorded yet, including at the zero cell. */
    assert(!ws_gte_sxy_recent(s, 10u, 100, 50));
    assert(!ws_gte_sxy_recent(s, 0u, -512, -256));

    ws_gte_sxy_note(s, 10u, 397, 58);
    ws_gte_sxy_note(s, 10u, -20, -8);
    assert(ws_gte_sxy_recent(s, 10u, 397, 58));
    assert(ws_gte_sxy_recent(s, 10u, -20, -8));
    assert(!ws_gte_sxy_recent(s, 10u, 398, 58));
    assert(!ws_gte_sxy_recent(s, 10u, 397, 59));

    /* A packet built in one vblank and resubmitted for the next several
     * (the ordering table is rebuilt every fourth vblank) still matches. */
    for (uint32_t f = 10u; f <= 10u + WS_GTE_SXY_WINDOW; ++f)
        assert(ws_gte_sxy_recent(s, f, 397, 58));
    assert(!ws_gte_sxy_recent(s, 10u + WS_GTE_SXY_WINDOW + 1u, 397, 58));

    /* A fresh projection of the same position refreshes it. */
    ws_gte_sxy_note(s, 16u, 397, 58);
    assert(ws_gte_sxy_recent(s, 20u, 397, 58));

    /* A reader behind the stamp (render thread lag) does not match. */
    assert(!ws_gte_sxy_recent(s, 3u, 397, 58));

    /* The 15-bit stamp wraps without turning ancient entries into hits. */
    ws_gte_sxy_note(s, 32767u, 1, 1);
    assert(ws_gte_sxy_recent(s, 32767u, 1, 1));
    assert(ws_gte_sxy_recent(s, 32768u + 2u, 1, 1));
    assert(!ws_gte_sxy_recent(s, 32768u + 100u, 1, 1));

    free(s);
    puts("ws_gte_sxy_test: PASS");
    return 0;
}
