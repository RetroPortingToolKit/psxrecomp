#pragma once

#include "mod_plugins.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace PSXRecompV4 {

/* How finished a feature is, and therefore who is allowed to see it.
 *
 * Stable ships and is offered to everyone. Experimental ships, badged, and is
 * default-off: it works, but has not been validated on this title. Developer
 * does not ship at all -- contributors reach those features by cloning the
 * repo and building locally.
 *
 * This lives on the FEATURE, not the package. A package is the trust and
 * installation boundary; how finished one of its features is has nothing to do
 * with that, and tying the two forced a whole catalog down to the maturity of
 * its least finished feature. A package may still declare a channel, which its
 * features inherit unless they state their own. */
enum class ModChannel {
    Stable = 0,
    Experimental = 1,
    Developer = 2,
};

/* Whether this build may see developer-channel features. A local build may; a
 * CI/release build may not, which is what "developer does not ship" means in
 * practice. runtime.cmake defines PSX_MOD_DEVELOPER_CHANNEL for local builds. */
#if defined(PSX_MOD_DEVELOPER_CHANNEL) && PSX_MOD_DEVELOPER_CHANNEL
inline constexpr bool kDeveloperChannelDefault = true;
#else
inline constexpr bool kDeveloperChannelDefault = false;
#endif

const char* mod_channel_name(ModChannel channel);
bool mod_channel_from_name(const std::string& name, ModChannel& out);

enum class ModOptionType {
    Boolean,
    Choice,
    Integer,
};

struct ModChoice {
    std::string value;
    std::string label;
};

struct ModOption {
    std::string feature_id;
    std::string id;
    std::string label;
    std::string description;
    std::string group;
    ModOptionType type = ModOptionType::Boolean;
    std::string default_value;
    int64_t min_value = 0;
    int64_t max_value = 0;
    int64_t step = 1;
    std::vector<ModChoice> choices;
    /* Optional id of a BOOLEAN option in the same feature that overrides this
     * one. While that option is true this control is inert: the launcher greys
     * it out and plugins ignore its value. Models "tick Instant and the speed
     * box stops mattering" without inventing a second widget type. */
    std::string disabled_by;
};

struct ModFeature {
    std::string id;
    std::string name;
    std::string author;
    std::string description;
    std::string group = "General";
    ModChannel channel = ModChannel::Stable;
    bool default_enabled = false;
    bool hidden = false;
    bool legacy = false;
};

enum class ModConstraintKind {
    OrderedInteger,
    RequiresFeature,
};

enum class ModConstraintDirection {
    Nondecreasing,
    Nonincreasing,
};

struct ModConstraint {
    std::string feature_id;
    ModConstraintKind kind = ModConstraintKind::OrderedInteger;
    ModConstraintDirection direction =
        ModConstraintDirection::Nondecreasing;
    std::vector<std::string> options;
    std::string required_feature_id;
    std::string required_option_id;
    std::string required_value;
};

struct ModRequirement {
    std::string id;
    std::string version;
};

/* A feature of this package that needs a feature of ANOTHER package while a
 * condition on its own options holds (manifest [[requirement]], format 7).
 *
 * Package-level [[dependency]] is unconditional, and an in-package
 * requires_feature constraint cannot name another package. This is the
 * missing shape: "Extras = Full needs psx.enhancement.8mb-ram/8mb-ram". The
 * required feature is DERIVED for the session -- resolve() activates it even
 * when it is hidden or disabled in state.toml -- and is never written to
 * state.toml, because the player did not choose it. A requirement that cannot
 * be met (package absent, excluded, wrong version, feature missing) fails the
 * plan loudly; it never runs the requiring selection without it. */
struct ModFeatureRequirement {
    std::string feature_id;           /* the requiring feature (owner) */
    std::string package_id;           /* the package that provides it */
    std::string version = "*";        /* same range syntax as [[dependency]] */
    std::string required_feature_id;  /* feature of package_id to activate */
    std::map<std::string, std::string> when;  /* owner's option conditions */
};

