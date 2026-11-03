#ifndef PSXRECOMP_HOST_ENHANCEMENTS_H
#define PSXRECOMP_HOST_ENHANCEMENTS_H

/*
 * The framework's built-in host enhancement mods:
 *
 *   psx.enhancement.crt-filter       CRT Filter: screen model, scanlines and
 *                                    their strength
 *   psx.enhancement.image-filtering  Image Filtering: smoothing (the linear vs
 *                                    nearest present scale, [video]
 *                                    antialiasing), texture filtering and FMV
 *                                    filtering
 *   psx.enhancement.enhanced-audio   Enhanced Audio: the high-quality SPU mix
 *                                    ([audio] spu_hq)
 *   psx.feature.rewind               Rewind: the local rewind ring, its buffer
 *                                    depth and capture interval
 *   psx.enhancement.internal-resolution
 *                                    Internal Resolution: fixed 1x..32x or height
 *                                    ([video] internal_resolution), Auto
 *                                    (dynamic resolution) included
 *
 * These used to be player settings (settings.toml, the launcher's Display and
 * Audio cards, the in-game menu). The title's game.toml [video] / [audio] keys
 * stay its BASELINE: each enabled option replaces its part, and an option
 * left at "Game default" keeps the baseline value. Internal Resolution starts
 * enabled at fixed 4x; disabling it selects native resolution with dynamic
 * resolution off. The other mods leave the baseline unchanged when disabled
 * (docs/MOD_PACKAGES.md "Built-in host enhancements").
 *
 * Lifecycle, the psx.enhancement.pgxp lesson (pgxp_session.h):
 *
 *   reset_mod_owned_presentation()  -> psx_host_enh_reset()
 *   mod_runtime_activate_plugins()  -> each enabled mod RECORDS its request
 *   renderer setup (session_reboot) -> main.cpp resolves the baseline and the
 *                                      requests (psx_host_enh_resolve) and
 *                                      applies the result
 *
 * Activation only records: it runs before the renderer setup, which applies
 * the baseline and would otherwise undo whatever activation had applied.
 *
 * All five are host-only (their [[plugin]] declares netplay = "host_only"):
 * they change what this machine shows, plays or keeps, never guest execution,
 * so they stay each player's own in netplay and never enter the plan
 * fingerprint (docs/MOD_PACKAGES.md "Host-only plugins").
 *
 * Live option changes (the in-game menu's psx_mod_set_option_live): the CRT
 * Filter and Image Filtering plugins update their request and call the
 * applier main.cpp registered, which resolves again and pushes the result to
 * the renderer. The Internal Resolution plugin calls its own applier, which
 * can refuse (the backend cannot change scale live here, or a netplay match
 * holds the scale): the change then applies at the next start. Main thread
 * only, like the rest of the mod runtime.
 */

#include "internal_resolution.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An option at "Game default": keep the title's baseline value. */
#define PSX_HOST_ENH_DEFAULT (-1)

/* What the four mods can change, in the title's [video] / [audio] terms. */
typedef struct PsxHostLook {
    int screen_kind;            /* 0 raw, 1 crt, 2 composite, 3 trinitron */
    int scanlines;              /* 0/1 */
    int scanline_strength_pct;  /* 0..100 */
    int present_linear;         /* [video] antialiasing: linear present 0/1 */
    int texture_filter;         /* PSX_TEXTURE_FILTER_* (video_texture_filter.h) */
    int fmv_filter;             /* VIDEO_FMV_FILTER_* (config value) */
    int spu_hq;                 /* [audio] spu_hq: SPU float shadow 0/1 */
} PsxHostLook;

/* This session's requests: zero-initialized (reset) means no mod enabled. */
typedef struct PsxHostEnhRequests {
    int crt;                    /* CRT Filter enabled */
    int crt_screen_kind;        /* PSX_HOST_ENH_DEFAULT or 1..3 */
    int crt_scanlines;          /* PSX_HOST_ENH_DEFAULT, 0 or 1 */
    int crt_strength_pct;       /* 1..100; applies while crt_scanlines is 1 */
    int image;                  /* Image Filtering enabled */
    int image_smoothing;        /* PSX_HOST_ENH_DEFAULT, 0 sharp or 1 smooth */
    int image_texture_filter;   /* PSX_HOST_ENH_DEFAULT or PSX_TEXTURE_FILTER_* */
    int image_fmv_filter;       /* PSX_HOST_ENH_DEFAULT or VIDEO_FMV_FILTER_* */
    int audio;                  /* Enhanced Audio enabled */
    int rewind;                 /* Rewind enabled */
    int rewind_depth;           /* 50 / 100 / 150 / 200 */
    int rewind_interval;        /* 1 / 4 / 8 / 12 / 15 */
    int resolution;             /* Internal Resolution enabled */
    int resolution_value;       /* PSX_IR_UNSET (Game default) or a PSX_IR_*
                                 * value (internal_resolution.h), Auto included;
                                 * not PSX_HOST_ENH_DEFAULT, which is
                                 * PSX_IR_DISPLAY's encoding */
} PsxHostEnhRequests;

/* The session's internal resolution in main.cpp's terms: the value
 * (PSX_IR_UNSET: the legacy [video] supersampling factor stands) and whether
 * dynamic resolution steps under it. */
