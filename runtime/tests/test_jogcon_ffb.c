/* psx_jogcon_ffb.h: JogCon motor byte -> wheel constant force / spring. */
#include <stdio.h>
#include "psx_jogcon_ffb.h"

static int s_fail;
static int s_plays, s_stops;
static uint32_t s_ms;
static PsxJogconFfb s_last;
static void rec_play(void *c, const PsxJogconFfb *f, uint32_t ms) {
    (void)c; s_plays++; s_ms = ms; s_last = *f;
}
static void rec_stop(void *c) { (void)c; s_stops++; }
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
    {   /* player setting off (the default): R4's hold commands send nothing */
        PsxJogconFfbBackend b = { NULL, rec_play, rec_stop };
        PsxJogconFfbDriver d = {{0, 0}, 0};
        for (uint8_t st = 1; st <= 15; ++st) psx_jogcon_ffb_tick(&d, &b, 0, 3, st, 0);
        CHECK(s_plays == 0 && s_stops == 0 && !d.active,
              "setting off: no haptic calls at all");
        psx_jogcon_ffb_tick(&d, &b, 1, 3, 4, 0);
        CHECK(s_plays == 1 && d.active, "toggled on: the hold drives the wheel");
        psx_jogcon_ffb_tick(&d, &b, 0, 3, 4, 0);
        CHECK(s_stops == 1 && !d.active, "toggled off live: neutral now");
        s_plays = s_stops = 0;
    }
    {   /* driver */
        PsxJogconFfbBackend b = { NULL, rec_play, rec_stop };
        PsxJogconFfbDriver d = {{0, 0}, 0};
        psx_jogcon_ffb_tick(&d, &b, 1, 0, 0, 0);
        CHECK(s_plays == 0 && s_stops == 0, "idle: nothing sent");
        psx_jogcon_ffb_tick(&d, &b, 1, 3, 9, 0);
        CHECK(s_plays == 1 && s_last.spring == 9 * 32767 / 15 && d.active,
              "hold plays a spring");
        CHECK(s_ms == PSX_JOGCON_FFB_LIFETIME_MS && s_ms > 0 && s_ms < 1000,
              "every force has a short finite lifetime (stall -> slack)");
        psx_jogcon_ffb_tick(&d, &b, 1, 3, 9, 0);
        CHECK(s_plays == 2, "renewed every VBlank");
        psx_jogcon_ffb_tick(&d, &b, 1, 0, 0, 0);
        CHECK(s_stops == 1 && !d.active, "guest stop -> backend stop once");
        psx_jogcon_ffb_tick(&d, &b, 1, 0, 0, 0);
        CHECK(s_stops == 1, "no repeated stops while idle");
        /* pause / disconnect / session end */
        psx_jogcon_ffb_tick(&d, &b, 1, 1, 15, 0);
        psx_jogcon_ffb_neutral(&d, &b);
        CHECK(s_stops == 2 && !d.active && !d.last.constant, "neutral stops a live force");
        psx_jogcon_ffb_neutral(&d, &b);
        CHECK(s_stops == 2, "neutral is idempotent");
        /* opt-in off or JogCon no longer presented */
        psx_jogcon_ffb_tick(&d, &b, 1, 2, 15, 0);
        psx_jogcon_ffb_tick(&d, &b, 0, 2, 15, 0);
        CHECK(s_stops == 3 && !d.active, "disabled mid-force -> stop");
        {   /* null backend callbacks are safe */
            PsxJogconFfbBackend n = { NULL, NULL, NULL };
            psx_jogcon_ffb_tick(&d, &n, 1, 1, 15, 0);
            psx_jogcon_ffb_neutral(&d, &n);
        }
    }
    if (s_fail) return 1;
    puts("JogCon force feedback tests passed");
    return 0;
}
