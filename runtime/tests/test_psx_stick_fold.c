/* psx_stick_fold.h: the default map's digital left-stick fold onto the D-pad. */
#include "psx_stick_fold.h"
#include <stdio.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

enum { UP = 1 << 4, RIGHT = 1 << 5, DOWN = 1 << 6, LEFT = 1 << 7, CROSS = 1 << 14 };
static const int dz = 3277;  /* main.cpp kDefaultDeadzoneRaw */

int main(void) {
    /* Centred stick: nothing pressed, other buttons untouched. */
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0x80, 0x80, dz) == 0xFFFF);
    CHECK(psx_fold_left_stick_to_dpad((uint16_t)~CROSS, 0x80, 0x80, dz) == (uint16_t)~CROSS);
    /* Full deflections press exactly one direction each. */
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0xFF, 0x80, dz) == (uint16_t)~RIGHT);
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0x00, 0x80, dz) == (uint16_t)~LEFT);
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0x80, 0x00, dz) == (uint16_t)~UP);
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0x80, 0xFF, dz) == (uint16_t)~DOWN);
    /* Diagonal presses both axes. */
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0xFF, 0x00, dz) == (uint16_t)~(RIGHT | UP));
    /* Per-axis deadzone: a strong right push with small Y drift is Right only. */
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0xFF, 0x80 + 12, dz) == (uint16_t)~RIGHT);
    /* Threshold: 13 steps (3328) passes 3277, 12 steps (3072) does not. */
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0x80 + 13, 0x80, dz) == (uint16_t)~RIGHT);
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0x80 + 12, 0x80, dz) == 0xFFFF);
    CHECK(psx_fold_left_stick_to_dpad(0xFFFF, 0x80 - 13, 0x80, dz) == (uint16_t)~LEFT);
    /* An already-held D-pad bit stays held. */
    CHECK(psx_fold_left_stick_to_dpad((uint16_t)~UP, 0xFF, 0x80, dz) == (uint16_t)~(UP | RIGHT));
    if (failures) return 1;
    puts("PASS psx_stick_fold");
    return 0;
}
