/* test_host_enhancements.c -- the built-in host enhancement mods (CRT Filter,
 * Image Filtering, Enhanced Audio, Rewind, Internal Resolution) and their
 * session requests
 * (host_enhancements.h, beads-eio.3.398).
 *
 * Links the real plugins and host_enhancements.c against a fake mod runtime
 * (registration + option values), and pins:
 *   - resolve(): disabled Internal Resolution is native; other disabled mods
 *     keep the title's baseline; an option at
 *     Game default keeps the baseline value; an enabled option replaces it;
 *     the scanline strength belongs to the player's own scanlines;
 *   - each plugin's activation RECORDS a request from its options (unknown or
 *     missing text falls back to the documented default) and applies
 *     nothing itself;
 *   - reset forgets every request (a session with the mod off, or a netplay
 *     clear, cannot inherit one);
 *   - the CRT Filter / Image Filtering option-changed callbacks re-record and
 *     report live only once main.cpp registered its applier;
 *   - the Internal Resolution mod: Game default keeps the title's value and
 *     dynamic resolution, Auto turns dynamic resolution on, a fixed choice
 *     turns it off; its option change reports what main.cpp's resolution
 *     applier answers (it can refuse: no live-capable backend, netplay). */
#include "host_enhancements.h"
#include "mod_plugins.h"
#include "video_texture_filter.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* ---- fake mod runtime ---------------------------------------------------- */
typedef struct { const char* id; PSXModActivationCallback cb; } Act;
typedef struct { const char* id; PSXModOptionChangedCallback cb; } Opt;
static Act g_act[8]; static int g_nact;
static Opt g_opt[8]; static int g_nopt;
typedef struct { const char* pkg; const char* feature; const char* option; const char* value; } Val;
static Val g_val[16]; static int g_nval;

int psx_mod_register_activation_plugin(const char* id, PSXModActivationCallback cb) {
    g_act[g_nact].id = id; g_act[g_nact].cb = cb; g_nact++;
    return 1;
}
int psx_mod_register_option_changed_plugin(const char* id, PSXModOptionChangedCallback cb) {
    g_opt[g_nopt].id = id; g_opt[g_nopt].cb = cb; g_nopt++;
    return 1;
}
int psx_mod_option_value(const char* pkg, const char* feature, const char* option,
                         char* out, uint32_t out_size) {
    for (int i = g_nval - 1; i >= 0; --i)
        if (!strcmp(g_val[i].pkg, pkg) && !strcmp(g_val[i].feature, feature) &&
            !strcmp(g_val[i].option, option)) {
            snprintf(out, out_size, "%s", g_val[i].value);
            return 1;
        }
    if (out && out_size) out[0] = '\0';
    return 0;
}
static void set_opt(const char* pkg, const char* feature, const char* option, const char* value) {
    g_val[g_nval].pkg = pkg; g_val[g_nval].feature = feature;
    g_val[g_nval].option = option; g_val[g_nval].value = value; g_nval++;
}
static void activate(const char* id) {
    int found = 0;
    for (int i = 0; i < g_nact; ++i)
        if (!strcmp(g_act[i].id, id)) { g_act[i].cb(); found = 1; }
    CHECK(found);
}
static int option_changed(const char* id, const char* option, const char* value) {
    for (int i = 0; i < g_nopt; ++i)
        if (!strcmp(g_opt[i].id, id)) return g_opt[i].cb(option, value);
    return -1;
}
static int g_applied;
static void applier(void) { ++g_applied; }
static int g_res_applied, g_res_answer;
static int res_applier(void) { ++g_res_applied; return g_res_answer; }

#define CRT "psx.enhancement.crt-filter"
#define IMG "psx.enhancement.image-filtering"
#define RWD "psx.feature.rewind"
#define IR  "psx.enhancement.internal-resolution"

