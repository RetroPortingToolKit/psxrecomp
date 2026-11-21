/* test_host_enhancements.c -- the player's host enhancement settings
 * (internal resolution, CRT filter, image filtering, enhanced audio,
 * post-processing, rewind) and the session requests made from them
 * (host_enhancements.h).
 *
 * Links host_enhancements.c alone and checks:
 *   - resolve(): without a request the title's baseline stands; a setting at
 *     Game default keeps the baseline value; a chosen value replaces it; the
 *     scanline strength belongs to the player's own scanlines;
 *   - the player record: psx_host_enh_player_set() takes the settings.toml
 *     ids and values, psx_host_enh_reset() makes them this session's
 *     requests, psx_host_enh_player_clear() forgets them (4x internal
 *     resolution, everything else off or Game default);
 *   - internal resolution: Game default keeps the title's value and dynamic
 *     resolution, Auto turns dynamic resolution on, a fixed choice turns it
 *     off, and the player's Automatic resolution switch wins over both. */
#include "host_enhancements.h"
#include "video_texture_filter.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void test_post_processing(void) {
    const PsxHostPostLook base = {1, 0.37f, "title.pfx"};
    PsxHostEnhRequests req;
    psx_host_enh_player_clear();
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    PsxHostPostLook out = psx_host_enh_resolve_post(&base, &req);
    CHECK(out.antialiasing == 1 && out.bloom == 0.37f && !strcmp(out.shader, "title.pfx"));
    CHECK(psx_host_enh_player_set("video.antialiasing_mode", "fxaa_hq"));
    CHECK(psx_host_enh_player_set("video.bloom", "1.5"));
    CHECK(psx_host_enh_player_set("video.post_shader", "custom.pfx"));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    out = psx_host_enh_resolve_post(&base, &req);
    CHECK(out.antialiasing == 2 && out.bloom == 1.5f && !strcmp(out.shader, "custom.pfx"));
    CHECK(psx_host_enh_player_set("video.antialiasing_mode", "off"));
    CHECK(psx_host_enh_player_set("video.bloom", "0"));
    CHECK(psx_host_enh_player_set("video.post_shader", ""));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    out = psx_host_enh_resolve_post(&base, &req);
    CHECK(out.antialiasing == 0 && out.bloom == 0 && !*out.shader);
    CHECK(psx_host_enh_parse_post_aa("fxaa_hq") == 2);
    CHECK(psx_host_enh_parse_supersample("1.333") == 1333);
    CHECK(psx_host_enh_parse_supersample("nan") == 0);
    CHECK(psx_host_enh_parse_supersample("5") == 0);
    CHECK(psx_host_enh_parse_resolution_min("native") == PSX_IR_NATIVE);
}