struct ModTarget {
    std::string game_id;
    std::string exe_sha256;
    std::string disc_sha256;
};

enum class ModPatchTarget {
    MainExe,
    DiscRaw,
    DiscUser,
};

enum class ModValueEncoding {
    U8,
    U16LE,
    U32LE,
    MipsLuiOriU32,
};

enum class ModIntegerPredicateOp {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

struct ModIntegerPredicate {
    bool present = false;
    std::string option;
    ModIntegerPredicateOp op = ModIntegerPredicateOp::Equal;
    int64_t value = 0;
};

struct ModPatchField {
    uint64_t offset = 0;
    std::vector<uint8_t> replacement;
    std::string replace_from_option;
    ModValueEncoding replace_encoding = ModValueEncoding::U8;
    int64_t replace_addend = 0;
};

struct ModPatch {
    std::string feature_id;
    ModPatchTarget target = ModPatchTarget::MainExe;
    uint64_t location = 0; /* guest address or canonical disc-stream byte offset */
    std::vector<uint8_t> expected;
    std::vector<uint8_t> replacement;
    std::string replace_from_option;
    ModValueEncoding replace_encoding = ModValueEncoding::U8;
    uint64_t replace_offset = 0;
    int64_t replace_addend = 0;
    bool replace_omit_when_default = false;
    std::string when_option;
    std::string when_value;
    std::map<std::string, std::string> when;
    ModIntegerPredicate when_integer;
    std::vector<ModPatchField> fields;
    int64_t order = 0;
};

struct ModOverlay {
    std::string feature_id;
    ModPatchTarget target = ModPatchTarget::DiscRaw;
    uint64_t location = 0;
    std::filesystem::path file;
    std::string sha256;
    std::string expected_sha256;
    uint64_t size = 0;
    std::map<std::string, std::string> when;
    struct FeaturePredicate {
        bool present = false;
        std::string package_id;
        std::string feature_id;
        bool enabled = true;
    } when_feature;
    int64_t order = 0;
};

struct ModPlugin {
    std::string feature_id;
    std::string id;
    std::map<std::string, std::string> when;
    int64_t order = 0;
    /* [[plugin]] netplay = "local_view": the plugin only changes what this
     * player sees. In a netplay match it stays on, per player, but its hooks
     * run only inside the sandboxed own-view render
     * (psx_mod_render_local_view), never in the shared simulation. */
    bool netplay_local_view = false;
    /* netplay = "input": the plugin only transforms this player's own pad
     * before it is staged (psx_mod_set_pad_transform). Online it stays on per
     * player: the post-transform pad is what every peer simulates. */
    bool netplay_input = false;
    /* netplay = "host_output": the plugin only reads guest state and drives
     * host output (rumble, haptics). Online it stays on per player and its
     * hooks run in the shared simulation, but every guest write it attempts
     * is refused there, so it can never diverge the peers. */
    bool netplay_host_output = false;
};

struct ModResource {
    std::string feature_id;
    std::string id;
    std::string label;
    std::string description;
    std::string file_patterns;
    std::string file_description;
    std::string format = "file";
    bool required = false;
    uint64_t size = 0;
    std::string sha256;
    // Source inputs are launcher configuration, not guest resources. A shared
    // key stores one path across packages; derived resources never get pickers.
    std::string shared_source;
    bool input_only = false;
    bool hidden = false;
    // Package-relative path used when the player has not selected one (for
    // example a pack folder the package ships). A selection always wins.
    std::string default_path;
};

struct ModDerivedDisc {
    std::string kind = "vcdiff";
    std::filesystem::path patch;
    std::string patch_sha256;
    uint64_t output_size = 0;
    std::string output_sha256;
    std::string when_option;
    std::string when_value;
    std::map<std::string, std::string> when;
};

struct ModAuthorLink {
    std::string name;
    std::string url;
};

/* Where a package's files came from, and therefore who owns them on disk.
 *
 * Bundled packages are build output: the framework and the title stage them
 * into <exe>/mods/bundled, which every build wipes and re-stages. Nothing the
 * player owns may live there. Installed packages are written by the launcher
 * into <exe>/mods/installed and are never touched by a build.
 *
 * Before this split there was one <exe>/mods/packages tree serving both roles,
 * so a rebuild deleted every mod the player had installed. */
enum class ModPackageOrigin {
    Bundled,
    Installed,
};

struct ModPackage {
    uint32_t format_version = 0;
    std::string id;
    std::string version;
    std::string name;
    std::string author;
    std::vector<ModAuthorLink> author_links;
    std::string description;
    std::string license;
    std::string source_name;
    std::string source_url;
    std::string resolver = "declarative";
    std::string prepare;
    std::string save_compatibility = "shared";
    /* Default channel for features that do not declare their own. */
    ModChannel channel = ModChannel::Stable;
    ModPackageOrigin origin = ModPackageOrigin::Installed;
    /* Set when an installed package of the same id shadows a bundled one. */
    bool shadows_bundled = false;
    std::filesystem::path root;
    std::vector<ModTarget> targets;
    std::vector<ModRequirement> dependencies;
    std::vector<std::string> conflicts;
    std::vector<ModFeature> features;
    std::vector<ModOption> options;
    std::vector<ModConstraint> constraints;
    std::vector<ModFeatureRequirement> requirements;
    std::vector<ModPatch> patches;
    std::vector<ModOverlay> overlays;
    std::vector<ModPlugin> plugins;
    std::vector<ModResource> resources;
    std::vector<ModDerivedDisc> derived_discs;
};

struct ModPrepareContext {
    const ModPackage& package;
    std::string feature_id;
    std::filesystem::path disc_path;
    std::filesystem::path cache_root;
    std::filesystem::path application_dir;
    std::map<std::string, std::filesystem::path> inputs;
};
using ModMediaPreparer = std::function<bool(const ModPrepareContext&,
    std::map<std::string, std::filesystem::path>&, std::string&)>;
// Implementations are linked trusted code. A manifest cannot execute a command.
// Normally called synchronously during launch preparation. A title enabling
// PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE also permits preboot offline preparation
// on a launcher-owned worker. Every registered callback/service used by that
// title must then support serialized worker calls without SDL/UI/GL or
// main-thread affinity. Registration must finish before opening the launcher;
// activation callbacks still run separately, before renderer initialization.
bool mod_register_media_preparer(const std::string& id, ModMediaPreparer callback,
                                 ModMediaPreparer cache_probe = {});

struct ModFeatureSelection {
    bool enabled = false;
    bool has_enabled = false;
    std::map<std::string, std::string> values;
    std::map<std::string, std::string> resources;
};

struct ModSelection {
    /* v1 migration state. Feature-style manifests do not use these fields. */
    bool enabled = false;
    std::string version;
    std::map<std::string, std::string> values;
    std::map<std::string, ModFeatureSelection> features;
};

struct ModResolution {
    bool ok = false;
    std::string fingerprint;
    std::vector<const ModPackage*> ordered;
    struct Write {
        struct Field {
            uint64_t offset = 0;
            std::vector<uint8_t> replacement;
        };
        ModPatchTarget target = ModPatchTarget::MainExe;
        uint64_t location = 0;
        std::vector<uint8_t> expected;
        std::vector<uint8_t> replacement;
        std::vector<Field> fields;
        std::string package_id;
        std::string feature_id;
    };
    std::vector<Write> writes;
    struct Overlay {
        ModPatchTarget target = ModPatchTarget::DiscRaw;
        uint64_t location = 0;
        std::vector<uint8_t> payload;
        std::string payload_sha256;
        std::string expected_sha256;
        std::string package_id;
        std::string feature_id;
    };
    std::vector<Overlay> overlays;
    struct DerivedDisc {
        std::string kind;
        std::filesystem::path patch;
        std::string patch_sha256;
        uint64_t output_size = 0;
        std::string output_sha256;
        std::string package_id;
    };
    std::vector<DerivedDisc> derived_discs;
    struct Plugin {
        std::string id;
        std::string package_id;
        std::string feature_id;
        bool netplay_local_view = false;
        bool netplay_input = false;
        bool netplay_host_output = false;
    };
    std::vector<Plugin> plugins;
    struct Resource {
        std::string package_id;
        std::string feature_id;
        std::string id;
        std::filesystem::path path;
        std::string format;
        std::string sha256;
        std::shared_ptr<const std::vector<uint8_t>> bytes;
    };
    std::vector<Resource> resources;
    struct Diagnostic {
        std::string message;
        std::string resource;
        std::string package_id;
        std::string feature_id;
        std::string other_package_id;
        std::string other_feature_id;
    };
    std::vector<Diagnostic> diagnostics;
    /* Features the plan activates because an active [[requirement]] needs
     * them, not because state.toml enables them. One entry per derived
     * feature (the first requirement that activated it). */
    struct ImplicitFeature {
        std::string package_id;
        std::string feature_id;
        std::string required_by_package_id;
        std::string required_by_feature_id;
    };
    std::vector<ImplicitFeature> implicit_features;
    /* The selection the plan was built from: state.toml's selection plus the
     * implicit activations above. Plugins read option values from this, so a
     * committed plan and the values its plugins observe cannot disagree. */
    std::map<std::string, ModSelection> selections;
    std::vector<std::string> errors;
};

struct ModBuiltinResolverContext {
    const std::map<std::string, const ModPackage*>* active_packages = nullptr;
    const std::map<std::string, ModSelection>* selections = nullptr;
};

using ModBuiltinResolver = std::function<bool(
    const ModPackage& package,
    const ModSelection& selection,
    const ModBuiltinResolverContext& context,
    std::vector<ModResolution::Write>& writes,
    std::vector<std::string>& errors)>;

class ModPackageManager {
public:
    explicit ModPackageManager(std::filesystem::path mods_root = {});

