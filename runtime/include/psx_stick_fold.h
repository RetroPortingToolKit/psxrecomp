#pragma once
/* The default controller map's digital stick fold for a pad expressed as PSX
 * stick bytes. main.cpp's default map binds ls_up/down/left/right to
 * lefty-/lefty+/leftx-/leftx+ and folds them onto the D-pad (fold_bit) while
 * the pad presents as DIGITAL, with the same per-axis test as
 * controller_source_pressed_h (v > dz, v < -dz). The debug host input layer
 * (set_input {"layer":"host"}) is a default-mapped gamepad, so its left stick
 * must press the D-pad exactly as a physical controller's would. */
#include <stdint.h>

/* A PSX stick byte (0x00..0xFF, centre 0x80) in SDL axis units. */
static inline int psx_stick_byte_to_axis(uint8_t v) { return ((int)v - 0x80) * 256; }

/* Press (clear, active-low) the D-pad bits the left stick folds onto. */
static inline uint16_t psx_fold_left_stick_to_dpad(uint16_t word, uint8_t lx,
                                                   uint8_t ly, int deadzone_raw) {
    const int x = psx_stick_byte_to_axis(lx), y = psx_stick_byte_to_axis(ly);
    if (y < -deadzone_raw) word &= (uint16_t)~(1u << 4);  /* Up */
    if (x > deadzone_raw)  word &= (uint16_t)~(1u << 5);  /* Right */
    if (y > deadzone_raw)  word &= (uint16_t)~(1u << 6);  /* Down */
    if (x < -deadzone_raw) word &= (uint16_t)~(1u << 7);  /* Left */
    return word;
}
