/* internal_resolution.h — the internal resolution: presets, their ids (the
 * game.toml [video] internal_resolution spellings and the built-in Internal
 * Resolution mod's option values), and the integer scale each one asks the
 * renderer for.
 *
 * Pure (no SDL, no GL) so the config loader, the mod plugin and a test can
 * use it.
 *
 * VALUE ENCODING:
 *   0   unset: the legacy [video] supersampling factor stands
 *   1   native (1x)
 *  -1   match display: the target is the monitor's pixel height
 *  -2   auto: Match display as the ceiling with dynamic resolution on (the
 *       scale steps down to the title's dynamic_resolution_min floor only
 *       while the game would miss frames). Every scale helper below resolves
 *       it exactly like Match display; only the dynamic-resolution switch
 *       differs, and that is the caller's (psx_ir_is_auto).
 *  -101..-132 fixed integer scales 1x..32x, independent of reference height
 *   N   (N >= 2) a target output height in lines (720, 1080, ...)
 *
 * SCALE. The renderer only does integer scales (the pack of the hr surface
 * back to native VRAM is exact only then), so a target height becomes
 *     S = ceil(target_lines / reference_lines)
 * clamped to [1, s_max]. reference_lines is the title's usual display height
 * ([video] resolution_reference_lines, default 240): R4's races are 320x240,
 * so 1080p is S = 5 (1200 lines, area-resolved to 1080) and 4K is S = 9.
 * A 480-line mode (interlaced menus) renders at twice the target and is
 * resolved down. The backend then clamps S again to what it can allocate. */
#pragma once

#include <stddef.h>

#define PSX_IR_UNSET     0
#define PSX_IR_NATIVE    1
#define PSX_IR_DISPLAY  (-1)
#define PSX_IR_AUTO     (-2)
#define PSX_IR_DEFAULT_REF_LINES 240
#define PSX_IR_MAX_SCALE 32
#define PSX_IR_SCALE_BASE (-100)
#define PSX_IR_SCALE(scale) (PSX_IR_SCALE_BASE - (scale))
/* Custom line counts accepted from config (a 16K panel is 8640 lines). */
#define PSX_IR_MIN_LINES 2
#define PSX_IR_MAX_LINES 8640

typedef struct PsxIrPreset {
    const char *id;     /* stable id: game.toml and the mod option value */
    const char *label;  /* display label (the mod option's) */
    int         value;  /* encoding above */
} PsxIrPreset;

static const PsxIrPreset k_psx_ir_presets[] = {
    /* Fixed scales come first; legacy height ids below remain valid saved choices. */
    { "1x", "1x (Native)", PSX_IR_SCALE(1) },
    { "2x", "2x", PSX_IR_SCALE(2) },
    { "3x", "3x", PSX_IR_SCALE(3) },
    { "4x", "4x", PSX_IR_SCALE(4) },
    { "5x", "5x", PSX_IR_SCALE(5) },
    { "6x", "6x", PSX_IR_SCALE(6) },
    { "7x", "7x", PSX_IR_SCALE(7) },
    { "8x", "8x", PSX_IR_SCALE(8) },
    { "9x", "9x", PSX_IR_SCALE(9) },
    { "10x", "10x", PSX_IR_SCALE(10) },
    { "11x", "11x", PSX_IR_SCALE(11) },
    { "12x", "12x", PSX_IR_SCALE(12) },
    { "13x", "13x", PSX_IR_SCALE(13) },
    { "14x", "14x", PSX_IR_SCALE(14) },
    { "15x", "15x", PSX_IR_SCALE(15) },
    { "16x", "16x", PSX_IR_SCALE(16) },
    { "17x", "17x", PSX_IR_SCALE(17) },
    { "18x", "18x", PSX_IR_SCALE(18) },
    { "19x", "19x", PSX_IR_SCALE(19) },
    { "20x", "20x", PSX_IR_SCALE(20) },
    { "21x", "21x", PSX_IR_SCALE(21) },
    { "22x", "22x", PSX_IR_SCALE(22) },
    { "23x", "23x", PSX_IR_SCALE(23) },
    { "24x", "24x", PSX_IR_SCALE(24) },
    { "25x", "25x", PSX_IR_SCALE(25) },
    { "26x", "26x", PSX_IR_SCALE(26) },
    { "27x", "27x", PSX_IR_SCALE(27) },
    { "28x", "28x", PSX_IR_SCALE(28) },
    { "29x", "29x", PSX_IR_SCALE(29) },
    { "30x", "30x", PSX_IR_SCALE(30) },
    { "31x", "31x", PSX_IR_SCALE(31) },
    { "32x", "32x", PSX_IR_SCALE(32) },
    { "native",  "Native",        PSX_IR_NATIVE  },
    { "720p",    "720p",          720            },
    { "1080p",   "1080p",         1080           },
    { "1440p",   "1440p",         1440           },
    { "4k",      "4K",            2160           },
    { "5k",      "5K",            2880           },
    { "8k",      "8K",            4320           },
    { "display", "Match display", PSX_IR_DISPLAY },
    { "auto",    "Auto",          PSX_IR_AUTO    },
};
#define PSX_IR_PRESET_COUNT ((int)(sizeof(k_psx_ir_presets) / sizeof(k_psx_ir_presets[0])))