    void set_root(std::filesystem::path mods_root);
    const std::filesystem::path& root() const { return root_; }
    /* Developer-channel features are stripped at scan and install time unless
     * this is set. Defaults to kDeveloperChannelDefault; tests set it
     * explicitly so they exercise both a local and a release build. */
    void set_developer_channel_visible(bool visible) {
        developer_channel_ = visible;
    }
    bool developer_channel_visible() const { return developer_channel_; }

    /* Build-owned catalog: wiped and re-staged by every build. */
    std::filesystem::path bundled_root() const { return root_ / "bundled"; }
    /* Launcher-owned catalog: a build never touches this tree. */
    std::filesystem::path installed_root() const { return root_ / "installed"; }

    bool scan(std::string* error = nullptr);
    bool load_state(std::string* error = nullptr);
    bool save_state(std::string* error = nullptr) const;
    bool prepare_resources(const std::string& game_id,
                           const std::filesystem::path& disc_path,
                           const std::filesystem::path& cache_root,
                           std::string* error = nullptr, bool cached_only = false);

    bool install_archive(const std::filesystem::path& archive,
                         std::string* installed_id = nullptr,
                         std::string* installed_version = nullptr,
                         std::string* error = nullptr);
    bool remove_version(const std::string& id, const std::string& version,
                        std::string* error = nullptr);