static void test_resolution(void) {
    PsxHostEnhRequests req;
    memset(&req, 0, sizeof req);
    /* A title shipping 1080p with dynamic resolution on under it. */
    const PsxHostResolution base = {1080, 1, 720, 1500};
    PsxHostResolution r = psx_host_enh_resolve_resolution(&base, &req);
    CHECK(r.internal_resolution == PSX_IR_NATIVE && r.dynamic_resolution == 0); /* mod off */
    req.resolution = 1;
    req.resolution_value = PSX_IR_UNSET;                                 /* Game default */
    r = psx_host_enh_resolve_resolution(&base, &req);
    CHECK(r.internal_resolution == 1080 && r.dynamic_resolution == 1);
    req.resolution_value = 2160;                                         /* 4K: fixed */
    r = psx_host_enh_resolve_resolution(&base, &req);
    CHECK(r.internal_resolution == 2160 && r.dynamic_resolution == 0);
    req.resolution_value = PSX_IR_AUTO;
    r = psx_host_enh_resolve_resolution(&base, &req);
    CHECK(r.internal_resolution == PSX_IR_AUTO && r.dynamic_resolution == 1);
    req.resolution_value = PSX_IR_DISPLAY;                               /* not "default" */
    r = psx_host_enh_resolve_resolution(&base, &req);
    CHECK(r.internal_resolution == PSX_IR_DISPLAY && r.dynamic_resolution == 0);
    /* A title with only the legacy factor: Game default keeps it unset. */
    const PsxHostResolution legacy = {PSX_IR_UNSET, 0};
    req.resolution_value = PSX_IR_UNSET;
    r = psx_host_enh_resolve_resolution(&legacy, &req);
    CHECK(r.internal_resolution == PSX_IR_UNSET && r.dynamic_resolution == 0);
    req.resolution_min = PSX_IR_NATIVE;
    req.resolution_supersample = 1333;
    req.resolution_dynamic = 2;
    r = psx_host_enh_resolve_resolution(&base, &req);
    CHECK(r.dynamic_resolution_min == PSX_IR_NATIVE && r.supersample_milli == 1333 && r.dynamic_resolution);
    req.resolution_min = req.resolution_supersample = req.resolution_dynamic = 0;
    req.resolution = 0;
    req.resolution_value = PSX_IR_NATIVE;                                /* mod off again */
    r = psx_host_enh_resolve_resolution(&base, &req);
    CHECK(r.internal_resolution == PSX_IR_NATIVE && r.dynamic_resolution == 0);

    /* Old explicit choices keep their meaning; absent/invalid text uses 4x. */
    static const struct { const char* text; int value; } choices[] = {
        {"default", PSX_IR_UNSET}, {"native", PSX_IR_NATIVE}, {"720p", 720},
        {"1080p", 1080}, {"1440p", 1440}, {"4k", 2160}, {"5k", 2880}, {"8k", 4320},
        {"display", PSX_IR_DISPLAY}, {"auto", PSX_IR_AUTO},
        {"4x", PSX_IR_SCALE(4)}, {"32x", PSX_IR_SCALE(32)},
        {"", PSX_HOST_ENH_RESOLUTION_DEFAULT},
        {"1600", PSX_HOST_ENH_RESOLUTION_DEFAULT}, {"4K", PSX_HOST_ENH_RESOLUTION_DEFAULT},
    };
    for (unsigned i = 0; i < sizeof choices / sizeof choices[0]; ++i)
        CHECK(psx_host_enh_parse_internal_resolution(choices[i].text) == choices[i].value);
    CHECK(psx_host_enh_parse_internal_resolution(NULL) == PSX_HOST_ENH_RESOLUTION_DEFAULT);

    /* The player setting: a fresh profile is a fixed 4x. */
    psx_host_enh_player_clear();
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(req.resolution && req.resolution_value == PSX_IR_SCALE(4));
    const PsxHostResolution old_4k = {2160, 1};
    r = psx_host_enh_resolve_resolution(&old_4k, &req);
    CHECK(r.internal_resolution == PSX_IR_SCALE(4) && r.dynamic_resolution == 0);
    CHECK(psx_resolve_internal_scale(r.internal_resolution, 240, 0, 32) == 4);
    CHECK(psx_resolve_internal_scale(r.internal_resolution, 480, 0, 32) == 4);
    /* The Automatic resolution switch keeps the fixed ceiling. */
    CHECK(psx_host_enh_player_set("video.dynamic_resolution", "true"));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    r = psx_host_enh_resolve_resolution(&old_4k, &req);
    CHECK(r.internal_resolution == PSX_IR_SCALE(4) && r.dynamic_resolution == 1);
    CHECK(psx_host_enh_player_set("video.internal_resolution", "16x"));
    CHECK(psx_host_enh_player_set("video.dynamic_resolution", "false"));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    r = psx_host_enh_resolve_resolution(&old_4k, &req);
    CHECK(r.internal_resolution == PSX_IR_SCALE(16) && r.dynamic_resolution == 0);
    /* Game default: the title's own choice and its dynamic resolution. */
    psx_host_enh_player_clear();
    CHECK(psx_host_enh_player_set("video.internal_resolution", "default"));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    r = psx_host_enh_resolve_resolution(&old_4k, &req);
    CHECK(r.internal_resolution == 2160 && r.dynamic_resolution == 1);
    /* A session's requests are a copy: changing the record later does not
     * reach the running session until the next reset. */
    CHECK(psx_host_enh_player_set("video.internal_resolution", "2x"));
    psx_host_enh_requests(&req);
    CHECK(req.resolution_value == PSX_IR_UNSET);
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(req.resolution_value == PSX_IR_SCALE(2));
    psx_host_enh_player_clear();
}

