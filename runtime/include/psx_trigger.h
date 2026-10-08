#pragma once

#include <stdint.h>

/* SDL trigger axes use 0..32767 for released..pressed. Preserve intermediate
 * values and round to the host-plugin byte range without a digital threshold. */
static inline uint8_t psx_trigger_axis_to_u8(int16_t value) {
    if (value <= 0) return 0;
    if (value >= 32767) return 255;
    return (uint8_t)(((int32_t)value * 255 + 16383) / 32767);
}