    bool set_enabled(const std::string& id, bool enabled, std::string* error = nullptr);
    bool select_version(const std::string& id, const std::string& version,
                        std::string* error = nullptr);
    bool set_option(const std::string& id, const std::string& option,
                    const std::string& value, std::string* error = nullptr);
    bool set_feature_enabled(const std::string& package_id,
                             const std::string& feature_id, bool enabled,
                             std::string* error = nullptr);
    bool set_feature_option(const std::string& package_id,
                            const std::string& feature_id,
                            const std::string& option_id,
                            const std::string& value,
                            std::string* error = nullptr);
    bool set_feature_resource_path(const std::string& package_id,
                                   const std::string& feature_id,
                                   const std::string& resource_id,
                                   const std::filesystem::path& path,
                                   std::string* error = nullptr);

    const std::map<std::string, std::map<std::string, ModPackage>>& packages() const {
        return packages_;
    }
    const std::map<std::string, ModSelection>& selections() const { return selections_; }
    /* Swap the in-memory selection for `next` and return the previous one.
     * Never saved by itself: a session that must not run the player's whole
     * choice (a netplay match) swaps a reduced selection in, resolves, and
     * swaps the player's back before anything can save_state(). */
    std::map<std::string, ModSelection> exchange_selections(
        std::map<std::string, ModSelection> next) {
        selections_.swap(next);
        return next;
    }
    const ModPackage* selected_package(const std::string& id) const;
    const ModFeature* selected_feature(const std::string& package_id,
                                       const std::string& feature_id) const;
    bool feature_enabled(const std::string& package_id,
                         const std::string& feature_id) const;
    std::string feature_option_value(const std::string& package_id,
                                     const std::string& feature_id,
                                     const std::string& option_id) const;
    std::filesystem::path feature_resource_path(
        const std::string& package_id,
        const std::string& feature_id,
        const std::string& resource_id) const;
    /* True when an active [[requirement]] activates this feature for the
     * session although state.toml does not enable it. feature_enabled() keeps
     * answering for the player's own choice, which is what the launcher shows
     * and what save_state() persists. */
    bool feature_implicitly_enabled(const std::string& package_id,
                                    const std::string& feature_id) const;
    /* An option value as a resolved plan sees it (plan.selections), for
     * plugins running under that plan. */
    std::string feature_option_value(const ModResolution& plan,
                                     const std::string& package_id,
                                     const std::string& feature_id,
                                     const std::string& option_id) const;

