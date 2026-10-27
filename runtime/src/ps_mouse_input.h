#pragma once
/* Host side of the emulated PlayStation Mouse (SIO_PAD_MOUSE): the PC mouse's
 * relative motion and two buttons, collected from SDL events and handed to the
 * guest once per sampled frame through PsxNetPad -> psx_pad_apply_to_sio.
 *
 * Motion is summed with its fractional part; each latch moves at most one
 * signed byte per axis into the frame and keeps the rest for later frames, so
 * a fast flick is spread over frames instead of being clipped away (the SIO
 * device then spreads it over polls the same way). A button pressed and
 * released between two latches still shows as pressed in the next frame.
 *
 * Pure: no SDL, no SIO. Owned by the SDL/main thread. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsMouseFrame {
    int8_t   dx, dy;   /* +X right, +Y down, PlayStation Mouse units */
    uint16_t buttons;  /* SIO_MOUSE_BTN_* active low, other bits set */
} PsMouseFrame;

enum { PS_MOUSE_LEFT = 0x01, PS_MOUSE_RIGHT = 0x02 };

/* Forget owed motion, latched clicks and held buttons (capture release,
 * reset, state load, device change). */
void ps_mouse_input_reset(void);
void ps_mouse_input_motion(float dx, float dy);
void ps_mouse_input_button(int button, int down);
/* Once per sampled frame: move this frame's share into the latched frame. */
void ps_mouse_input_latch(void);
/* The latched frame; reading does not consume it. */
void ps_mouse_input_frame(PsMouseFrame *out);
/* Neutral frame (no motion, no buttons). */
void ps_mouse_input_neutral(PsMouseFrame *out);

#ifdef __cplusplus
}
#endif