static void test_resolution(void) {
    PsxHostEnhRequests req;
    memset(&req, 0, sizeof req);
    /* A title shipping 1080p with dynamic resolution on under it. */
    const PsxHostResolution base = {1080, 1};
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

    /* The plugin: activation records, an option change asks the applier. */
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(!req.resolution && req.resolution_value == PSX_IR_UNSET);
    activate("psx.internal-resolution");                /* fresh profile: fixed 4x */
    psx_host_enh_requests(&req);
    CHECK(req.resolution && req.resolution_value == PSX_IR_SCALE(4));
    const PsxHostResolution old_4k = {2160, 1};
    r = psx_host_enh_resolve_resolution(&old_4k, &req);
    CHECK(r.internal_resolution == PSX_IR_SCALE(4) && r.dynamic_resolution == 0);
    CHECK(psx_resolve_internal_scale(r.internal_resolution, 240, 0, 32) == 4);
    CHECK(psx_resolve_internal_scale(r.internal_resolution, 480, 0, 32) == 4);
    set_opt(IR, "internal-resolution", "resolution", "auto");
    psx_host_enh_reset();
    activate("psx.internal-resolution");
    psx_host_enh_requests(&req);
    CHECK(req.resolution && req.resolution_value == PSX_IR_AUTO);
    /* No applier yet (renderer setup has not run): not live. */
    set_opt(IR, "internal-resolution", "resolution", "1440p");
    CHECK(option_changed("psx.internal-resolution", "resolution", "1440p") == 0);
    psx_host_enh_set_resolution_applier(res_applier);
    g_res_answer = 0;                                   /* backend cannot, or netplay */
    CHECK(option_changed("psx.internal-resolution", "resolution", "1440p") == 0 &&
          g_res_applied == 1);
    g_res_answer = 1;
    CHECK(option_changed("psx.internal-resolution", "resolution", "1440p") == 1 &&
          g_res_applied == 2);
    psx_host_enh_requests(&req);
    CHECK(req.resolution_value == 1440);
    psx_host_enh_set_resolution_applier(NULL);
    CHECK(psx_host_enh_apply_resolution_live() == 0);
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(!req.resolution && req.resolution_value == PSX_IR_UNSET);
}

static void test_resolve(void) {
    /* A Tomba-like title: nearest, no smoothing, composite, no scanlines. */
    const PsxHostLook base = {2, 0, 30, 0, PSX_TEXTURE_FILTER_NEAREST, 0, 0};
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
    /* The mod off again: nothing it asked for survives. */
    req.crt = req.image = req.audio = 0;
    out = psx_host_enh_resolve(&base, &req);
    CHECK(!memcmp(&out, &base, sizeof out));
}

static void test_parsers(void) {
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
    CHECK(psx_host_enh_parse_texture_filter("nearest") == PSX_TEXTURE_FILTER_NEAREST);
    CHECK(psx_host_enh_parse_texture_filter("bilinear") == PSX_TEXTURE_FILTER_BILINEAR);
    CHECK(psx_host_enh_parse_texture_filter("xbr") == PSX_TEXTURE_FILTER_XBR);
    CHECK(psx_host_enh_parse_texture_filter("stable") == PSX_HOST_ENH_DEFAULT);
    CHECK(psx_host_enh_parse_fmv_filter("nearest") == 0 && psx_host_enh_parse_fmv_filter("bilinear") == 1);
    CHECK(psx_host_enh_parse_fmv_filter("sharp") == 2 && psx_host_enh_parse_fmv_filter("bicubic") == 3);
    CHECK(psx_host_enh_parse_rewind_depth("150") == 150 && psx_host_enh_parse_rewind_depth("75") == 50);
    CHECK(psx_host_enh_parse_rewind_interval("4") == 4 && psx_host_enh_parse_rewind_interval("5") == 15);
}