static void test_resolve(void) {
    /* A Tomba-like title: nearest, no smoothing, composite, no scanlines. */
    const PsxHostLook base = {2, 0, 30, 0, PSX_TEXTURE_FILTER_NEAREST, 0, 0, 1};
    PsxHostEnhRequests req;
    memset(&req, 0, sizeof req);
    PsxHostLook out = psx_host_enh_resolve(&base, &req);
    CHECK(!memcmp(&out, &base, sizeof out));   /* every mod off: the baseline */

    req.crt = 1;
    req.crt_screen_kind = PSX_HOST_ENH_DEFAULT;
    req.crt_scanlines = PSX_HOST_ENH_DEFAULT;
    req.crt_strength_pct = 80;
    req.image = 1;
    req.image_smoothing = PSX_HOST_ENH_DEFAULT;
    req.image_texture_filter = PSX_HOST_ENH_DEFAULT;
    req.image_fmv_filter = PSX_HOST_ENH_DEFAULT;
    req.image_dithering = PSX_HOST_ENH_DEFAULT;
    out = psx_host_enh_resolve(&base, &req);
    CHECK(!memcmp(&out, &base, sizeof out));   /* all Game default: the baseline */

    req.crt_screen_kind = 3;
    req.crt_scanlines = 1;
    req.image_smoothing = 1;
    req.image_texture_filter = PSX_TEXTURE_FILTER_XBR;
    req.image_fmv_filter = 3;
    req.audio = 1;
    out = psx_host_enh_resolve(&base, &req);
    CHECK(out.screen_kind == 3 && out.scanlines == 1 && out.scanline_strength_pct == 80);
    CHECK(out.present_linear == 1 && out.texture_filter == PSX_TEXTURE_FILTER_XBR);
    CHECK(out.fmv_filter == 3 && out.spu_hq == 1);

    /* Scanlines Off: the strength is irrelevant and the title's is kept. */
    req.crt_scanlines = 0;
    out = psx_host_enh_resolve(&base, &req);
    CHECK(out.scanlines == 0 && out.scanline_strength_pct == 30);
    /* Game default scanlines over a title that has them: its own strength. */
    PsxHostLook lined = base;
    lined.scanlines = 1;
    req.crt_scanlines = PSX_HOST_ENH_DEFAULT;
    out = psx_host_enh_resolve(&lined, &req);
    CHECK(out.scanlines == 1 && out.scanline_strength_pct == 30);
    /* Smoothing Sharp over a smoothing title. */
    PsxHostLook smooth = base;
    smooth.present_linear = 1;
    req.image_smoothing = 0;
    out = psx_host_enh_resolve(&smooth, &req);
    CHECK(out.present_linear == 0);
    /* Dithering choices replace only their part; default keeps each title. */
    req.image_dithering = 0;
    out = psx_host_enh_resolve(&base, &req);
    CHECK(out.dithering == 0 && out.texture_filter == PSX_TEXTURE_FILTER_XBR);
    req.image_dithering = 2;
    out = psx_host_enh_resolve(&base, &req);
    CHECK(out.dithering == 2);
    PsxHostLook fine = base;
    fine.dithering = 2;
    req.image_dithering = PSX_HOST_ENH_DEFAULT;
    out = psx_host_enh_resolve(&fine, &req);
    CHECK(out.dithering == 2);
    /* The mod off again: nothing it asked for survives. */
    req.crt = req.image = req.audio = 0;
    out = psx_host_enh_resolve(&base, &req);
    CHECK(!memcmp(&out, &base, sizeof out));
}

