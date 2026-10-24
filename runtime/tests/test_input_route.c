/* input_route: frame-exact scripted pad route with optional analog axes.
 * Build/run: ctest -R input_route_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_route.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); failures++; } \
} while (0)

int main(void)
{
    InputRoute *r = (InputRoute *)calloc(1, sizeof(InputRoute));
    if (!r) return 2;
    int b = -1;
    uint8_t ax[4];

    /* Validation. */
    input_route_clear(r);
    CHECK(input_route_start(r, 0, 0) == INPUT_ROUTE_ERR_EMPTY, "empty route cannot start");
    CHECK(input_route_append(r, 0, 0xFFFF, NULL) == INPUT_ROUTE_ERR_FRAMES, "frames must be positive");
    CHECK(input_route_append(r, 1, 0x10000, NULL) == INPUT_ROUTE_ERR_BUTTONS, "buttons is 16-bit");
    CHECK(input_route_append(r, 1, -2, NULL) == INPUT_ROUTE_ERR_BUTTONS, "negative buttons rejected");
    {
        int bad[4] = { 256, -1, -1, -1 };
        CHECK(input_route_append(r, 1, 0xFFFF, bad) == INPUT_ROUTE_ERR_AXIS, "axis > 255 rejected");
        int bad2[4] = { -1, -1, -1, -5 };
        CHECK(input_route_append(r, 1, 0xFFFF, bad2) == INPUT_ROUTE_ERR_AXIS, "axis < -1 rejected");
    }
    CHECK(r->count == 0, "rejected steps are not queued");

    /* Backwards compatible: steps without axes (NULL, or all -1). */
    int none[4] = { -1, -1, -1, -1 };
    CHECK(input_route_append(r, 2, 0xFFBF, NULL) == INPUT_ROUTE_OK, "plain step");
    CHECK(input_route_append(r, 1, 0xFFFF, none) == INPUT_ROUTE_OK, "all -1 = no axes");
    CHECK(r->steps[0].has_axes == 0 && r->steps[1].has_axes == 0, "no axes recorded");
    /* Axes step: lx/ly given, rx/ry centred. */
    int stick[4] = { 0, 255, -1, -1 };
    CHECK(input_route_append(r, 3, 0xFFFF, stick) == INPUT_ROUTE_OK, "axes step");
    CHECK(r->steps[2].has_axes == 1, "axes recorded");
    CHECK(r->steps[2].axes[0] == 0 && r->steps[2].axes[1] == 255 &&
          r->steps[2].axes[2] == 0x80 && r->steps[2].axes[3] == 0x80,
          "omitted axes on an axes step are centred");
    int stick2[4] = { 0x80, 0x10, 0xF0, 0x80 };
    CHECK(input_route_append(r, 1, 0xBFFF, stick2) == INPUT_ROUTE_OK, "second axes step");

    /* Not started: no override, no axes. */
    CHECK(input_route_next(r, 10, &b) == 0, "inactive route does not drive");
    CHECK(input_route_frame_axes(r, ax) == 0, "inactive route has no axes");

    /* Delayed start: waits for at_frame. */
    CHECK(input_route_start(r, 100, 10) == INPUT_ROUTE_OK, "start");
    CHECK(input_route_append(r, 1, 0xFFFF, NULL) == INPUT_ROUTE_ERR_ACTIVE, "append while active refused");
    CHECK(input_route_next(r, 99, &b) == 0, "waits for at_frame");

    /* Frame 100..101: step 0 (no axes). */
    CHECK(input_route_next(r, 100, &b) == 1 && b == 0xFFBF, "step0 frame0");
    CHECK(input_route_frame_axes(r, ax) == 0, "step0 has no axes");
    CHECK(input_route_next(r, 101, &b) == 1 && b == 0xFFBF, "step0 frame1");
    /* Frame 102: step 1. */
    CHECK(input_route_next(r, 102, &b) == 1 && b == 0xFFFF, "step1");
    CHECK(input_route_frame_axes(r, ax) == 0, "step1 has no axes");
    /* Frames 103..105: step 2 axes, latched for the SAME poll even after the
     * cursor advanced past the step on its last frame. */
    for (int f = 0; f < 3; f++) {
        CHECK(input_route_next(r, 103u + (uint64_t)f, &b) == 1 && b == 0xFFFF, "step2 buttons");
        memset(ax, 0, sizeof ax);
        CHECK(input_route_frame_axes(r, ax) == 1, "step2 axes live");
        CHECK(ax[0] == 0 && ax[1] == 255 && ax[2] == 0x80 && ax[3] == 0x80, "step2 axes values");
    }
    CHECK(r->index == 3, "cursor advanced to step 3 after step 2's last frame");
    /* Frame 106: step 3 (last) - route ends after it, axes still apply now. */
    CHECK(input_route_next(r, 106, &b) == 1 && b == 0xBFFF, "step3 buttons");
    CHECK(input_route_frame_axes(r, ax) == 1 && ax[1] == 0x10 && ax[2] == 0xF0, "step3 axes");
    CHECK(r->active == 0, "route finished");
    /* Next poll: route no longer drives; axes released. */
    CHECK(input_route_next(r, 107, &b) == 0, "finished route does not drive");
    CHECK(input_route_frame_axes(r, ax) == 0, "axes released after the route");

    /* Restart replays from the top; stop releases immediately. */
    CHECK(input_route_start(r, 0, 200) == INPUT_ROUTE_OK, "restart");
    CHECK(input_route_next(r, 200, &b) == 1 && b == 0xFFBF, "replay from step 0");
    input_route_stop(r);
    CHECK(input_route_next(r, 201, &b) == 0 && input_route_frame_axes(r, ax) == 0, "stop releases");

    /* Capacity. */
    input_route_clear(r);
    for (int i = 0; i < INPUT_ROUTE_MAX_STEPS; i++)
        if (input_route_append(r, 1, 0xFFFF, NULL) != INPUT_ROUTE_OK) { CHECK(0, "fill"); break; }
    CHECK(input_route_append(r, 1, 0xFFFF, NULL) == INPUT_ROUTE_ERR_FULL, "full");
    CHECK(strcmp(input_route_error_text(INPUT_ROUTE_ERR_AXIS),
                 "lx/ly/rx/ry must be 0..255 (0x80 = centre)") == 0, "axis error text");

    free(r);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("input_route_test: ok\n");
    return 0;
}
