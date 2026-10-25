#include "local_multiplayer_layout.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static void rect(unsigned views, unsigned seat, int w, int h,
                 int x, int y, int rw, int rh) {
    LocalMultiplayerRect r = local_multiplayer_rect(views, seat, w, h);
    assert(r.x == x && r.y == y && r.w == rw && r.h == rh);
}
int main(void) {
    rect(3, 0, 960, 720, 0, 360, 480, 360);
    rect(3, 1, 960, 720, 480, 360, 480, 360);
    rect(3, 2, 960, 720, 240, 0, 480, 360);
    /* Integer rounding leaves both lower pillars, with at most one pixel
     * difference, even when the resized canvas dimensions are odd. */
    rect(3, 0, 641, 481, 0, 240, 320, 241);
    rect(3, 1, 641, 481, 320, 240, 321, 241);
    rect(3, 2, 641, 481, 160, 0, 320, 240);
    rect(4, 0, 960, 720, 0, 360, 480, 360);
    rect(4, 1, 960, 720, 480, 360, 480, 360);
    rect(4, 2, 960, 720, 0, 0, 480, 360);
    rect(4, 3, 960, 720, 480, 0, 480, 360);
    rect(2, 0, 960, 360, 0, 0, 480, 360);
    rect(2, 1, 960, 360, 480, 0, 480, 360);
    puts("ALL PASS");
    return 0;
}
