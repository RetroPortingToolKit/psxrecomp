#pragma once
/* quality_presets.h -- graphics presets: detection, persistence and the
 * player's override (docs/QUALITY_PRESETS.md). C++: it works on the
 * recompiler's config types. */
#include "quality_tier.h"

#include <functional>
#include <string>
#include <vector>

namespace PSXRecompV4 { struct UserSettings; struct GameConfig; }

namespace psxq {

struct Decision {
    int  preset   = PSX_QUALITY_NONE;  /* tier, CUSTOM, or NONE (title has no presets) */
    int  base     = PSX_QUALITY_NONE;  /* the preset whose values apply (CUSTOM's start) */
    int  detected = PSX_QUALITY_NONE;  /* this run's detection, or the saved one */
    bool detected_now = false;         /* a detection ran (first launch / new hardware) */
    bool persist  = false;             /* write the quality_* keys to settings.toml */
    bool env_override = false;         /* PSX_QUALITY chose this run's preset */
    bool hardware_changed_custom = false; /* new hardware, Custom kept untouched */
    bool detection_skipped = false;    /* headless / hidden run: no detection */
    bool gl_probed = false;            /* the detection read GL_RENDERER */
    std::string fingerprint, summary, reason;
};

/* Bit i set = the title offers tier i. */
unsigned offered_mask(const PSXRecompV4::GameConfig& gc);
/* Every [video] key any preset sets: the keys a preset governs. */
std::vector<std::string> governed_keys(const PSXRecompV4::GameConfig& gc);

/* The policy, with the host already probed. gl_probe reads GL_RENDERER when
 * a detection is due; only a detection it succeeded for is saved. env is
 * PSX_QUALITY ("low".."ultra"; one run, never saved); redetect forces a
 * detection; no_detect (headless / hidden window) never detects or saves. */
Decision decide(unsigned offered, const PSXRecompV4::UserSettings& us,
                PsxHostInfo host, const char* env, bool redetect,
                const std::function<int(char*, size_t)>& gl_probe,
                bool no_detect = false);

/* While a preset (not Custom) is in force the preset owns its keys: drop the
 * player's saved values for them so settings.toml cannot override it. */
void mask_user_settings(PSXRecompV4::UserSettings& us,
                        const std::vector<std::string>& keys);

/* Store a decision's quality_* keys in us (for save_user_settings). */
void record(PSXRecompV4::UserSettings& us, const Decision& d);

}  // namespace psxq
