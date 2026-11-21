/* host_enhancements.c -- the player's host enhancement settings, this
 * session's requests made from them, and the setting-text parsers
 * (host_enhancements.h). No SDL, no GL: runtime/tests/test_host_enhancements.c
 * links it alone. */
#include "host_enhancements.h"
#include "video_texture_filter.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static PsxHostEnhRequests s_req;
static PsxHostEnhRequests s_player;
static int s_player_ready;

static void player_ready(void) {
    if (!s_player_ready) psx_host_enh_player_clear();
}

void psx_host_enh_reset(void) {
    player_ready();
    s_req = s_player;
}

void psx_host_enh_requests(PsxHostEnhRequests* out) {
    if (out) *out = s_req;
}

void psx_host_enh_player_clear(void) {
    memset(&s_player, 0, sizeof s_player);
    s_player.crt_strength_pct = PSX_HOST_ENH_STRENGTH_DEFAULT;
    s_player.rewind_depth = PSX_HOST_ENH_REWIND_DEPTH_DEFAULT;
    s_player.rewind_interval = PSX_HOST_ENH_REWIND_INTERVAL_DEFAULT;
    s_player.resolution = 1;
    s_player.resolution_value = PSX_HOST_ENH_RESOLUTION_DEFAULT;
    s_player.resolution_min = PSX_IR_UNSET;
    s_player_ready = 1;
}

static void player_crt(void) {
    if (s_player.crt) return;
    s_player.crt = 1;
    s_player.crt_screen_kind = PSX_HOST_ENH_DEFAULT;
    s_player.crt_scanlines = PSX_HOST_ENH_DEFAULT;
}

static void player_image(void) {
    if (s_player.image) return;
    s_player.image = 1;
    s_player.image_smoothing = PSX_HOST_ENH_DEFAULT;
    s_player.image_texture_filter = PSX_HOST_ENH_DEFAULT;
    s_player.image_fmv_filter = PSX_HOST_ENH_DEFAULT;
    s_player.image_dithering = PSX_HOST_ENH_DEFAULT;
}

static void player_post(void) {
    if (s_player.post) return;
    s_player.post = 1;
    s_player.post_antialiasing = PSX_HOST_ENH_DEFAULT;
    s_player.post_bloom = PSX_HOST_ENH_DEFAULT;
    s_player.post_bloom_pct = 25;
    s_player.post_shader_mode = PSX_HOST_ENH_DEFAULT;
}

static int is(const char* a, const char* b);

static int flag(const char* value) {
    return is(value, "true");
}

int psx_host_enh_player_set(const char* id, const char* value) {
    player_ready();
    if (!id || !value) return 0;
    if (is(id, "video.internal_resolution")) {
        s_player.resolution = 1;
        s_player.resolution_value = psx_host_enh_parse_internal_resolution(value);
    } else if (is(id, "video.dynamic_resolution")) {
        s_player.resolution_dynamic = flag(value) ? 2 : 1;
    } else if (is(id, "video.dynamic_resolution_min")) {
        s_player.resolution_min = psx_host_enh_parse_resolution_min(value);
    } else if (is(id, "video.supersample")) {
        s_player.resolution_supersample = psx_host_enh_parse_supersample(value);
    } else if (is(id, "video.crt_filter")) {
        player_crt();
        s_player.crt_screen_kind = psx_host_enh_parse_screen_kind(value);
    } else if (is(id, "video.scanlines")) {
        player_crt();
        s_player.crt_scanlines = flag(value);
    } else if (is(id, "video.scanline_strength")) {
        const double v = strtod(value, NULL);
        player_crt();
        s_player.crt_strength_pct = v >= 0.0 && v <= 1.0 ? (int)floor(v * 100.0 + 0.5)
                                                         : PSX_HOST_ENH_STRENGTH_DEFAULT;
    } else if (is(id, "video.antialiasing")) {
        player_image();
        s_player.image_smoothing = flag(value);
    } else if (is(id, "video.texture_filtering")) {
        player_image();
        s_player.image_texture_filter = psx_host_enh_parse_texture_filter(value);
    } else if (is(id, "video.fmv_filter")) {
        player_image();
        s_player.image_fmv_filter = psx_host_enh_parse_fmv_filter(value);
    } else if (is(id, "video.dithering")) {
        /* The player spelling is off / on / fine. */
        player_image();
        s_player.image_dithering = psx_host_enh_parse_dithering(is(value, "on") ? "native" : value);
    } else if (is(id, "video.antialiasing_mode")) {
        player_post();
        s_player.post_antialiasing = psx_host_enh_parse_post_aa(value);
    } else if (is(id, "video.bloom")) {
        const double v = strtod(value, NULL);
        player_post();
        s_player.post_bloom = v > 0.0 ? 1 : 0;
        if (v > 0.0) {
            const int pct = (int)floor(v * 100.0 + 0.5);
            s_player.post_bloom_pct = pct < 1 ? 1 : pct > 200 ? 200 : pct;
        }
    } else if (is(id, "video.post_shader")) {
        player_post();
        s_player.post_shader_mode = *value ? 1 : 0;
        snprintf(s_player.post_shader, sizeof s_player.post_shader, "%s", value);
    } else if (is(id, "audio.spu_hq")) {
        s_player.audio = flag(value) ? 1 : 2;
    } else if (is(id, "video.rewind")) {
        s_player.rewind = flag(value);
    } else if (is(id, "video.rewind_depth")) {
        s_player.rewind_depth = psx_host_enh_parse_rewind_depth(value);
    } else if (is(id, "video.rewind_interval")) {
        s_player.rewind_interval = psx_host_enh_parse_rewind_interval(value);
    } else {
        return 0;
    }
    return 1;
}

static int is(const char* a, const char* b) {
    return a && strcmp(a, b) == 0;
}

int psx_host_enh_parse_screen_kind(const char* text) {
    if (is(text, "raw")) return 0;
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

int psx_host_enh_parse_dithering(const char* text) {
    if (is(text, "off")) return 0;
    if (is(text, "native")) return 1;
    if (is(text, "fine")) return 2;
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

int psx_host_enh_parse_resolution_min(const char* text) {
    int value = PSX_IR_UNSET;
    if (text && psx_ir_parse(text, &value) && !psx_ir_is_auto(value)) return value;
    return PSX_IR_UNSET;
}

int psx_host_enh_parse_supersample(const char* text) {
    if (!text || !*text || is(text, "default")) return 0;
    char* end;
    double value = strtod(text, &end);
    if (*end || !(value >= 1.0 && value <= 4.0)) return 0;
    return (int)(value * 1000.0 + 0.5);
}

int psx_host_enh_parse_post_aa(const char* text) {
    if (is(text, "off")) return 0;
    if (is(text, "fxaa")) return 1;
    if (is(text, "fxaa_hq")) return 2;
    return PSX_HOST_ENH_DEFAULT;
}

int psx_host_enh_parse_rewind_interval(const char* text) {
    const int v = parse_int(text, PSX_HOST_ENH_REWIND_INTERVAL_DEFAULT);
    return v == 1 || v == 4 || v == 8 || v == 12 || v == 15
        ? v : PSX_HOST_ENH_REWIND_INTERVAL_DEFAULT;
}
