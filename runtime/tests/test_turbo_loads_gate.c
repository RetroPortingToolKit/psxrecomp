/* The Fast Loading gate against drive-state patterns recorded from a
 * Resident Evil 3 PAL playback. One character
 * per vblank, run-length encoded:
 *   D  data read (reading, XA-ADPCM off, no XA stream, no CD-DA)
 *   h  load hold (the drive's load-in-progress gap tail), XA mode off
 *   x  load hold with XA mode on (streamed audio)
 *   .  idle
 * Only these flags are kept: no disc data or sector addresses. */
#include "turbo_loads_gate.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* Door transitions (must engage and hold to the end of the load). */
static const char *DOOR_A =
    "6. 2x 2h 2D 3h 9D 3h 4D 3h 14D 3h 3D 3h 14D 3h 2D 19h 7D 2h 66D 1h 6D 2h 15D 2h 10D 4h "
    "45D 2h 2D 2h 10D 4h 27D 4h 9D 3h 2D 1h 6D 3h 7D 29h 14.";
static const char *DOOR_B = "4. 2D 4h 33D 2h 38D 1h 16D 4h 16D 4h 24D 6h 11D 26h 3x 16.";
/* Reads while the player has control, and an XA stream tail (must not engage). */
static const char *PLAY_A = "5. 11D 7h 11D 30h 16.";
static const char *PLAY_B = "4. 11D 29h 11.";
static const char *XA_TAIL = "9. 28x 13.";

typedef struct { int first; int turbo; int len; int engaged_at_last_read; } Result;

static Result run(const char *rle) {
    TurboLoadsGate g;
    Result r = { -1, 0, 0, 0 };
    int last_read = -1, i = 0;
    char seq[4096];
    size_t n = 0;
    for (const char *p = rle; *p;) {
        if (isspace((unsigned char)*p)) { ++p; continue; }
        int count = (int)strtol(p, (char **)&p, 10);
        for (int k = 0; k < count && n < sizeof seq; ++k) seq[n++] = *p;
        ++p;
    }
    turbo_loads_gate_reset(&g);
    int engaged_last = 0;
    for (i = 0; i < (int)n; ++i) {
        const int on = turbo_loads_gate_step(&g, seq[i] == 'D', seq[i] == 'h', 6);
        if (on && r.first < 0) r.first = i;
        r.turbo += on;
        if (seq[i] == 'D') { last_read = i; engaged_last = on; }
    }
    r.len = (int)n;
    r.engaged_at_last_read = last_read >= 0 && engaged_last;
    return r;
}

int main(void) {
    Result r;
    r = run(DOOR_A);
    CHECK(r.first >= 0 && r.first <= 70 && r.engaged_at_last_read,
          "door A engages within 70 vblanks and holds to its last read (first %d)", r.first);
    r = run(DOOR_B);
    CHECK(r.first >= 0 && r.first <= 55 && r.engaged_at_last_read,
          "door B engages within 55 vblanks and holds to its last read (first %d)", r.first);
    r = run(PLAY_A);
    CHECK(r.turbo == 0, "gameplay read A never engages (%d turbo vblanks)", r.turbo);
    r = run(PLAY_B);
    CHECK(r.turbo == 0, "gameplay read B never engages (%d turbo vblanks)", r.turbo);
    r = run(XA_TAIL);
    CHECK(r.turbo == 0, "an XA stream tail never engages (%d turbo vblanks)", r.turbo);

    /* The window threshold: 39 reads in 60 vblanks stay off, 40 engage. */
    {
        TurboLoadsGate g;
        int on = 0;
        turbo_loads_gate_reset(&g);
        for (int i = 0; i < TURBO_LOADS_WINDOW; ++i)
            on = turbo_loads_gate_step(&g, i % 3 != 0 && i < 59, 0, 6);   /* 39 reads */
        CHECK(!on, "39 of 60 data reads do not engage");
        on = turbo_loads_gate_step(&g, 1, 0, 6);   /* the 40th in the window */
        CHECK(on, "40 of the last 60 engage");
        /* Release: 6 vblanks without reads or hold, then off with a clear window. */
        for (int i = 0; i < 6; ++i) on = turbo_loads_gate_step(&g, 0, 0, 6);
        CHECK(on, "still on through the release frames");
        on = turbo_loads_gate_step(&g, 0, 0, 6);
        CHECK(!on && turbo_loads_gate_window_reads(&g) == 0, "released, window cleared");
        on = turbo_loads_gate_step(&g, 1, 0, 6);
        CHECK(!on, "one read after release does not re-engage");
    }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: fast loading gate");
    return 0;
}
