// The game window opens at a normal size, never maximised: with no explicit
// width it is a whole multiple of 240 lines that fits in two thirds of the
// usable height, and an explicit width is honoured, clamped to the display.

#include "window_size.h"

#include <cstdio>

static int g_failures = 0;

static void expect(const char* what, int num, int den, int req_w,
                   int have, int bw, int bh, int want_w, int want_h) {
    int w = req_w, h = 0;
    psx_window_size(&w, &h, num, den, have, bw, bh);
    if (w != want_w || h != want_h) {
        std::printf("FAIL %s: got %dx%d want %dx%d\n", what, w, h, want_w, want_h);
        ++g_failures;
    }
}

int main() {
    // Default size by usable work area (4:3), and never taller than 2/3.
    expect("1080p default", 4, 3, 0, 1, 1920, 1032, 640, 480);
    expect("1440p default", 4, 3, 0, 1, 2560, 1392, 960, 720);
    expect("4K default",    4, 3, 0, 1, 3840, 2112, 1600, 1200);
    expect("8K default",    4, 3, 0, 1, 7680, 4272, 3520, 2640);
    expect("768p default",  4, 3, 0, 1, 1366, 728, 640, 480);    // floor of 2x
    expect("no bounds",     4, 3, 0, 0, 0, 0, 640, 480);

    // Wide aspects keep the same height, wider client.
    expect("1440p 16:9",   16, 9, 0, 1, 2560, 1392, 1280, 720);
    expect("1440p 21:9",   21, 9, 0, 1, 2560, 1392, 1680, 720);

    // An explicit width wins, clamped to the usable area.
    expect("explicit 1280",  4, 3, 1280, 1, 2560, 1392, 1280, 960);
    expect("explicit big",   4, 3, 4000, 1, 1920, 1032, 1376, 1032);
    expect("explicit small", 4, 3, 320,  1, 1920, 1032, 640, 480);

    // Every default fits with a third of the height left for decorations.
    for (int bh = 600; bh <= 4320; bh += 7) {
        int w = 0, h = 0;
        psx_window_size(&w, &h, 4, 3, 1, bh * 2, bh);
        if (h > bh * 2 / 3 && h > 480) {
            std::printf("FAIL usable h=%d: default h=%d exceeds 2/3\n", bh, h);
            ++g_failures;
        }
        if (h % 240 != 0) {
            std::printf("FAIL usable h=%d: default h=%d not a multiple of 240\n", bh, h);
            ++g_failures;
        }
    }

    if (g_failures) {
        std::printf("window_size_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("window_size_test: OK\n");
    return 0;
}
