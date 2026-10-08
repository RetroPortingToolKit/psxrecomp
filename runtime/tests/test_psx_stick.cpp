#include "psx_stick.h"

#include <cstdio>

int main() {
    uint8_t x = 0, y = 0;
    psx_stick_to_dualshock(0, 0, 4096, 0, &x, &y);
    if (x != 0x80 || y != 0x80) return 1;

    psx_stick_to_dualshock(4095, 0, 4096, 0, &x, &y);
    if (x != 0x80 || y != 0x80) return 2;

    psx_stick_to_dualshock(-32768, 0, 4096, 0, &x, &y);
    if (x != 0x00 || y != 0x80) return 3;

    psx_stick_to_dualshock(32767, 0, 4096, 0, &x, &y);
    if (x != 0xFF || y != 0x80) return 4;

    psx_stick_to_dualshock(16384, 0, 4096, 0, &x, &y);
    if (x <= 0x80 || x >= 0xFF || y != 0x80) return 5;
    std::printf("stick mapping: center/left/right/partial pass (partial x=%u)\n",
                (unsigned)x);
    return 0;
}
