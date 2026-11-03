/* host_enhancements.c -- this session's requests from the built-in host
 * enhancement mods and the option-text parsers their plugins share
 * (host_enhancements.h). No SDL, no GL: runtime/tests/test_host_enhancements.c
 * links it with the plugins and the real mod runtime. */
#include "host_enhancements.h"
#include "video_texture_filter.h"

#include <stdlib.h>
#include <string.h>

static PsxHostEnhRequests s_req;
static PsxHostEnhApplier s_applier;
static PsxHostEnhResolutionApplier s_resolution_applier;

void psx_host_enh_reset(void) {
    memset(&s_req, 0, sizeof s_req);
}

void psx_host_enh_request_crt(int screen_kind, int scanlines, int strength_pct) {
    s_req.crt = 1;
    s_req.crt_screen_kind = screen_kind;
    s_req.crt_scanlines = scanlines;
    s_req.crt_strength_pct = strength_pct;
}

void psx_host_enh_request_image(int smoothing, int texture_filter, int fmv_filter) {
    s_req.image = 1;
    s_req.image_smoothing = smoothing;
    s_req.image_texture_filter = texture_filter;
    s_req.image_fmv_filter = fmv_filter;
}

void psx_host_enh_request_audio(void) {
    s_req.audio = 1;
}

void psx_host_enh_request_rewind(int depth, int interval) {
    s_req.rewind = 1;
    s_req.rewind_depth = depth;
    s_req.rewind_interval = interval;
}

void psx_host_enh_request_resolution(int value) {
    s_req.resolution = 1;
    s_req.resolution_value = value;
}

void psx_host_enh_requests(PsxHostEnhRequests* out) {
    if (out) *out = s_req;
}

void psx_host_enh_set_live_applier(PsxHostEnhApplier fn) {
    s_applier = fn;
}

int psx_host_enh_apply_live(void) {
    if (!s_applier) return 0;
    s_applier();
    return 1;
}

void psx_host_enh_set_resolution_applier(PsxHostEnhResolutionApplier fn) {
    s_resolution_applier = fn;
}

int psx_host_enh_apply_resolution_live(void) {
    return s_resolution_applier ? (s_resolution_applier() ? 1 : 0) : 0;
}

static int is(const char* a, const char* b) {
    return a && strcmp(a, b) == 0;
}

int psx_host_enh_parse_screen_kind(const char* text) {
    if (is(text, "crt")) return 1;
    if (is(text, "composite")) return 2;
    if (is(text, "trinitron")) return 3;
    return PSX_HOST_ENH_DEFAULT;
}

int psx_host_enh_parse_tristate(const char* text) {
    if (is(text, "on")) return 1;
    if (is(text, "off")) return 0;
    return PSX_HOST_ENH_DEFAULT;
}

static int parse_int(const char* text, int fallback) {
    if (!text || !*text) return fallback;
    char* end = NULL;
    const long v = strtol(text, &end, 10);
    if (!end || *end) return fallback;
    return (int)v;
}

int psx_host_enh_parse_strength(const char* text) {
    const int v = parse_int(text, PSX_HOST_ENH_STRENGTH_DEFAULT);
    return v < 1 || v > 100 ? PSX_HOST_ENH_STRENGTH_DEFAULT : v;
}

int psx_host_enh_parse_smoothing(const char* text) {
    if (is(text, "smooth")) return 1;
    if (is(text, "sharp")) return 0;
    return PSX_HOST_ENH_DEFAULT;
}

int psx_host_enh_parse_texture_filter(const char* text) {
    if (is(text, "nearest")) return PSX_TEXTURE_FILTER_NEAREST;
    if (is(text, "bilinear")) return PSX_TEXTURE_FILTER_BILINEAR;
    if (is(text, "xbr")) return PSX_TEXTURE_FILTER_XBR;
    return PSX_HOST_ENH_DEFAULT;
}

/* VIDEO_FMV_FILTER_* (config_loader.h, C++): nearest 0, bilinear 1,
 * sharp 2, bicubic 3 -- the same spellings as [video] fmv_filter. */
int psx_host_enh_parse_fmv_filter(const char* text) {
    if (is(text, "nearest")) return 0;
    if (is(text, "bilinear")) return 1;
    if (is(text, "sharp")) return 2;
    if (is(text, "bicubic")) return 3;
    return PSX_HOST_ENH_DEFAULT;
}

int psx_host_enh_parse_rewind_depth(const char* text) {
    const int v = parse_int(text, PSX_HOST_ENH_REWIND_DEPTH_DEFAULT);
    return v == 50 || v == 100 || v == 150 || v == 200
        ? v : PSX_HOST_ENH_REWIND_DEPTH_DEFAULT;
}

/* Only the preset ids: a manifest choice is never a raw line count. */
int psx_host_enh_parse_internal_resolution(const char* text) {
    if (is(text, "default")) return PSX_IR_UNSET;
    if (!text) return PSX_HOST_ENH_RESOLUTION_DEFAULT;
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++)
        if (is(text, k_psx_ir_presets[i].id)) return k_psx_ir_presets[i].value;
    return PSX_HOST_ENH_RESOLUTION_DEFAULT;
}

int psx_host_enh_parse_rewind_interval(const char* text) {
    const int v = parse_int(text, PSX_HOST_ENH_REWIND_INTERVAL_DEFAULT);
    return v == 1 || v == 4 || v == 8 || v == 12 || v == 15
        ? v : PSX_HOST_ENH_REWIND_INTERVAL_DEFAULT;
}
