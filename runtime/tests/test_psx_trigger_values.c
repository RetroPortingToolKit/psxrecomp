/* SDL trigger axis -> 0..255 host-extras magnitude (no digital threshold). */
#include <stdio.h>
#include "psx_trigger.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

int main(void) {
    CHECK(psx_trigger_axis_to_u8(-32768) == 0);
    CHECK(psx_trigger_axis_to_u8(0) == 0);
    CHECK(psx_trigger_axis_to_u8(8192) == 64);
    CHECK(psx_trigger_axis_to_u8(16384) == 128);
    CHECK(psx_trigger_axis_to_u8(24575) == 191);
    CHECK(psx_trigger_axis_to_u8(32767) == 255);
    return 0;
}
