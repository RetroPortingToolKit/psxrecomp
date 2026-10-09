/* quality_presets.cpp -- graphics presets (quality_presets.h,
 * docs/QUALITY_PRESETS.md).
 *
 * A title offers presets as [quality.low/medium/high/ultra] tables of [video]
 * keys. On first launch, and whenever the machine's fingerprint changes while
 * a preset (not Custom) is in force, the host is detected and the matching
 * preset is chosen and saved to settings.toml. A player who changes any
 * setting a preset governs turns the state into Custom (the launcher does
 * this); Custom is never overridden again, only re-detected on request. */
#include "quality_presets.h"

#include "config_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace psxq {

unsigned offered_mask(const PSXRecompV4::GameConfig& gc) {
    unsigned m = 0;
    for (const auto& p : gc.quality_presets) {
        const int t = psx_quality_from_name(p.name.c_str());
        if (t >= 0 && t < PSX_QUALITY_COUNT) m |= 1u << t;
    }
    return m;
}

std::vector<std::string> governed_keys(const PSXRecompV4::GameConfig& gc) {
    std::vector<std::string> keys;
    for (const auto& p : gc.quality_presets)
        for (const auto& k : p.keys)
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    return keys;
}

Decision decide(unsigned offered, const PSXRecompV4::UserSettings& us,
                PsxHostInfo host, const char* env, bool redetect,
                const std::function<int(char*, size_t)>& gl_probe, bool no_detect) {
    Decision d;
    if (!(offered & 0xFu)) return d;          /* no presets: nothing changes */
    char fp[17];
    psx_quality_fingerprint(&host, fp);       /* OS facts only: stable per launch */
    d.fingerprint = fp;

    const int saved = us.has_quality_preset
        ? psx_quality_from_name(us.quality_preset.c_str()) : PSX_QUALITY_NONE;
    const bool same_hw = us.has_quality_hardware && us.quality_hardware == d.fingerprint;
    const bool due = redetect || saved == PSX_QUALITY_NONE ||
                     (saved != PSX_QUALITY_CUSTOM && !same_hw);

    if (due && no_detect) {
        /* Headless and hidden-window runs (CI, scripted tests) never detect
         * and never write settings.toml: a saved preset still applies, else
         * the title's [video] block does. */
        d.detection_skipped = true;
        if (saved >= 0 && saved < PSX_QUALITY_COUNT)
            d.preset = d.base = psx_quality_pick_offered(saved, offered);
    } else if (due) {
        /* The GPU OpenGL will actually use: a hybrid laptop or a desktop
         * with an iGPU may name another adapter in the OS. The OS name (or
         * the Linux PCI id) is only a fallback, and a result reached without
         * the GL probe is used for this run but not saved, so the next
         * launch probes again. */
        bool probed = false;
        if (gl_probe) {
            char r[sizeof host.gpu];
            if (gl_probe(r, sizeof r) && r[0]) {
                std::snprintf(host.gpu, sizeof host.gpu, "%s", r);
                probed = true;
            }
        }
        char reason[192];
        d.detected = psx_quality_classify(&host, reason, sizeof reason);
        d.reason = reason;
        d.detected_now = true;
        d.gl_probed = probed;
        d.persist = probed;
        d.preset = d.base = psx_quality_pick_offered(d.detected, offered);
    } else if (saved == PSX_QUALITY_CUSTOM) {
        d.preset = PSX_QUALITY_CUSTOM;
        int base = us.has_quality_base ? psx_quality_from_name(us.quality_base.c_str())
                                       : PSX_QUALITY_NONE;
        if (base < 0 || base >= PSX_QUALITY_COUNT) {
            const int det = us.has_quality_detected
                ? psx_quality_from_name(us.quality_detected.c_str()) : PSX_QUALITY_NONE;
            base = (det >= 0 && det < PSX_QUALITY_COUNT) ? det : PSX_QUALITY_ULTRA;
        }
        d.base = psx_quality_pick_offered(base, offered);
        d.hardware_changed_custom = !same_hw;
    } else {
        d.preset = d.base = psx_quality_pick_offered(saved, offered);
        /* The title dropped the saved preset: save the substitute. */
        d.persist = d.preset != saved;
    }
    if (!d.detected_now && us.has_quality_detected)
        d.detected = psx_quality_from_name(us.quality_detected.c_str());

    char sum[256];
    psx_quality_summary(&host, sum, sizeof sum);
    d.summary = sum;

    if (env && env[0]) {
        const int t = psx_quality_from_name(env);
        if (t >= 0 && t < PSX_QUALITY_COUNT) {
            d.preset = d.base = psx_quality_pick_offered(t, offered);
            d.env_override = true;
            d.persist = false;               /* one run: never written */
        }
    }
    return d;
}

void mask_user_settings(PSXRecompV4::UserSettings& us,
                        const std::vector<std::string>& keys) {
    for (const std::string& k : keys) {
        if (k == "internal_resolution" || k == "supersampling") {
            us.has_internal_resolution = false;
            us.has_supersampling = false;
        } else if (k == "dynamic_resolution") {
            us.has_dynamic_resolution = false;
        } else if (k == "dynamic_resolution_min") {
            us.has_dynamic_resolution_min = false;
        } else if (k == "frame_generation") {
            us.has_frame_generation = false;
        } else if (k == "render_thread") {
            us.has_render_thread = false;
        } else if (k == "present_thread") {
            us.has_present_thread = false;
        } else if (k == "texture_filtering") {
            us.has_texture_filter = false;
        } else if (k == "fmv_filter") {
            us.has_fmv_filter = false;
        } else if (k == "antialiasing") {
            us.has_antialiasing = false;
        } else if (k == "geometry_correction") {
            us.has_geometry_correction = false;
        } else if (k == "perspective_texturing") {
            us.has_perspective_texturing = false;
        } else if (k == "frame_interpolation") {
            us.has_frame_interpolation = false;
        } else if (k == "frame_interpolation_fps") {
            us.has_frame_interpolation_fps = false;
        }
        /* Every other [video] key has no settings.toml form: the preset's
         * value already stands. */
    }
}

void record(PSXRecompV4::UserSettings& us, const Decision& d) {
    if (d.preset == PSX_QUALITY_NONE) return;
    us.quality_preset = psx_quality_name(d.preset);
    us.has_quality_preset = true;
    if (d.base >= 0 && d.base < PSX_QUALITY_COUNT) {
        us.quality_base = psx_quality_name(d.base);
        us.has_quality_base = true;
    }
    if (!d.fingerprint.empty() && (d.detected_now || !us.has_quality_hardware)) {
        us.quality_hardware = d.fingerprint;
        us.has_quality_hardware = true;
    }
    if (d.detected >= 0 && d.detected < PSX_QUALITY_COUNT) {
        us.quality_detected = psx_quality_name(d.detected);
        us.has_quality_detected = true;
    }
}

}  // namespace psxq
