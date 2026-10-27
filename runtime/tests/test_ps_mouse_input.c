/* Host PlayStation Mouse accumulator (ps_mouse_input.c): per-frame latching
 * of summed SDL motion with fractional carry, signed-byte clipping with the
 * rest owed to later frames, the owed-motion bound, click latching between
 * frames, idempotent reads and reset. */

#include "ps_mouse_input.h"
#include "sio.h"
#include <stdio.h>

static int failures;

#define EXPECT(label, expected, actual) do {                                  \
    long e_ = (long)(expected), a_ = (long)(actual);                          \
    if (e_ != a_) {                                                           \
        fprintf(stderr, "FAIL %s: expected=%ld actual=%ld\n", label, e_, a_); \
        failures++;                                                           \
    }                                                                         \
} while (0)

static PsMouseFrame latch(void) {
    PsMouseFrame f;
    ps_mouse_input_latch();
    ps_mouse_input_frame(&f);
    return f;
}

int main(void) {
    PsMouseFrame f, g;

    ps_mouse_input_reset();
    f = latch();
    EXPECT("neutral.dx", 0, f.dx);
    EXPECT("neutral.dy", 0, f.dy);
    EXPECT("neutral.buttons", 0xFFFF, f.buttons);

    /* Several SDL events between two frames sum; the frame consumes them. */
    ps_mouse_input_motion(3.0f, -2.0f);
    ps_mouse_input_motion(4.0f, -1.0f);
    f = latch();
    EXPECT("sum.dx", 7, f.dx);
    EXPECT("sum.dy", -3, f.dy);
    ps_mouse_input_frame(&g);
    EXPECT("read.idempotent", 7, g.dx);
    f = latch();
    EXPECT("consumed.dx", 0, f.dx);
    EXPECT("consumed.dy", 0, f.dy);

    /* Sub-unit motion (high-DPI / scaled SDL3 deltas) is carried, not lost. */
    for (int i = 0; i < 4; i++) ps_mouse_input_motion(0.25f, -0.5f);
    f = latch();
    EXPECT("fraction.dx", 1, f.dx);
    EXPECT("fraction.dy", -2, f.dy);
    ps_mouse_input_motion(0.6f, 0.0f);
    f = latch();
    EXPECT("fraction.partial", 0, f.dx);
    ps_mouse_input_motion(0.6f, 0.0f);
    f = latch();
    EXPECT("fraction.carried", 1, f.dx);
    ps_mouse_input_reset();

    /* A flick beyond a signed byte is spread over frames. */
    ps_mouse_input_motion(300.0f, -300.0f);
    f = latch();
    EXPECT("large.1.dx", 127, f.dx);
    EXPECT("large.1.dy", -128, f.dy);
    f = latch();
    EXPECT("large.2.dx", 127, f.dx);
    EXPECT("large.2.dy", -128, f.dy);
    f = latch();
    EXPECT("large.3.dx", 46, f.dx);
    EXPECT("large.3.dy", -44, f.dy);
    f = latch();
    EXPECT("large.4.dx", 0, f.dx);

    /* Owed motion is bounded (a stall is not aim) and NaN is ignored. */
    ps_mouse_input_motion(1.0e9f, 0.0f);
    ps_mouse_input_motion(0.0f / 0.0f, 5.0f);
    {
        long total = 0;
        int frames = 0;
        do { f = latch(); total += f.dx; } while (f.dx && ++frames < 100);
        EXPECT("bound.total", 30 * 128, total);
    }
    ps_mouse_input_reset();

    /* Buttons: held is reported every frame, a click inside one frame once. */
    ps_mouse_input_button(PS_MOUSE_LEFT, 1);
    f = latch();
    EXPECT("left.held", (uint16_t)~SIO_MOUSE_BTN_LEFT, f.buttons);
    f = latch();
    EXPECT("left.still", (uint16_t)~SIO_MOUSE_BTN_LEFT, f.buttons);
    ps_mouse_input_button(PS_MOUSE_LEFT, 0);
    f = latch();
    EXPECT("left.released", 0xFFFF, f.buttons);
    ps_mouse_input_button(PS_MOUSE_RIGHT, 1);
    ps_mouse_input_button(PS_MOUSE_RIGHT, 0);
    f = latch();
    EXPECT("right.click", (uint16_t)~SIO_MOUSE_BTN_RIGHT, f.buttons);
    f = latch();
    EXPECT("right.click.once", 0xFFFF, f.buttons);
    ps_mouse_input_button(PS_MOUSE_LEFT, 1);
    ps_mouse_input_button(PS_MOUSE_RIGHT, 1);
    f = latch();
    EXPECT("both", (uint16_t)~(SIO_MOUSE_BTN_LEFT | SIO_MOUSE_BTN_RIGHT),
           f.buttons);
    ps_mouse_input_button(0x40, 1); /* other buttons are not mouse buttons */

    /* Reset forgets held buttons, clicks and owed motion. */
    ps_mouse_input_motion(50.0f, 50.0f);
    ps_mouse_input_reset();
    ps_mouse_input_frame(&f);
    EXPECT("reset.frame", 0xFFFF, f.buttons);
    f = latch();
    EXPECT("reset.buttons", 0xFFFF, f.buttons);
    EXPECT("reset.dx", 0, f.dx);

    ps_mouse_input_neutral(&f);
    EXPECT("neutral.fn", 0xFFFF, f.buttons);

    if (failures) {
        fprintf(stderr, "ps_mouse_input_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("ps_mouse_input_test: OK\n");
    return 0;
}