static inline int psx_ir_lower_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        if (ca != *b) return 0;
    }
    return *a == 0 && *b == 0;
}

/* Fixed scale values do not depend on a title's reference lines or SSAA target. */
static inline int psx_ir_is_scale(int value) {
    return value <= PSX_IR_SCALE(1) && value >= PSX_IR_SCALE(PSX_IR_MAX_SCALE);
}

/* 1 if value is a legal encoding (unset excluded). */
static inline int psx_ir_value_valid(int value) {
    return psx_ir_is_scale(value) || value == PSX_IR_NATIVE || value == PSX_IR_DISPLAY || value == PSX_IR_AUTO ||
           (value >= PSX_IR_MIN_LINES && value <= PSX_IR_MAX_LINES);
}

/* Auto: Match display with dynamic resolution on. */
static inline int psx_ir_is_auto(int value) { return value == PSX_IR_AUTO; }

/* The fixed target a value renders at, its ceiling for Auto: Auto resolves
 * as Match display, everything else as itself. */
static inline int psx_ir_ceiling(int value) {
    return value == PSX_IR_AUTO ? PSX_IR_DISPLAY : value;
}

/* Parse a persisted value: a preset id (case-insensitive, "auto" included) or
 * a decimal line count in [PSX_IR_MIN_LINES, PSX_IR_MAX_LINES]. Returns 1 and
 * stores the value on success; 0 for anything else (the caller keeps its
 * default). A dynamic-resolution floor is a fixed scale/height: callers reject
 * PSX_IR_AUTO themselves. */
static inline int psx_ir_parse(const char *s, int *out) {
    if (!s || !*s) return 0;
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++)
        if (psx_ir_lower_eq(s, k_psx_ir_presets[i].id)) { *out = k_psx_ir_presets[i].value; return 1; }
    long v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; if (v > PSX_IR_MAX_LINES) return 0; }
    if (p == s || *p) return 0;
    if (v < PSX_IR_MIN_LINES) return 0;   /* a number is always lines, never "1 = native" */
    *out = (int)v;
    return 1;
}

/* The preset id for a value, or NULL for a custom line count (persist those
 * as an integer). */
static inline const char *psx_ir_id_for(int value) {
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++)
        if (k_psx_ir_presets[i].value == value) return k_psx_ir_presets[i].id;
    return NULL;
}

static inline const char *psx_ir_label_for(int value) {
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++)
        if (k_psx_ir_presets[i].value == value) return k_psx_ir_presets[i].label;
    return NULL;
}

/* Unclamped scale a value asks for. display_px_h <= 0 (unknown) makes Match
 * display 1 until the caller can measure the monitor. */
static inline int psx_ir_scale_for(int value, int ref_lines, int display_px_h) {
    if (psx_ir_is_scale(value)) return PSX_IR_SCALE_BASE - value;
    if (ref_lines < 1) ref_lines = PSX_IR_DEFAULT_REF_LINES;
    value = psx_ir_ceiling(value);
    int target;
    if (value == PSX_IR_DISPLAY) target = display_px_h;
    else if (value >= PSX_IR_MIN_LINES) target = value;
    else return 1;                                  /* native / unset */
    if (target <= ref_lines) return 1;
    return (target + ref_lines - 1) / ref_lines;
}

/* The scale to request: psx_ir_scale_for clamped to [1, s_max]. */
static inline int psx_resolve_internal_scale(int value, int ref_lines,
                                             int display_px_h, int s_max) {
    int s = psx_ir_scale_for(value, ref_lines, display_px_h);
    if (s_max >= 1 && s > s_max) s = s_max;
    return s < 1 ? 1 : s;
}

