/* test_host_overlay.c -- unit tests for host_overlay.c hooks. */
#include "host_overlay.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int refuse_on = 0;
static int refuse(void) { return refuse_on; }

static void test_pause(void) {
    CHECK(psx_host_pause_depth() == 0 && psx_host_pause_reason() == NULL);
    CHECK(psx_host_pause_push("overlay"));
    CHECK(psx_host_pause_push("other"));
    CHECK(psx_host_pause_depth() == 2 && strcmp(psx_host_pause_reason(), "other") == 0);
    psx_host_pause_pop();
    CHECK(strcmp(psx_host_pause_reason(), "overlay") == 0);
    psx_host_pause_pop();
    psx_host_pause_pop(); /* extra pop is harmless */
    CHECK(psx_host_pause_depth() == 0);
    psx_host_pause_set_refuse_probe(refuse);
    refuse_on = 1;
    CHECK(!psx_host_pause_push("netplay") && psx_host_pause_depth() == 0);
    refuse_on = 0;
    CHECK(psx_host_pause_push(NULL) && strcmp(psx_host_pause_reason(), "host") == 0);
    psx_host_pause_pop();
    for (int i = 0; i < 8; i++) CHECK(psx_host_pause_push("n"));
    CHECK(!psx_host_pause_push("overflow"));
    for (int i = 0; i < 8; i++) psx_host_pause_pop();
}

static int draws = 0, last_w = 0, last_h = 0;
static void *last_ctx = NULL;
static void on_draw(int w, int h, void *ctx) { draws++; last_w = w; last_h = h; last_ctx = ctx; }

static void test_draw(void) {
    int tag = 7;
    CHECK(!psx_host_overlay_has_draw());
    psx_host_overlay_draw(640, 480); /* no callback: nothing happens */
    psx_host_overlay_set_draw_cb(on_draw, &tag);
    CHECK(psx_host_overlay_has_draw());
    psx_host_overlay_draw(1280, 800);
    CHECK(draws == 1 && last_w == 1280 && last_h == 800 && last_ctx == &tag);
    psx_host_overlay_draw(0, 800); /* minimised window: skipped */
    CHECK(draws == 1);
    psx_host_overlay_set_draw_cb(NULL, NULL);
    psx_host_overlay_draw(1280, 800);
    CHECK(draws == 1 && !psx_host_overlay_has_draw());
}

static int sunk = 0;
static int sink(const void *ev, void *ctx) { (void)ctx; sunk++; return *(const int *)ev == 1; }

static void test_sink(void) {
    int a = 1, b = 2;
    CHECK(!psx_host_input_sink_dispatch(&a)); /* none registered */
    psx_host_set_input_sink(sink, NULL);
    CHECK(psx_host_input_sink_dispatch(&a) && !psx_host_input_sink_dispatch(&b) && sunk == 2);
    CHECK(!psx_host_input_sink_dispatch(NULL) && sunk == 2);
    psx_host_set_input_sink(NULL, NULL);
    CHECK(!psx_host_input_sink_dispatch(&a));
    CHECK(!psx_host_ui_capture_active());
    psx_host_set_ui_capture(5);
    CHECK(psx_host_ui_capture_active() == 1);
    psx_host_set_ui_capture(0);
    CHECK(!psx_host_ui_capture_active());
}

static void test_video(void) {
    PsxHostVideoSettings a, b;
    memset(&a, 0, sizeof a);
    a.fmv_filter = 2; a.renderer = 1; a.render_thread = 1; a.scanline_strength_pct = 50;
    b = a;
    CHECK(psx_host_video_live_changes(&a, &b) == 0 && psx_host_video_restart_bits(&a, &b) == 0);
    b.scanline_strength_pct = 60; b.present_linear = 1; b.fullscreen = 1;
    CHECK(psx_host_video_live_changes(&a, &b) ==
          (PSX_VIDEO_LIVE_SCANLINES | PSX_VIDEO_LIVE_PRESENT_LINEAR | PSX_VIDEO_LIVE_FULLSCREEN));
    CHECK(psx_host_video_restart_bits(&a, &b) == 0);
    b.internal_resolution = 4; b.present_thread = 1;
    CHECK(psx_host_video_restart_bits(&a, &b) == (PSX_VIDEO_RESTART_RESOLUTION | PSX_VIDEO_RESTART_THREADS));
    b.renderer = 0;
    CHECK(psx_host_video_restart_bits(&a, &b) & PSX_VIDEO_RESTART_RENDERER);
    CHECK(psx_host_video_live_changes(NULL, &b) == 0 && psx_host_video_restart_bits(&a, NULL) == 0);
}

int main(void) {
    test_video();
    test_sink();
    test_draw();
    test_pause();
    printf("host_overlay_test: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
