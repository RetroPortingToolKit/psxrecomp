#ifdef NDEBUG
#undef NDEBUG
#endif

/* Executable regression for the depth24 per-row movie upload record.
 *
 * A 24-bit present reads whatever VRAM holds under the display area. When a
 * movie starts, rows it has not uploaded yet still hold an earlier scene, and
 * a present showed them as a stale border around the movie. gpu.c now keeps
 * one flag per physical VRAM row: the rising edge of MDEC activity clears the
 * flags, each completed CPU-to-VRAM upload in 24-bit mode while MDEC is active
 * sets them, and gpu_depth24_present_row blacks out rows whose flag is clear.
 * With MDEC idle (a 24-bit still image) every row presents as before.
 *
 * Drives the real gpu.c with synthetic VRAM (every byte 0x12, so a decoded
 * pixel is 0xFF121212) and a scripted MDEC activity flag. Checks: a still
 * image stays visible; movie rows not uploaded present black; uploaded rows
 * keep their RGB bytes; a still-to-movie edge drops the old record; uploads
 * wrap at physical row 512; a 24-bit depth change drops the record; the
 * first upload counts even before any present saw MDEC start; a save-state
 * load drops the record whatever the prior activity was; guest VRAM is never
 * changed. The record is presentation state only and is not serialized.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpu.c"

/* The shared stubs report MDEC idle; this test scripts it instead. */
#define mdec_recently_active gpu_exec_stub_mdec_recently_active
#include "gpu_exec_stubs.inc"
#undef mdec_recently_active

static int test_streaming;
int mdec_recently_active(uint32_t within_frames) {
    (void)within_frames;
    return test_streaming;
}

#define GRAY  0xFF121212u
#define BLACK 0xFF000000u

static int failures;
static GpuDisplayInfo di;
static uint32_t out[4];

static void check(uint32_t y, uint32_t want, const char *what) {
    gpu_depth24_present_row(&di, y, out, 4);
    if (out[0] != want || out[3] != want) {
        printf("FAIL: %s: row %u gave %08X, want %08X\n",
               what, (unsigned)y, (unsigned)out[0], (unsigned)want);
        failures++;
    }
}

static void upload(uint32_t y, uint32_t h) {
    vram_write_x = 0; vram_write_y = y; vram_write_w = 6; vram_write_h = h;
    gp0_commit_cpu_to_vram();
}

int main(void) {
    for (unsigned i = 0; i < 1024u * 512u; ++i) vram[i] = 0x1212;
    gp1_display_mode(0x10);
    check(20, GRAY, "24-bit still with MDEC idle stays visible");

    test_streaming = 1;
    check(20, BLACK, "movie row not uploaded this session");
    upload(20, 1);
    check(20, GRAY, "uploaded movie row keeps its RGB bytes");
    check(21, BLACK, "neighbour row is still not uploaded");

    test_streaming = 0;
    check(0, GRAY, "MDEC idle again");
    test_streaming = 1;
    check(20, BLACK, "still-to-movie edge drops the old record");
    upload(20, 1);
    check(0, BLACK, "row 0 not uploaded");
    check(20, GRAY, "steady movie keeps the new upload");

    upload(511, 2);
    check(0, GRAY, "upload wraps at physical row 512");
    check(511, GRAY, "upload covers row 511");

    gp1_display_mode(0);
    gp1_display_mode(0x10);
    check(20, BLACK, "new 24-bit depth session drops the record");

    test_streaming = 0;
    check(20, GRAY, "MDEC idle before the first upload");
    test_streaming = 1;
    upload(20, 1);
    check(20, GRAY, "first upload counts before a present saw MDEC start");

    for (int prior = 0; prior < 2; prior++) {
        test_streaming = 1;
        upload(0, 512);
        test_streaming = prior;
        check(20, GRAY, "every row uploaded before the load");
        gpu_depth24_on_savestate_loaded();
        test_streaming = 1;
        check(20, BLACK, "save-state load drops the record");
        upload(20, 1);
        check(20, GRAY, "upload after the load is visible");
    }

    test_streaming = 0;
    check(21, GRAY, "MDEC idle shows every row");
    if (vram[20u * 1024u] != 0x1212) {
        printf("FAIL: guest VRAM changed\n");
        failures++;
    }
    if (failures) {
        printf("depth24 row policy: %d failure(s)\n", failures);
        return 1;
    }
    printf("depth24 row policy: all checks passed\n");
    return 0;
}