static void test_plugins(void) {
    PsxHostEnhRequests req;
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(!req.crt && !req.image && !req.audio && !req.rewind);

    /* Activation with no option values (defaults): requests recorded with
     * Game default everywhere, the documented numbers otherwise. */
    activate("psx.crt-filter");
    activate("psx.image-filtering");
    activate("psx.enhanced-audio");
    activate("psx.rewind");
    psx_host_enh_requests(&req);
    CHECK(req.crt && req.crt_screen_kind == PSX_HOST_ENH_DEFAULT &&
          req.crt_scanlines == PSX_HOST_ENH_DEFAULT && req.crt_strength_pct == 50);
    CHECK(req.image && req.image_smoothing == PSX_HOST_ENH_DEFAULT &&
          req.image_texture_filter == PSX_HOST_ENH_DEFAULT &&
          req.image_fmv_filter == PSX_HOST_ENH_DEFAULT);
    CHECK(req.audio);
    CHECK(req.rewind && req.rewind_depth == 50 && req.rewind_interval == 15);

    /* Chosen options. */
    set_opt(CRT, "crt-filter", "screen_model", "trinitron");
    set_opt(CRT, "crt-filter", "scanlines", "on");
    set_opt(CRT, "crt-filter", "scanline_strength", "35");
    set_opt(IMG, "image-filtering", "smoothing", "sharp");
    set_opt(IMG, "image-filtering", "texture_filtering", "bilinear");
    set_opt(IMG, "image-filtering", "fmv_filtering", "bicubic");
    set_opt(RWD, "rewind", "buffer", "200");
    set_opt(RWD, "rewind", "interval", "1");
    psx_host_enh_reset();
    activate("psx.crt-filter");
    activate("psx.image-filtering");
    activate("psx.rewind");
    psx_host_enh_requests(&req);
    CHECK(req.crt_screen_kind == 3 && req.crt_scanlines == 1 && req.crt_strength_pct == 35);
    CHECK(req.image_smoothing == 0 && req.image_texture_filter == PSX_TEXTURE_FILTER_BILINEAR &&
          req.image_fmv_filter == 3);
    CHECK(!req.audio);   /* not activated this session */
    CHECK(req.rewind_depth == 200 && req.rewind_interval == 1);

    /* Reset: a session without the mods inherits nothing. */
    psx_host_enh_reset();
    psx_host_enh_requests(&req);
    CHECK(!req.crt && !req.image && !req.audio && !req.rewind);

    /* Live option change: re-recorded; live only once main.cpp registered. */
    activate("psx.crt-filter");
    CHECK(option_changed("psx.crt-filter", "scanlines", "off") == 0);
    set_opt(CRT, "crt-filter", "scanlines", "off");
    psx_host_enh_set_live_applier(applier);
    CHECK(option_changed("psx.crt-filter", "scanlines", "off") == 1 && g_applied == 1);
    psx_host_enh_requests(&req);
    CHECK(req.crt && req.crt_scanlines == 0);
    set_opt(IMG, "image-filtering", "texture_filtering", "xbr");
    CHECK(option_changed("psx.image-filtering", "texture_filtering", "xbr") == 1 && g_applied == 2);
    psx_host_enh_requests(&req);
    CHECK(req.image && req.image_texture_filter == PSX_TEXTURE_FILTER_XBR);
    psx_host_enh_set_live_applier(NULL);
    CHECK(psx_host_enh_apply_live() == 0);
    /* Enhanced Audio and Rewind change at the next start (no live callback). */
    CHECK(option_changed("psx.enhanced-audio", "x", "y") == -1);
    CHECK(option_changed("psx.rewind", "buffer", "100") == -1);
}

int main(void) {
    test_resolve();
    test_parsers();
    test_plugins();
    test_resolution();
    if (g_fail) {
        fprintf(stderr, "host_enhancements_test: %d failure(s)\n", g_fail);
        return 1;
    }
    puts("host enhancements: baseline, Game default, requests, reset and live apply PASS");
    return 0;
}