/* ---- Supersample ([video] supersample, PSX_SUPERSAMPLE) ----------------------
 * A factor on the TARGET of a line-count preset or Match display, in
 * thousandths (1000 = 1.0, the historical behaviour; range 1000..4000). The
 * renderer already area-resolves an internal image taller than the output, so
 * asking for 2x the display's lines is ordered-grid SSAA of the whole frame:
 * Match display at 1080 px with 2.0 renders 2160 lines (S = 9) and resolves
 * to 1080. Native and the legacy integer factor are left alone. Dynamic
 * resolution takes the result as its ceiling and steps under it as usual. */
#define PSX_SS_MILLI_MIN 1000
#define PSX_SS_MILLI_MAX 4000

static inline int psx_ss_milli_clamp(int m) {
    return m < PSX_SS_MILLI_MIN ? PSX_SS_MILLI_MIN
         : m > PSX_SS_MILLI_MAX ? PSX_SS_MILLI_MAX : m;
}

/* "1.5", "2", "2x" -> 1500, 2000, 2000. 0 for anything else. */
static inline int psx_ss_parse_milli(const char *s) {
    if (!s || !*s) return 0;
    long whole = 0, frac = 0, div = 1;
    const char *p = s;
    while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); p++; if (whole > 100) return 0; }
    if (p == s) return 0;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { if (div < 1000) { frac = frac * 10 + (*p - '0'); div *= 10; } p++; } }
    if (*p == 'x' || *p == 'X') p++;
    if (*p) return 0;
    long m = whole * 1000 + frac * (1000 / div);
    if (m < PSX_SS_MILLI_MIN || m > PSX_SS_MILLI_MAX) return 0;
    return (int)m;
}

/* Opt-in display policy; keep the saved factor intact for smaller displays. */
static inline int psx_ir_display_supersample(int ss_milli, int display_px_h,
                                            int disable_at_lines) {
    return disable_at_lines > 0 && display_px_h >= disable_at_lines
        ? PSX_SS_MILLI_MIN : ss_milli;
}

/* psx_resolve_internal_scale with the supersample factor on the target. */
static inline int psx_resolve_internal_scale_ss(int value, int ref_lines,
                                                int display_px_h, int s_max,
                                                int ss_milli) {
    ss_milli = psx_ss_milli_clamp(ss_milli);
    value = psx_ir_ceiling(value);
    if (ss_milli != PSX_SS_MILLI_MIN) {
        if (value == PSX_IR_DISPLAY) {
            if (display_px_h > 0)
                display_px_h = (int)(((long long)display_px_h * ss_milli + 999) / 1000);
        } else if (value >= PSX_IR_MIN_LINES) {
            long long t = ((long long)value * ss_milli + 999) / 1000;
            value = t > 4LL * PSX_IR_MAX_LINES ? 4 * PSX_IR_MAX_LINES : (int)t;
        }
    }
    return psx_resolve_internal_scale(value, ref_lines, display_px_h, s_max);
}

/* A Match display cap bounds the final target, including supersampling.
 * Fixed presets and the legacy integer scale retain their own ceilings. */
static inline int psx_resolve_internal_scale_ss_cap(int value, int ref_lines,
        int display_px_h, int s_max, int ss_milli, int max_display_lines) {
    int s = psx_resolve_internal_scale_ss(value, ref_lines, display_px_h, s_max, ss_milli);
    if (psx_ir_ceiling(value) == PSX_IR_DISPLAY && max_display_lines > 0) {
        int cap = psx_resolve_internal_scale(max_display_lines, ref_lines, 0, s_max);
        if (s > cap) s = cap;
    }
    return s;
}

/* ---- Dynamic resolution floor ([video] dynamic_resolution_min) -------------
 * A preset or line count resolves as usual. "display" (PSX_IR_DISPLAY) is the
 * output's own lines: the selected internal resolution WITHOUT the supersample
 * factor (Match display = the monitor's pixel height). With supersample > 1
 * the controller then trades only supersampling and never drops below the
 * output. Clamped to [1, ceiling]. */
static inline int psx_dynres_floor_scale(int min_value, int selected_ir, int ref_lines,
                                         int display_px_h, int ceiling) {
    const int v = (min_value == PSX_IR_DISPLAY && selected_ir != PSX_IR_UNSET)
                  ? psx_ir_ceiling(selected_ir) : min_value;
    int s = psx_resolve_internal_scale(v, ref_lines, display_px_h, ceiling);
    return s > ceiling ? ceiling : s;
}