    // Defer prepared resources only for launcher diagnostics, never execution
    // or fingerprints. A commit prepares first, then resolves without deferral.
    ModResolution resolve(const std::string& game_id,
                          const std::string& exe_sha256 = {},
                          const std::string& disc_sha256 = {},
                          bool defer_prepared_resources = false) const;

    static bool read_manifest(const std::filesystem::path& path, ModPackage& out,
                              std::string* error = nullptr);

    /* Manifests that failed to parse during the last scan(), as
     * "<path>: <reason>". A package that cannot be read must never vanish
     * silently -- the launcher surfaces these so a mod author sees why their
     * package is absent from the list. */
    const std::vector<std::string>& scan_errors() const { return scan_errors_; }

    /* Package ids that mods/state.toml carries a selection for but that no
     * catalog root holds -- a package the player removed, a developer-only
     * package a release build stripped, or a framework builtin the title
     * excludes (EXCLUDE_BUILTIN_MODS). Such a selection is DORMANT: resolve()
     * never visits it, so it contributes nothing to the plan, and save_state()
     * keeps it verbatim so the choice returns if the package ever does. The
     * runtime names these once at startup instead of dropping them silently. */
    std::vector<std::string> dormant_selections() const;

private:
    /* One-time move of a pre-split <exe>/mods/packages tree into the two
     * owned roots. Anything the build also staged into bundled/ is dropped;
     * everything else is the player's and moves to installed/. */
    void migrate_legacy_root();
    bool scan_root(const std::filesystem::path& packages_root,
                   ModPackageOrigin origin, std::string* error);
    /* selections_ plus every feature an active [[requirement]] derives,
     * iterated to a fixed point (a derived feature may carry requirements of
     * its own). Unmet requirements are reported through errors/diagnostics.
     * Never touches selections_: derived activations are not player choices. */
    std::map<std::string, ModSelection> effective_selections(
        std::vector<ModResolution::ImplicitFeature>* implicit,
        std::vector<ModResolution::Diagnostic>* diagnostics,
        std::vector<std::string>* errors) const;