static void test_parsers(void) {
    CHECK(psx_host_enh_parse_screen_kind("raw") == 0);
    CHECK(psx_host_enh_parse_screen_kind("crt") == 1);
    CHECK(psx_host_enh_parse_screen_kind("composite") == 2);
    CHECK(psx_host_enh_parse_screen_kind("trinitron") == 3);
    CHECK(psx_host_enh_parse_screen_kind("default") == PSX_HOST_ENH_DEFAULT);
    CHECK(psx_host_enh_parse_screen_kind(NULL) == PSX_HOST_ENH_DEFAULT);
    CHECK(psx_host_enh_parse_tristate("on") == 1 && psx_host_enh_parse_tristate("off") == 0);
    CHECK(psx_host_enh_parse_tristate("") == PSX_HOST_ENH_DEFAULT);
    CHECK(psx_host_enh_parse_strength("1") == 1 && psx_host_enh_parse_strength("100") == 100);
    CHECK(psx_host_enh_parse_strength("0") == 50 && psx_host_enh_parse_strength("101") == 50);
    CHECK(psx_host_enh_parse_strength("5x") == 50 && psx_host_enh_parse_strength("") == 50);
    CHECK(psx_host_enh_parse_smoothing("smooth") == 1 && psx_host_enh_parse_smoothing("sharp") == 0);
    CHECK(psx_host_enh_parse_dithering("off") == 0);
    CHECK(psx_host_enh_parse_dithering("native") == 1);
    CHECK(psx_host_enh_parse_dithering("fine") == 2);
    CHECK(psx_host_enh_parse_dithering("default") == PSX_HOST_ENH_DEFAULT);
    CHECK(psx_host_enh_parse_dithering(NULL) == PSX_HOST_ENH_DEFAULT);
    CHECK(psx_host_enh_parse_dithering("invalid") == PSX_HOST_ENH_DEFAULT);
    CHECK(psx_host_enh_parse_texture_filter("nearest") == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_host_enh_parse_texture_filter("bilinear") == PSX_TEXTURE_FILTER_BILINEAR);
    CHECK(psx_host_enh_parse_texture_filter("xbr") == PSX_TEXTURE_FILTER_XBR);
    CHECK(psx_host_enh_parse_texture_filter("stable") == PSX_TEXTURE_FILTER_STABLE_WORLD);
    CHECK(psx_host_enh_parse_fmv_filter("nearest") == 0 && psx_host_enh_parse_fmv_filter("bilinear") == 1);
    CHECK(psx_host_enh_parse_fmv_filter("sharp") == 2 && psx_host_enh_parse_fmv_filter("bicubic") == 3);
    CHECK(psx_host_enh_parse_rewind_depth("150") == 150 && psx_host_enh_parse_rewind_depth("75") == 50);
    CHECK(psx_host_enh_parse_rewind_interval("4") == 4 && psx_host_enh_parse_rewind_interval("5") == 15);
}

