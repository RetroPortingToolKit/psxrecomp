/* Line breaking of the host toast (host_osd_wrap.h). */
#include "host_osd_wrap.h"
#include <stdio.h>
#include <stdlib.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char L[HOST_OSD_LINES][HOST_OSD_COLS + 1];
static int cut;
static int wrap(const char *m) { return host_osd_wrap(m, L, &cut); }

int main(void) {
    /* The numbers the toast is sized by: 8 pixels of margin, 2+2 of padding
     * at 2x, 16 pixels a character: 38 columns end at pixel 624 of 640. */
    CHECK(8 + (2 * 2 + HOST_OSD_COLS * 8) * 2 <= 640, "a full line fits the 640-pixel window");
    CHECK(8 + (2 * 2 + (HOST_OSD_COLS + 2) * 8) * 2 > 640, "and two more characters would not");

    CHECK(wrap("") == 0 && wrap(NULL) == 0 && !cut, "empty message: no lines");
    CHECK(wrap("Game saved") == 1 && !strcmp(L[0], "Game saved") && !cut, "a short message is one line");
    char full[HOST_OSD_COLS + 1];
    memset(full, 'x', HOST_OSD_COLS); full[HOST_OSD_COLS] = 0;
    CHECK(wrap(full) == 1 && !strcmp(L[0], full) && !cut, "exactly one line wide stays one line");

    /* "Prefix: reason": the prefix on its own line, the reason below. */
    CHECK(wrap("Save state not loaded: it was made by a different build") == 2 &&
          !strcmp(L[0], "Save state not loaded:") && !strcmp(L[1], "it was made by a different build") && !cut,
          "prefix on its own line, reason below: [%s] [%s]", L[0], L[1]);
    /* A reason too long for one line keeps the prefix line and wraps. */
    CHECK(wrap("Save state not loaded: the file was written by another build of this game") == 3 &&
          !strcmp(L[0], "Save state not loaded:") && !strcmp(L[1], "the file was written by another build") &&
          !strcmp(L[2], "of this game") && !cut, "three lines: [%s] [%s] [%s]", L[0], L[1], L[2]);
    /* A reason that does not fit under its prefix falls back to plain filling. */
    const char *long_reason = "Note: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
                              "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb cccccccccccccccccccccccccccccccccccccc";
    int n = wrap(long_reason);
    CHECK(n == 3 && !cut && !strncmp(L[0], "Note: aaaa", 10) && L[1][0] == 'b' && L[2][0] == 'c',
          "fallback fill keeps the whole text: [%s] [%s] [%s] cut=%d", L[0], L[1], L[2], cut);
    /* No prefix: break at spaces, never inside a word. */
    CHECK(wrap("Save states are off while netplay is connected") == 2 &&
          !strcmp(L[0], "Save states are off while netplay is") && !strcmp(L[1], "connected"),
          "plain fill: [%s] [%s]", L[0], L[1]);
    /* A file name after the prefix. */
    CHECK(wrap("Screenshot saved: screenshot-20261001T111417Z-0001") == 2 &&
          !strcmp(L[0], "Screenshot saved:") && !strcmp(L[1], "screenshot-20261001T111417Z-0001") && !cut,
          "saved name: [%s] [%s]", L[0], L[1]);
    /* A word longer than a line is split across lines, nothing lost. */
    char word[HOST_OSD_COLS * 2 + 6];
    memset(word, 'w', sizeof word - 1); word[sizeof word - 1] = 0;
    n = wrap(word);
    CHECK(n == 3 && (int)(strlen(L[0]) + strlen(L[1]) + strlen(L[2])) == (int)strlen(word) && !cut,
          "long word split over %d lines", n);
    /* Too long: three lines, the last ends in "...", and the caller is told. */
    char huge[400];
    memset(huge, 0, sizeof huge);
    for (int i = 0; i < 60; ++i) strcat(huge, "word ");
    n = wrap(huge);
    CHECK(n == HOST_OSD_LINES && cut && strlen(L[2]) <= HOST_OSD_COLS &&
          !strcmp(L[2] + strlen(L[2]) - 3, "..."), "over-long message is cut with dots: [%s]", L[2]);
    /* Every line always fits. */
    for (int i = 0; i < HOST_OSD_LINES; ++i) CHECK(strlen(L[i]) <= HOST_OSD_COLS, "line %d width", i);
    /* Runs of spaces do not make empty lines or leading blanks. */
    CHECK(wrap("  spaced    out   text that goes past the width of one line   ") == 2 && L[0][0] == 's' && L[1][0] != ' ',
          "spaces collapse: [%s] [%s]", L[0], L[1]);

    if (failures) { fprintf(stderr, "%d of %d checks failed\n", failures, checks); return 1; }
    printf("PASS: host toast wrapping, %d checks\n", checks);
    return 0;
}
