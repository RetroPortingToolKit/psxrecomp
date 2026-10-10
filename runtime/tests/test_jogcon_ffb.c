/* psx_jogcon_ffb.h: JogCon motor byte -> wheel constant force / spring. */
#include <stdio.h>
#include "psx_jogcon_ffb.h"

static int s_fail;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", m); ++s_fail; } } while (0)

int main(void) {
    PsxJogconFfb f = psx_jogcon_ffb_map(0, 15, 0);
    CHECK(f.constant == 0 && f.spring == 0, "stop: no force");
    f = psx_jogcon_ffb_map(1, 0, 0);
    CHECK(f.constant == 0 && f.spring == 0, "strength 0: no force");
    f = psx_jogcon_ffb_map(1, 15, 0);
    CHECK(f.constant == 32767 && f.spring == 0, "command 1 full: +constant");
    f = psx_jogcon_ffb_map(2, 15, 0);
    CHECK(f.constant == -32767, "command 2 full: -constant");
    f = psx_jogcon_ffb_map(2, 15, 1);
    CHECK(f.constant == 32767, "invert flips the direction");
    f = psx_jogcon_ffb_map(1, 5, 0);
    CHECK(f.constant == 10922, "strength scales linearly (5/15)");
    {
        int prev = 0;
        for (uint8_t st = 1; st <= 15; ++st) {
            f = psx_jogcon_ffb_map(1, st, 0);
            CHECK(f.constant > prev, "monotonic in strength");
            prev = f.constant;
        }
    }
    f = psx_jogcon_ffb_map(3, 8, 0);
    CHECK(f.constant == 0 && f.spring == 8 * 32767 / 15, "hold: centring spring");
    f = psx_jogcon_ffb_map(0x21, 0x0F, 0);
    CHECK(f.constant == 32767, "only the low nibbles count");
    if (s_fail) return 1;
    puts("JogCon force feedback tests passed");
    return 0;
}