static void test_player(void) {
    PsxHostEnhRequests req;
    psx_host_enh_player_clear();
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(!req.crt && !req.image && !req.audio && !req.rewind);

    /* Unknown ids are not this record's (the caller applies them). */
    CHECK(!psx_host_enh_player_set("video.renderer", "opengl"));
    CHECK(!psx_host_enh_player_set(NULL, "x") && !psx_host_enh_player_set("video.rewind", NULL));

    CHECK(psx_host_enh_player_set("video.crt_filter", "trinitron"));
    CHECK(psx_host_enh_player_set("video.scanlines", "true"));
    CHECK(psx_host_enh_player_set("video.scanline_strength", "0.35"));
    CHECK(psx_host_enh_player_set("video.antialiasing", "false"));
    CHECK(psx_host_enh_player_set("video.texture_filtering", "bilinear"));
    CHECK(psx_host_enh_player_set("video.fmv_filter", "bicubic"));
    CHECK(psx_host_enh_player_set("video.dithering", "fine"));
    CHECK(psx_host_enh_player_set("audio.spu_hq", "true"));
    CHECK(psx_host_enh_player_set("video.rewind", "true"));
    CHECK(psx_host_enh_player_set("video.rewind_depth", "200"));
    CHECK(psx_host_enh_player_set("video.rewind_interval", "1"));
    /* Not a session's request until the next session start. */
    psx_host_enh_requests(&req);
    CHECK(!req.crt && !req.image && !req.audio && !req.rewind);
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(req.crt && req.crt_screen_kind == 3 && req.crt_scanlines == 1 &&
          req.crt_strength_pct == 35);
    CHECK(req.image && req.image_smoothing == 0 &&
          req.image_texture_filter == PSX_TEXTURE_FILTER_BILINEAR &&
          req.image_fmv_filter == 3 && req.image_dithering == 2);
    CHECK(req.audio == 1);
    CHECK(req.rewind && req.rewind_depth == 200 && req.rewind_interval == 1);

    /* The resolved look over a title's baseline. */
    const PsxHostLook base = {2, 0, 30, 1, PSX_TEXTURE_FILTER_NEAREST, 0, 0, 0};
    PsxHostLook out = psx_host_enh_resolve(&base, &req);
    CHECK(out.screen_kind == 3 && out.scanlines == 1 && out.scanline_strength_pct == 35);
    CHECK(out.present_linear == 0 && out.texture_filter == PSX_TEXTURE_FILTER_BILINEAR);
    CHECK(out.fmv_filter == 3 && out.dithering == 2 && out.spu_hq == 1);
    /* Enhanced audio off is an explicit off over a title that has it on. */
    CHECK(psx_host_enh_player_set("audio.spu_hq", "false"));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(req.audio == 2);
    PsxHostLook hq = base;
    hq.spu_hq = 1;
    out = psx_host_enh_resolve(&hq, &req);
    CHECK(out.spu_hq == 0);
    /* The player spelling "on" is the native dither. */
    CHECK(psx_host_enh_player_set("video.dithering", "on"));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(req.image_dithering == 1);
    /* Out-of-range text falls back to the documented defaults. */
    CHECK(psx_host_enh_player_set("video.rewind_depth", "75"));
    CHECK(psx_host_enh_player_set("video.rewind_interval", "5"));
    CHECK(psx_host_enh_player_set("video.scanline_strength", "2"));
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(req.rewind_depth == 50 && req.rewind_interval == 15 && req.crt_strength_pct == 50);

    /* Clear: the next session inherits nothing but the 4x default. */
    psx_host_enh_player_clear();
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(!req.crt && !req.image && !req.audio && !req.rewind && !req.post);
    CHECK(req.resolution && req.resolution_value == PSX_IR_SCALE(4));
}

static void test_explicit_supersampling(void) {
    CHECK(psx_host_enh_render_scale(PSX_IR_SCALE(4), 240, 1080, 32, 1333, 0, 2, 1) == 6);
    CHECK(psx_host_enh_render_scale(PSX_IR_SCALE(4), 240, 1080, 32, 1333, 0, 2, 0) == 4);
    CHECK(psx_host_enh_render_scale(PSX_IR_SCALE(32), 480, 1080, 32, 4000, 0, 2, 1) == 32);
    CHECK(psx_host_enh_render_scale(PSX_IR_NATIVE, 240, 1080, 32, 1333, 0, 2, 1) == 2);
    CHECK(psx_host_enh_render_scale(PSX_IR_UNSET, 240, 1080, 32, 1333, 0, 2, 1) == 3);
    CHECK(psx_host_enh_render_scale(720, 240, 1080, 32, 1333, 0, 2, 1) == 4);
}

int main(void) {
    test_explicit_supersampling();
    test_post_processing();
    test_resolve();
    test_parsers();
    test_player();
    test_resolution();
    if (g_fail) {
        fprintf(stderr, "host_enhancements_test: %d failure(s)\n", g_fail);
        return 1;
    }
    puts("host enhancements: baseline, Game default, player settings and reset PASS");
    return 0;
}
