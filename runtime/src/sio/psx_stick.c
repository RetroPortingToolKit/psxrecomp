#include "psx_stick.h"

#include <math.h>

static int clamp_axis_setting(int value)
{
    if (value < 0)
        return 0;
    if (value > 32767)
        return 32767;
    return value;
}

static uint8_t signed_axis_to_byte(int value)
{
    int byte = (value + 32768) >> 8;
    if (byte < 0)
        byte = 0;
    if (byte > 255)
        byte = 255;
    return (uint8_t)byte;
}

void psx_stick_to_dualshock(int16_t x, int16_t y,
                            int deadzone, int anti_deadzone,
                            uint8_t *out_x, uint8_t *out_y)
{
    const double raw_x = (double)x;
    const double raw_y = (double)y;
    const double magnitude = sqrt(raw_x * raw_x + raw_y * raw_y);
    double source_magnitude;
    double travel;
    double output_magnitude;
    double scale;
    int scaled_x;
    int scaled_y;

    deadzone = clamp_axis_setting(deadzone);
    anti_deadzone = clamp_axis_setting(anti_deadzone);

    if (deadzone == 32767 ||
        magnitude <= (double)deadzone || magnitude <= 0.0) {
        *out_x = 0x80;
        *out_y = 0x80;
        return;
    }

    source_magnitude = magnitude > 32767.0 ? 32767.0 : magnitude;
    travel = (source_magnitude - (double)deadzone) /
             (32767.0 - (double)deadzone);
    if (travel < 0.0)
        travel = 0.0;
    if (travel > 1.0)
        travel = 1.0;

    output_magnitude = (double)anti_deadzone +
                       travel * (32767.0 - (double)anti_deadzone);
    scale = output_magnitude / magnitude;
    scaled_x = (int)lround(raw_x * scale);
    scaled_y = (int)lround(raw_y * scale);

    if (scaled_x < -32768)
        scaled_x = -32768;
    if (scaled_x > 32767)
        scaled_x = 32767;
    if (scaled_y < -32768)
        scaled_y = -32768;
    if (scaled_y > 32767)
        scaled_y = 32767;

    *out_x = signed_axis_to_byte(scaled_x);
    *out_y = signed_axis_to_byte(scaled_y);
}
