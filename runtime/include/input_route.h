/*
 * input_route.h - frame-exact scripted pad route (debug server
 * input_route_* commands). A client queues run-length steps while the route
 * is inactive, then starts it atomically; the pad sampler consumes one step
 * frame per guest pad poll, so a route replays on the same guest frames in
 * every run (deterministic under turbo loads and TCP latency).
 *
 * Each step carries the 16-bit active-low button word and, optionally, the
 * four analog axes (lx, ly, rx, ry: 0..255, 0x80 = centre) that `press`
 * accepts. A step without axes leaves the sticks to the non-route source
 * (no axis override), exactly as before axes existed.
 */
#ifndef PSX_INPUT_ROUTE_H
#define PSX_INPUT_ROUTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_ROUTE_MAX_STEPS 4096

typedef struct {
    uint32_t frames;
    uint16_t buttons;
    uint8_t  has_axes;
    uint8_t  axes[4];       /* lx, ly, rx, ry */
} InputRouteStep;

typedef struct {
    InputRouteStep steps[INPUT_ROUTE_MAX_STEPS];
    uint32_t count;
    uint32_t index;
    uint32_t remaining;
    int      active;
    uint64_t start_frame;   /* waits for this vblank count (0 = now) */
    /* What the most recent input_route_next() handed out, so the axis query
     * that follows the button query in the same pad poll reads the SAME step
     * even after the button query advanced the cursor. */
    int      frame_axes_valid;
    uint8_t  frame_axes[4];
} InputRoute;

enum {
    INPUT_ROUTE_OK = 0,
    INPUT_ROUTE_ERR_ACTIVE,      /* append while running */
    INPUT_ROUTE_ERR_FRAMES,      /* frames <= 0 */
    INPUT_ROUTE_ERR_BUTTONS,     /* not a 16-bit word */
    INPUT_ROUTE_ERR_AXIS,        /* axis outside 0..255 */
    INPUT_ROUTE_ERR_FULL,
    INPUT_ROUTE_ERR_EMPTY        /* start with no steps */
};

void input_route_clear(InputRoute *r);
/* axes: NULL = no axes for this step; else 4 values, each -1 (= centre 0x80
 * when any other axis is given) or 0..255. */
int  input_route_append(InputRoute *r, int frames, int buttons, const int axes[4]);
int  input_route_start(InputRoute *r, uint64_t at_frame, uint64_t now_frame);
void input_route_stop(InputRoute *r);
/* Pad poll: returns 1 and the step's buttons while the route drives the pad
 * (advancing it by one frame), else 0. Also latches the step's axes for
 * input_route_frame_axes(). */
int  input_route_next(InputRoute *r, uint64_t now_frame, int *buttons);
/* Axes of the step the last input_route_next() returned; 0 when that step had
 * none or the route did not drive the last poll. */
int  input_route_frame_axes(const InputRoute *r, uint8_t out[4]);
const char *input_route_error_text(int code);

#ifdef __cplusplus
}
#endif

#endif /* PSX_INPUT_ROUTE_H */
