/* input_route.c - see input_route.h. */
#include "input_route.h"

#include <string.h>

void input_route_clear(InputRoute *r)
{
    r->count = 0;
    r->index = 0;
    r->remaining = 0;
    r->active = 0;
    r->start_frame = 0;
    r->frame_axes_valid = 0;
}

int input_route_append(InputRoute *r, int frames, int buttons, const int axes[4])
{
    if (r->active) return INPUT_ROUTE_ERR_ACTIVE;
    if (frames <= 0) return INPUT_ROUTE_ERR_FRAMES;
    if (buttons < 0 || buttons > 0xFFFF) return INPUT_ROUTE_ERR_BUTTONS;
    int any = 0;
    if (axes) {
        for (int i = 0; i < 4; i++) {
            if (axes[i] < -1 || axes[i] > 255) return INPUT_ROUTE_ERR_AXIS;
            if (axes[i] >= 0) any = 1;
        }
    }
    if (r->count >= INPUT_ROUTE_MAX_STEPS) return INPUT_ROUTE_ERR_FULL;
    InputRouteStep *s = &r->steps[r->count++];
    s->frames = (uint32_t)frames;
    s->buttons = (uint16_t)buttons;
    s->has_axes = (uint8_t)any;
    for (int i = 0; i < 4; i++)
        s->axes[i] = (uint8_t)(any && axes[i] >= 0 ? axes[i] : 0x80);
    return INPUT_ROUTE_OK;
}

int input_route_start(InputRoute *r, uint64_t at_frame, uint64_t now_frame)
{
    if (r->count == 0) return INPUT_ROUTE_ERR_EMPTY;
    r->index = 0;
    r->remaining = r->steps[0].frames;
    r->start_frame = at_frame > now_frame ? at_frame : 0u;
    r->frame_axes_valid = 0;
    r->active = 1;
    return INPUT_ROUTE_OK;
}

void input_route_stop(InputRoute *r)
{
    r->active = 0;
    r->remaining = 0;
    r->frame_axes_valid = 0;
}

int input_route_next(InputRoute *r, uint64_t now_frame, int *buttons)
{
    if (!(r->active && r->index < r->count && now_frame >= r->start_frame)) {
        r->frame_axes_valid = 0;
        return 0;
    }
    const InputRouteStep *s = &r->steps[r->index];
    *buttons = (int)s->buttons;
    r->frame_axes_valid = s->has_axes;
    memcpy(r->frame_axes, s->axes, 4);
    if (r->remaining > 0 && --r->remaining == 0) {
        r->index++;
        if (r->index < r->count) r->remaining = r->steps[r->index].frames;
        else r->active = 0;
    }
    return 1;
}

int input_route_frame_axes(const InputRoute *r, uint8_t out[4])
{
    if (!r->frame_axes_valid) return 0;
    memcpy(out, r->frame_axes, 4);
    return 1;
}

const char *input_route_error_text(int code)
{
    switch (code) {
    case INPUT_ROUTE_OK: return "ok";
    case INPUT_ROUTE_ERR_ACTIVE: return "input route is active";
    case INPUT_ROUTE_ERR_FRAMES: return "frames must be positive";
    case INPUT_ROUTE_ERR_BUTTONS: return "buttons must be a 16-bit pad word";
    case INPUT_ROUTE_ERR_AXIS: return "lx/ly/rx/ry must be 0..255 (0x80 = centre)";
    case INPUT_ROUTE_ERR_FULL: return "input route is full";
    case INPUT_ROUTE_ERR_EMPTY: return "input route is empty";
    }
    return "input route error";
}