typedef struct PsxHostResolution {
    int internal_resolution;
    int dynamic_resolution;
} PsxHostResolution;

#define PSX_HOST_ENH_STRENGTH_DEFAULT 50
#define PSX_HOST_ENH_REWIND_DEPTH_DEFAULT 50
#define PSX_HOST_ENH_REWIND_INTERVAL_DEFAULT 15
#define PSX_HOST_ENH_RESOLUTION_DEFAULT PSX_IR_SCALE(4)

/* The effective look: the baseline, with every part an enabled mod names
 * replaced. Pure. */
static inline PsxHostLook psx_host_enh_resolve(const PsxHostLook* baseline,
                                               const PsxHostEnhRequests* req) {
    PsxHostLook out = *baseline;
    if (req->crt) {
        if (req->crt_screen_kind != PSX_HOST_ENH_DEFAULT)
            out.screen_kind = req->crt_screen_kind;
        if (req->crt_scanlines != PSX_HOST_ENH_DEFAULT) {
            out.scanlines = req->crt_scanlines ? 1 : 0;
            /* The strength belongs to the player's own scanlines; Game
             * default keeps the title's look, its strength included. */
            if (out.scanlines) out.scanline_strength_pct = req->crt_strength_pct;
        }
    }
    if (req->image) {
        if (req->image_smoothing != PSX_HOST_ENH_DEFAULT)
            out.present_linear = req->image_smoothing ? 1 : 0;
        if (req->image_texture_filter != PSX_HOST_ENH_DEFAULT)
            out.texture_filter = req->image_texture_filter;
        if (req->image_fmv_filter != PSX_HOST_ENH_DEFAULT)
            out.fmv_filter = req->image_fmv_filter;
    }
    if (req->audio) out.spu_hq = 1;
    return out;
}

/* The effective internal resolution: native when disabled, the saved title
 * baseline for Game default, or the enabled mod's fixed scale/height choice. Auto is
 * Match display with dynamic resolution on; any fixed choice turns it off.
 * Pure. */
static inline PsxHostResolution psx_host_enh_resolve_resolution(
        const PsxHostResolution* baseline, const PsxHostEnhRequests* req) {
    /* Off is the native reference; an explicitly saved Game default still
     * opts into the title baseline. Fresh enabled profiles select fixed 4x. */
    PsxHostResolution out = {PSX_IR_NATIVE, 0};
    if (req->resolution) {
        out = *baseline;
        if (req->resolution_value != PSX_IR_UNSET) {
            out.internal_resolution = req->resolution_value;
            out.dynamic_resolution = psx_ir_is_auto(req->resolution_value) ? 1 : 0;
        }
    }
    return out;
}

/* Session start, before plugin activation: forget every request. */
void psx_host_enh_reset(void);

/* Activation (and live option changes) of the built-in plugins. Arguments use
 * the encodings above; out-of-range values are clamped to the option default
 * by the parsers below before they get here. */
void psx_host_enh_request_crt(int screen_kind, int scanlines, int strength_pct);
void psx_host_enh_request_image(int smoothing, int texture_filter, int fmv_filter);
void psx_host_enh_request_audio(void);
void psx_host_enh_request_rewind(int depth, int interval);
void psx_host_enh_request_resolution(int value);

/* A copy of this session's requests. */
void psx_host_enh_requests(PsxHostEnhRequests* out);

/* main.cpp's live re-application (resolve + push to the renderer). Returns
 * psx_host_enh_apply_live()'s answer: 1 when a registered applier ran (the
 * change is visible now), 0 when none is registered yet (renderer setup has
 * not run; it will resolve the request itself). */
typedef void (*PsxHostEnhApplier)(void);
void psx_host_enh_set_live_applier(PsxHostEnhApplier fn);
int  psx_host_enh_apply_live(void);

/* The Internal Resolution plugin's live re-application. The applier returns 1
 * when the new scale took effect now, 0 when it waits for the next start
 * (no live-capable backend, or a netplay match). No applier registered: 0. */
typedef int (*PsxHostEnhResolutionApplier)(void);
void psx_host_enh_set_resolution_applier(PsxHostEnhResolutionApplier fn);
int  psx_host_enh_apply_resolution_live(void);

/* Manifest option text -> encoding. Unknown text gives the option's default
 * (Game default, or the documented number), never an error: a value the
 * catalog accepted is always one of these. */
int psx_host_enh_parse_screen_kind(const char* text);   /* default crt composite trinitron */
int psx_host_enh_parse_tristate(const char* text);      /* default on off */
int psx_host_enh_parse_strength(const char* text);      /* "1".."100" */
int psx_host_enh_parse_smoothing(const char* text);     /* default smooth sharp */
int psx_host_enh_parse_texture_filter(const char* text);/* default nearest bilinear xbr */
int psx_host_enh_parse_fmv_filter(const char* text);    /* default nearest bilinear sharp bicubic */
int psx_host_enh_parse_rewind_depth(const char* text);  /* "50" "100" "150" "200" */
int psx_host_enh_parse_rewind_interval(const char* text);/* "1" "4" "8" "12" "15" */
/* "default" -> PSX_IR_UNSET; fixed 1x..32x and legacy height ids are retained.
 * Missing/unknown option text uses the shared 4x default. */
int psx_host_enh_parse_internal_resolution(const char* text);

#ifdef __cplusplus
}
#endif

#endif
