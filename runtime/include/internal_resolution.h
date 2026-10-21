/* internal_resolution.h — the "Internal resolution" setting: presets, their
 * persisted ids, and the integer scale each one asks the renderer for.
 *
 * Pure (no SDL, no GL) so both the config loader and a test can use it.
 *
 * VALUE ENCODING (shared with recomp-ui's Settings.internal_resolution):
 *   0   unset: the legacy [video] supersampling factor stands
 *   1   native (1x)
 *  -1   match display: the target is the monitor's pixel height
 *   N   (N >= 2) a target output height in lines (720, 1080, ... or a
 *       legacy "Nx" entry expressed as reference_lines * N)
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
#define PSX_IR_DEFAULT_REF_LINES 240
/* Custom line counts accepted from config (a 16K panel is 8640 lines). */
#define PSX_IR_MIN_LINES 2
#define PSX_IR_MAX_LINES 8640

typedef struct PsxIrPreset {
    const char *id;     /* stable id persisted in settings.toml */
    const char *label;  /* launcher label */
    int         value;  /* encoding above */
} PsxIrPreset;

static const PsxIrPreset k_psx_ir_presets[] = {
    { "native",  "Native",        PSX_IR_NATIVE  },
    { "720p",    "720p",          720            },
    { "1080p",   "1080p",         1080           },
    { "1440p",   "1440p",         1440           },
    { "4k",      "4K",            2160           },
    { "5k",      "5K",            2880           },
    { "8k",      "8K",            4320           },
    { "display", "Match display", PSX_IR_DISPLAY },
};
#define PSX_IR_PRESET_COUNT ((int)(sizeof(k_psx_ir_presets) / sizeof(k_psx_ir_presets[0])))

static inline int psx_ir_lower_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        if (ca != *b) return 0;
    }
    return *a == 0 && *b == 0;
}

/* 1 if value is a legal encoding (unset excluded). */
static inline int psx_ir_value_valid(int value) {
    return value == PSX_IR_NATIVE || value == PSX_IR_DISPLAY ||
           (value >= PSX_IR_MIN_LINES && value <= PSX_IR_MAX_LINES);
}

/* Parse a persisted value: a preset id (case-insensitive) or a decimal line
 * count in [PSX_IR_MIN_LINES, PSX_IR_MAX_LINES]. Returns 1 and stores the
 * value on success; 0 for anything else (the caller keeps its default). */
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
    if (ref_lines < 1) ref_lines = PSX_IR_DEFAULT_REF_LINES;
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
    if (value == PSX_IR_DISPLAY && max_display_lines > 0) {
        int cap = psx_resolve_internal_scale(max_display_lines, ref_lines, 0, s_max);
        if (s > cap) s = cap;
    }
    return s;
}

/* A legacy [video] supersampling factor as a value the launcher can show:
 * the preset that resolves to the same scale (1 -> Native, 3 -> 720p,
 * 5 -> 1080p, 6 -> 1440p, 9 -> 4K, 12 -> 5K, 18 -> 8K at 240 lines),
 * otherwise ref_lines * n as a custom entry ("2x (480 lines)"). */
static inline int psx_ir_from_supersampling(int n, int ref_lines) {
    if (ref_lines < 1) ref_lines = PSX_IR_DEFAULT_REF_LINES;
    if (n <= 1) return PSX_IR_NATIVE;
    for (int i = 0; i < PSX_IR_PRESET_COUNT; i++) {
        int v = k_psx_ir_presets[i].value;
        if (v >= PSX_IR_MIN_LINES && psx_ir_scale_for(v, ref_lines, 0) == n) return v;
    }
    long lines = (long)ref_lines * n;
    return lines > PSX_IR_MAX_LINES ? PSX_IR_MAX_LINES : (int)lines;
}

/* ---- The launcher round trip -------------------------------------------------
 * The host seeds the launcher with its video state, the player may change it,
 * and the host adopts what comes back. A recomp-ui built with the Internal
 * resolution row (RECOMP_LAUNCHER_HAS_INTERNAL_RESOLUTION) owns the preset.
 * An older one only has the legacy Supersampling row, a 1x..4x cycle that
 * clamps whatever it is seeded with. There a pick in that row must win over
 * any preset, and a preset the player cannot see (game.toml, a hand-edited
 * settings.toml) must survive an untouched row. With no preset the round trip
 * is exactly the historical one: seed the factor, take the factor back. */
#define PSX_IR_LEGACY_SS_MAX 4   /* the legacy row's range, and settings.toml's
                                  * supersampling for an older runtime */

/* The value seeded into the legacy Supersampling row. has_row: the launcher
 * shows the Internal resolution row instead (the factor passes through). */
static inline int psx_ir_launcher_seed_supersampling(int has_row, int preset,
                                                     int supersampling,
                                                     int ref_lines,
                                                     int display_px_h) {
    if (has_row || preset == PSX_IR_UNSET) return supersampling;
    /* The preset's scale, as near as the row can show it. */
    return psx_resolve_internal_scale(preset, ref_lines, display_px_h,
                                      PSX_IR_LEGACY_SS_MAX);
}

typedef struct PsxIrAdopted {
    int preset;   /* the preset kept (PSX_IR_UNSET: the factor stands) */
    int scale;    /* the scale to request */
    int save_ss;  /* settings.toml supersampling */
    int save_ir;  /* settings.toml internal_resolution; PSX_IR_UNSET omits it */
} PsxIrAdopted;

/* What the host adopts when the launcher returns.
 *   has_row       the launcher showed the Internal resolution row
 *   preset        the host's preset when it seeded the launcher
 *   ss_seed       what psx_ir_launcher_seed_supersampling returned
 *   ss_result     the Supersampling row's value on return
 *   ir_result     the Internal resolution row's value (has_row only)
 *   s_max         the chosen renderer's ceiling */
static inline PsxIrAdopted psx_ir_adopt_launcher(int has_row, int preset,
                                                 int ss_seed, int ss_result,
                                                 int ir_result, int ref_lines,
                                                 int display_px_h, int s_max) {
    PsxIrAdopted a;
    if (has_row) {
        if (psx_ir_value_valid(ir_result)) a.preset = ir_result;
        else if (preset != PSX_IR_UNSET) a.preset = preset;
        else a.preset = psx_ir_from_supersampling(ss_seed < 1 ? 1 : ss_seed, ref_lines);
    } else {
        int shown = ss_seed < 1 ? 1 : ss_seed > PSX_IR_LEGACY_SS_MAX
                                                ? PSX_IR_LEGACY_SS_MAX : ss_seed;
        a.preset = (preset != PSX_IR_UNSET && ss_result == shown) ? preset
                                                                  : PSX_IR_UNSET;
    }
    if (a.preset == PSX_IR_UNSET) {
        a.scale = ss_result;
        a.save_ss = ss_result;
        a.save_ir = PSX_IR_UNSET;
    } else {
        a.scale = psx_resolve_internal_scale(a.preset, ref_lines, display_px_h, s_max);
        /* An older runtime reading settings.toml only knows supersampling
         * (1..4): leave it the nearest it can do. */
        a.save_ss = a.scale < PSX_IR_LEGACY_SS_MAX ? a.scale : PSX_IR_LEGACY_SS_MAX;
        a.save_ir = a.preset;
    }
    return a;
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
                  ? selected_ir : min_value;
    int s = psx_resolve_internal_scale(v, ref_lines, display_px_h, ceiling);
    return s > ceiling ? ceiling : s;
}
