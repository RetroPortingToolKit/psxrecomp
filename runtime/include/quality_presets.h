#pragma once
/* quality_presets.h -- graphics presets: detection, persistence and the
 * player's override (docs/QUALITY_PRESETS.md). C++: it works on the
 * recompiler's config types. */
#include "quality_tier.h"

#include <functional>
#include <string>
#include <vector>

namespace PSXRecompV4 { struct UserSettings; struct GameConfig; }
struct PsxHostVideoSettings;

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

/* What the player did in a settings editor (boot launcher, soft-return
 * launcher, or the in-game menu's live settings), as the frontend reports it.
 * Both launchers report it through Settings.quality_preset: a tier picked =
 * PICK, 5 (Custom) after a governed row changed = GOVERNED_EDIT. The live
 * path compares the governed values it applies with the ones in force. */
enum class Edit { NONE, GOVERNED_EDIT, PICK };

/* The one preset/key-ownership transaction every settings.toml save that may
 * carry governed keys goes through. `save` is what the editor wants to write
 * (it may have been built from an empty seed); `disk` is settings.toml as it
 * is now. On return `save` holds the governed keys and quality_* metadata
 * that may be written, and `run` this run's state:
 *  - a one-run PSX_QUALITY override never persists: the governed keys and
 *    quality_* keys are restored from disk (so the previous preset or Custom
 *    values survive), even after a governed edit, which applies this run only;
 *  - PICK (not under an override): the picked preset is recorded; it owns its
 *    keys, so their saved values are kept as they are on disk;
 *  - GOVERNED_EDIT, or Custom already: Custom from the current base, with the
 *    edited governed values, so the next launch keeps them;
 *  - NONE with a preset in force: governed keys and quality_* keys as on disk.
 * Unrelated keys in `save` are never touched. Returns true if the state in
 * `run` changed (for the caller's log line). */
bool settle_save(PSXRecompV4::UserSettings& save, const PSXRecompV4::UserSettings& disk,
                 Decision& run, Edit edit, int picked,
                 const std::vector<std::string>& keys);

/* The live path's classification: GOVERNED_EDIT when a value the in-game
 * menu changes (cur -> next) belongs to a governed key, else NONE. */
Edit live_edit(const PsxHostVideoSettings& cur, const PsxHostVideoSettings& next,
               const std::vector<std::string>& keys);

/* Host-side launcher classification. The frontend reports Custom when any
 * row its snapshot tracks changed; only rows of keys this title governs
 * count. row_changed(key) says whether that key's row differs from what the
 * frontend's base preset puts there. Returns GOVERNED_EDIT, PICK (*picked =
 * base) when the base is a new tier and only unrelated rows changed, or NONE. */
Edit launcher_custom_edit(const std::vector<std::string>& keys, int base, int current,
                          const std::function<bool(const std::string&)>& row_changed,
                          int* picked);

/* Copy one governed key's settings.toml field(s) (value and presence). */
void copy_key(PSXRecompV4::UserSettings& dst, const PSXRecompV4::UserSettings& src,
              const std::string& key);

}  // namespace psxq
