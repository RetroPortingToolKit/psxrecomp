/* test_host_overlay.c -- unit tests for host_overlay.c hooks. */
#include "host_overlay.h"

#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif
#include <stdlib.h>
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

/* One texture-filter enum for settings, handoff and menu (xBR included). */
static void test_texture_filter(void) {
    CHECK(psx_texture_filter_from_ui(0) == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_texture_filter_from_ui(1) == PSX_TEXTURE_FILTER_BILINEAR);
    CHECK(psx_texture_filter_from_ui(3) == PSX_TEXTURE_FILTER_XBR);
    CHECK(psx_texture_filter_from_ui(9) == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_texture_filter_from_ui(-1) == PSX_TEXTURE_FILTER_NEAREST);
}

/* Windows relaunch quoting: a parser for the CommandLineToArgvW rules (the
 * real one too, on Windows) must give every argument back unchanged. */
static int parse_cmdline(const char* line, char out[][256], int max) {
    int n = 0;
    const char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || n >= max) break;
        char* o = out[n];
        size_t len = 0;
        int inq = 0;
        for (;;) {
            size_t bs = 0;
            while (*p == '\\') { ++bs; ++p; }
            if (*p == '"') {
                for (size_t i = 0; i < bs / 2; ++i) o[len++] = '\\';
                if (bs & 1) { o[len++] = '"'; ++p; continue; }
                ++p;
                if (inq && *p == '"') { o[len++] = '"'; ++p; continue; }
                inq = !inq;
                continue;
            }
            for (size_t i = 0; i < bs; ++i) o[len++] = '\\';
            if (!*p || (!inq && (*p == ' ' || *p == '\t'))) break;
            o[len++] = *p++;
        }
        o[len] = '\0';
        ++n;
    }
    return n;
}

static void test_cmdline_quote(void) {
    const char* args[] = {
        "C:\\Games\\R4 Recomp\\r4-runtime.exe",
        "--disc",
        "D:\\Discs\\R4 - Ridge Racer Type 4 (USA).cue",
        "--memcard-dir",
        "C:\\Users\\Me\\Saved Games\\R4\\",          /* trailing separator */
        "C:\\path\\with \"quote\".bin",                 /* embedded quote */
        "C:\\a\\b\\\\\"c",                            /* backslashes before a quote */
        "",                                                   /* empty argument */
        "plain",
        "tab\there",
        "trailing\\\\",                                    /* backslashes, no spaces */
    };
    const int n = (int)(sizeof(args) / sizeof(args[0]));
    char* line = psx_win_cmdline_join(n, args);
    char got[16][256];
    CHECK(line != NULL);
    CHECK(parse_cmdline(line, got, 16) == n);
    for (int i = 0; i < n; ++i) CHECK(strcmp(got[i], args[i]) == 0);
    /* No quoting when not needed; measuring matches writing. */
    char small[8];
    CHECK(psx_win_cmdline_quote("abc", small, sizeof small) == 3 && strcmp(small, "abc") == 0);
    CHECK(psx_win_cmdline_quote("a b", NULL, 0) == 5);
    CHECK(psx_win_cmdline_quote("", NULL, 0) == 2);
#if defined(_WIN32)
    {
        int wn = 0;
        int len = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
        wchar_t* wl = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)len);
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wl, len);
        wchar_t** wv = CommandLineToArgvW(wl, &wn);
        CHECK(wv && wn == n);
        for (int i = 1; wv && i < n && i < wn; ++i) {  /* argv[0] has its own rules; no quotes in exe paths */
            char u[512];
            WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, u, sizeof u, NULL, NULL);
            CHECK(strcmp(u, args[i]) == 0);
        }
        LocalFree(wv);
        free(wl);
    }
#endif
    free(line);
}

int main(void) {
    test_cmdline_quote();
    test_video();
    test_texture_filter();
    test_sink();
    test_draw();
    test_pause();
    printf("host_overlay_test: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