    std::filesystem::path root_;
    bool developer_channel_ = kDeveloperChannelDefault;
    std::vector<std::string> scan_errors_;
    std::map<std::string, std::map<std::string, ModPackage>> packages_;
    std::map<std::string, ModSelection> selections_;
    std::map<std::string, std::filesystem::path> sources_;
};

bool mod_register_builtin_resolver(const std::string& id, ModBuiltinResolver resolver);
void mod_clear_builtin_resolvers_for_tests();
bool mod_register_activation_plugin(const std::string& id, void (*callback)(void));
bool mod_register_vblank_plugin(const std::string& id, void (*callback)(void));
bool mod_register_savestate_plugin(const std::string& id, void (*callback)(void));
void mod_invoke_savestate_plugin(const std::string& id);
/* A function-entry hook is a trusted implementation like the others: manifests
 * select it by id, and it runs only while a resolved plan activates that id. */
bool mod_register_function_entry_plugin(const std::string& id, uint32_t address,
                                        PSXModFunctionEntryCallback callback);
bool mod_register_function_filter_plugin(const std::string& id, uint32_t address,
                                         PSXModFunctionFilterCallback callback);
bool mod_register_guest_function_plugin(const std::string& id, uint32_t address,
                                        PSXModFunctionEntryCallback callback);
bool mod_plugin_registered(const std::string& id);
void mod_invoke_activation_plugin(const std::string& id);
void mod_invoke_vblank_plugin(const std::string& id);
struct ModFunctionEntryHook {
    uint32_t address = 0;
    PSXModFunctionEntryCallback callback = nullptr;
    PSXModFunctionFilterCallback filter = nullptr;
};
/* Hooks one implementation registered, in registration order. mod_runtime
 * flattens these into an address table when the plan's plugins activate. */
std::vector<ModFunctionEntryHook> mod_function_entry_hooks(const std::string& id);
std::vector<ModFunctionEntryHook> mod_guest_functions(const std::string& id);
struct ModInstructionHook {
    uint32_t address, expected;
    PSXModFunctionEntryCallback callback;
};
bool mod_register_instruction_plugin(const std::string& id, uint32_t address,
                                      uint32_t expected, PSXModFunctionEntryCallback callback);
std::vector<ModInstructionHook> mod_instruction_hooks(const std::string& id);
void mod_clear_plugins_for_tests();

/* Every id a trusted implementation registered (activation, vblank or
 * function-entry), sorted. */
std::vector<std::string> mod_registered_plugin_ids();

/* Registered implementations that no manifest under `manifest_roots` declares
 * as a [[plugin]] id. The resolved plan activates plugins by manifest id only,
 * so such an implementation can never run: a hook registered under a sub-id
 * ("x.widescreen.hud") is silently dead while its package ("x.widescreen")
 * resolves cleanly. Roots are searched recursively for manifest.toml; pass the
 * AUTHORED trees (title mods/preloaded, framework mods/builtin) so developer
 * packages a release catalog strips still count as declarations. Manifests
 * that fail to parse are reported through `errors`. */
struct ModPluginAudit {
    std::vector<std::string> undeclared;   /* registered, never declared */
    std::vector<std::string> declared;     /* every manifest [[plugin]] id */
    std::vector<std::string> errors;
    size_t manifests = 0;
};
ModPluginAudit mod_audit_registered_plugins(
    const std::vector<std::filesystem::path>& manifest_roots);

} // namespace PSXRecompV4
