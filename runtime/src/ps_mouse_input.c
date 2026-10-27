#include "ps_mouse_input.h"
#include "sio.h"

/* Bound on owed motion: 30 frames of full-speed travel, the same window the
 * SIO device keeps (Mednafen mouse.cpp). Anything beyond is a stall, not aim. */
#define PS_MOUSE_OWED_MAX (30.0f * 128.0f)

static float s_dx, s_dy;
static uint8_t s_held, s_pressed;
static PsMouseFrame s_frame = { 0, 0, 0xFFFFu };

static float owed_clamp(float v) {
    if (v > PS_MOUSE_OWED_MAX) return PS_MOUSE_OWED_MAX;
    if (v < -PS_MOUSE_OWED_MAX) return -PS_MOUSE_OWED_MAX;
    return v;
}

/* Whole units toward zero, clipped to a signed byte; the remainder stays. */
static int8_t take(float *owed) {
    int v = (int)*owed;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    *owed -= (float)v;
    return (int8_t)v;
}

static uint16_t button_word(uint8_t pressed) {
    uint16_t w = 0xFFFFu;
    if (pressed & PS_MOUSE_LEFT) w &= (uint16_t)~SIO_MOUSE_BTN_LEFT;
    if (pressed & PS_MOUSE_RIGHT) w &= (uint16_t)~SIO_MOUSE_BTN_RIGHT;
    return w;
}

void ps_mouse_input_neutral(PsMouseFrame *out) {
    if (!out) return;
    out->dx = out->dy = 0;
    out->buttons = 0xFFFFu;
}

void ps_mouse_input_reset(void) {
    s_dx = s_dy = 0.0f;
    s_held = s_pressed = 0;
    ps_mouse_input_neutral(&s_frame);
}

void ps_mouse_input_motion(float dx, float dy) {
    /* NaN fails both comparisons in owed_clamp; drop it here instead. */
    if (dx != dx || dy != dy) return;
    s_dx = owed_clamp(s_dx + dx);
    s_dy = owed_clamp(s_dy + dy);
}

void ps_mouse_input_button(int button, int down) {
    const uint8_t bit = (uint8_t)(button & (PS_MOUSE_LEFT | PS_MOUSE_RIGHT));
    if (down) {
        s_held |= bit;
        s_pressed |= bit;
    } else {
        s_held &= (uint8_t)~bit;
    }
}

void ps_mouse_input_latch(void) {
    s_frame.dx = take(&s_dx);
    s_frame.dy = take(&s_dy);
    s_frame.buttons = button_word((uint8_t)(s_held | s_pressed));
    s_pressed = 0;
}

void ps_mouse_input_frame(PsMouseFrame *out) {
    if (out) *out = s_frame;
}
