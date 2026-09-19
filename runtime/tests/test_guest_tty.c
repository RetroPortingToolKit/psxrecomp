#include "guest_tty.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t out[8] = {0};
    uint64_t total = 99;

    psx_guest_tty_reset();
    assert(psx_guest_tty_snapshot(out, sizeof(out), &total) == 0);
    assert(total == 0);

    psx_guest_tty_putchar('A');
    psx_guest_tty_putchar('B');
    psx_guest_tty_putchar(0);
    psx_guest_tty_putchar('\n');
    assert(psx_guest_tty_snapshot(out, sizeof(out), &total) == 4);
    assert(total == 4);
    assert(memcmp(out, "AB\0\n", 4) == 0);

    memset(out, 0, sizeof(out));
    assert(psx_guest_tty_snapshot(out, 2, &total) == 2);
    assert(total == 4);
    assert(out[0] == 0 && out[1] == '\n');

    psx_guest_tty_reset();
    assert(psx_guest_tty_snapshot(out, sizeof(out), &total) == 0);
    assert(total == 0);
    return 0;
}
