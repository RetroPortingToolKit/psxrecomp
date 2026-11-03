#include "netplay_fmv_confirm.h"
#undef NDEBUG
#include <assert.h>

/* Model the gate's counting: the peer's digests trail by `lag` ticks. */
static uint32_t streak_after(uint32_t media_end, uint32_t ticks, uint32_t lag)
{
    uint32_t seen = 0, streak = 0, sim;
    for (sim = media_end; sim < media_end + ticks; ++sim) {
        const uint32_t resolved = sim > lag ? sim - lag : 0;
        if (resolved < media_end) continue;           /* nothing past the movie yet */
        streak += rb_fmv_confirm_gain(seen > media_end ? seen : media_end, resolved);
        if (resolved > seen) seen = resolved;
    }
    return streak;
}

int main(void)
{
    assert(rb_fmv_confirm_gain(10, 10) == 0);
    assert(rb_fmv_confirm_gain(10, 9) == 0);
    assert(rb_fmv_confirm_gain(10, 13) == 3);
    /* Same machine (no lag) and cross-machine (2-6 ticks of lag) both reach
     * the 16-tick confirmation well inside the 300-tick cap. */
    assert(streak_after(1000, 300, 0) >= 16);
    assert(streak_after(1000, 300, 2) >= 16);
    assert(streak_after(1000, 300, 6) >= 16);
    /* Bursty confirmations (a frontier jump) still count every tick once. */
    assert(streak_after(1000, 40, 30) == 9);
    return 0;
}
