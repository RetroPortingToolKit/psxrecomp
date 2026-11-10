/* guncon_map.h: host pointer to GunCon X/Y (PS1B-305). The host-mapping
 * vectors come from recomp-corpus references/ps1/PERIPHERAL-GUNCON-SPEC.md. */
#include "guncon_map.h"

#include <stdio.h>

static int fails;

static void expect_xy(double fx, double fy, uint32_t x1, uint32_t x2, uint32_t y1,
                      uint32_t y2, int pal, int want_hit, unsigned want_x,
                      unsigned want_y, const char *what) {
    uint16_t x = 0, y = 0;
    const int hit = guncon_map_pointer(fx, fy, x1, x2, y1, y2, pal, &x, &y);
    if (hit == want_hit && x == want_x && y == want_y) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s: got hit=%d X=%04X Y=%04X, want hit=%d X=%04X Y=%04X\n",
            what, hit, x, y, want_hit, want_x, want_y);
    ++fails;
}

static void expect_box(int w, int h, int num, int den, int x, int y, int bw, int bh,
                       const char *what) {
    int rx, ry, rw, rh;
    guncon_letterbox(w, h, num, den, &rx, &ry, &rw, &rh);
    if (rx == x && ry == y && rw == bw && rh == bh) { printf("ok: %s\n", what); return; }
    fprintf(stderr, "FAIL: %s: got %d,%d %dx%d, want %d,%d %dx%d\n",
            what, rx, ry, rw, rh, x, y, bw, bh);
    ++fails;
}

int main(void) {
    /* Spec host-mapping vectors: NTSC, X1=600 X2=3160, Y1=16 Y2=256. */
    expect_xy(0.0, 0.0, 600, 3160, 16, 256, 0, 1, 0x004E, 0x0010, "NTSC left edge, top");
    expect_xy(0.5, 0.5, 600, 3160, 16, 256, 0, 1, 0x010D, 0x0088, "NTSC centre");
    expect_xy(1.0, 1.0, 600, 3160, 16, 256, 0, 1, 0x01CC, 0x00FF, "NTSC right edge, bottom row");
    /* Inside the gun's documented range at the standard NTSC edges. */
    {
        uint16_t x, y;
        guncon_map_pointer(0.0, 0.0, 600, 3160, 16, 256, 0, &x, &y);
        const int lo_ok = x >= 0x004D;
        guncon_map_pointer(1.0, 1.0, 600, 3160, 16, 256, 0, &x, &y);
        const int hi_ok = x <= 0x01CD;
        if (lo_ok && hi_ok) printf("ok: standard NTSC range stays inside 004Dh..01CDh\n");
        else { fprintf(stderr, "FAIL: standard NTSC range leaves 004Dh..01CDh\n"); ++fails; }
    }

    /* Outside the image: no light. */
    expect_xy(-0.01, 0.5, 600, 3160, 16, 256, 0, 0, 0x0001, 0x000A, "left of the image");
    expect_xy(0.5, 1.01, 600, 3160, 16, 256, 0, 0, 0x0001, 0x000A, "below the image");

    /* An unset horizontal range falls back to the standard fullscreen one. */
    expect_xy(0.0, 0.0, 0, 0, 16, 256, 0, 1, 0x004E, 0x0010, "unset X range: NTSC standard");
    /* An unset vertical range falls back to the standard active lines. */
    expect_xy(0.5, 0.5, 600, 3160, 0, 0, 0, 1, 0x010D, 0x0088, "unset Y range: NTSC standard");

    /* PAL: X on the PAL video clock; Y rows from Y1 over the shown lines. */
    expect_xy(0.0, 0.0, 624, 3184, 0x23, 0x123, 1, 1, 0x0053, 0x0023, "PAL left edge, top");
    expect_xy(0.5, 0.5, 624, 3184, 0x23, 0x123, 1, 1, 0x0113, 0x00A3, "PAL centre");

    /* A range above the active region: the shown lines start at Y1 and the
     * row count is the clamped span (display_scanout.h). */
    expect_xy(0.0, 0.0, 600, 3160, 8, 256, 0, 1, 0x004E, 0x0008, "Y1 above the active region");
    expect_xy(1.0, 1.0, 600, 3160, 8, 256, 0, 1, 0x01CC, 0x00F7, "its last row is Y1 + span - 1");

    /* The letterbox matches the renderer's: largest num:den box, centred. */
    expect_box(1280, 720, 4, 3, 160, 0, 960, 720, "4:3 in 1280x720 is pillarboxed");
    expect_box(800, 900, 4, 3, 0, 150, 800, 600, "4:3 in 800x900 is letterboxed");
    expect_box(1920, 1080, 16, 9, 0, 0, 1920, 1080, "16:9 fills 1920x1080");
    expect_box(640, 480, 0, 0, 0, 0, 640, 480, "a zero aspect falls back to 4:3");

    /* Button bindings are active-low and independent of coordinate mapping. */
    for (int mask = 0; mask < 32; ++mask) {
        int no_light = -1;
        const int trigger = !!(mask & 1), a = !!(mask & 2), b = !!(mask & 4);
        const int dark = !!(mask & 8), offscreen = !!(mask & 16);
        const uint16_t word = guncon_host_buttons(trigger, a, b, dark, offscreen, &no_light);
        const uint16_t expected = (uint16_t)(0xFFFFu &
            ~((trigger || offscreen ? 0x2000u : 0) | (a ? 8u : 0) | (b ? 0x4000u : 0)));
        if (word != expected || no_light != (dark || offscreen)) {
            fprintf(stderr, "FAIL GunCon binding mask %d\n", mask); ++fails;
        }
    }
    if (fails) { fprintf(stderr, "guncon_map_test: %d failure(s)\n", fails); return 1; }
    printf("guncon_map_test: all checks passed\n");
    return 0;
}
