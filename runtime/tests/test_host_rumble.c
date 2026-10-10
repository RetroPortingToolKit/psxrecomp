/* psx_host_rumble.h: title-supplied rumble merge and expiry. */
#include <stdio.h>
#include "psx_host_rumble.h"

static int s_fail;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", m); ++s_fail; } } while (0)

int main(void) {
    PsxHostRumbleSlot slot = {0, 0, 0, 0};
    uint8_t small = 0, large = 0;
    psx_host_rumble_merge(&slot, 5, &small, &large);
    CHECK(small == 0 && large == 0, "never set: guest motors unchanged");

    psx_host_rumble_set(&slot, 10, 1, 150);
    psx_host_rumble_merge(&slot, 10, &small, &large);
    CHECK(small == 0xFF && large == 150, "set: small on, large 150");

    small = 0; large = 200;
    psx_host_rumble_merge(&slot, 12, &small, &large);
    CHECK(large == 200 && small == 0xFF, "louder guest large motor wins");

    small = 0; large = 0;
    psx_host_rumble_merge(&slot, 10 + PSX_HOST_RUMBLE_TTL - 1, &small, &large);
    CHECK(large == 150, "still live just before the TTL");
    small = 0; large = 0;
    psx_host_rumble_merge(&slot, 10 + PSX_HOST_RUMBLE_TTL, &small, &large);
    CHECK(small == 0 && large == 0, "lapses after the TTL without a refresh");

    psx_host_rumble_set(&slot, 0xFFFFFFFEu, 0, 999);
    small = 0; large = 0;
    psx_host_rumble_merge(&slot, 1u, &small, &large);
    CHECK(large == 255 && small == 0, "clamped to 255; VBlank wrap handled");

    if (s_fail) return 1;
    puts("host rumble tests passed");
    return 0;
}
