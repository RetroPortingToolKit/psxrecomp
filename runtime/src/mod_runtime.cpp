#include "mod_runtime.h"
#include "host_launch_timing.h"
#include "cpu_state.h"

#include "disc_path.h"
#include "host_file_identity.h"
#include "iso_reader.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "psx_netplay.h"
#include "gpu.h"
#include "gpu_hd_textures.h"
#include "psx_memory.h"
#include "render_pass_projection.h"
#include "psx_sha256.h"
#include "cpu_state.h"
#include "psx_lobby_client.h"

extern "C" uint32_t host_launch_timing_snapshot(HostLaunchTimingEvent* out,
    uint32_t cap, uint64_t* total, uint64_t* overwritten) {
    return PSXRecompV4::host_launch_timing_ring().snapshot(out, cap, total, overwritten);
}

extern "C" const char* host_launch_timing_stage_name(uint32_t stage) {
    static const char* const names[] = {
        "provider_commit", "runtime_commit", "commit", "disc_hash",
        "prepare_resources", "media_provider", "resolve", "overlay_verify",
        "derived_disc", "save_state", "build_disc_index", "overlay_worker",
        "overlay_join", "plugin_activation", "plugin_callback",
        "provider_netplay_commit"
    };
    return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <optional>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" uint8_t psx_read_byte(uint32_t addr);
extern "C" void psx_write_byte(uint32_t addr, uint8_t value);
extern "C" uint16_t psx_read_half(uint32_t addr);
extern "C" void psx_write_half(uint32_t addr, uint16_t value);
extern "C" uint32_t psx_read_word(uint32_t addr);
extern "C" void psx_write_word(uint32_t addr, uint32_t value);
extern "C" uint32_t psx_mod_memory_alloc(uint32_t size, uint32_t alignment);
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t size,
                                                  uint32_t alignment);
extern "C" int psx_ws_x_margin(void);
extern "C" void gpu_ws_tag_hud_primitive(uint32_t primitive, int edge);
extern "C" void gpu_ws_tag_world_primitive(uint32_t primitive, int is_world);
extern "C" void gpu_ws_set_adaptive_backdrop_preload(int enabled);
extern "C" void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);
extern "C" int fntrace_is_game_started(void);

/* Declared in mod_plugins.h; see active_function_entry_hooks() below. */
uint32_t g_psx_mod_function_entry_hooks = 0;
uint32_t g_psx_mod_guest_functions = 0;
uint32_t g_psx_mod_instruction_hooks = 0;
/* Guest vblanks seen by the mod runtime: the plugin counters' time base. */
static uint64_t g_mod_vblanks = 0;

namespace PSXRecompV4 {
namespace {

struct RuntimeMods {
    struct DiscExtent { uint32_t lba, count; const uint8_t* data; };
    std::vector<DiscExtent> disc_extents;
    struct AudioTrack { uint32_t lba, count, source_lba; const uint8_t* data; };
    std::vector<AudioTrack> audio_tracks;
    uint32_t extent_start = 0;
    bool activating = false;
    ModPackageManager manager;
    ModResolution plan;
    ModResolution validation;
    std::map<uint32_t, std::vector<size_t>> raw_disc_index;
    std::map<uint32_t, std::vector<size_t>> user_disc_index;
    std::map<uint32_t, std::vector<size_t>> raw_overlay_index;
    std::map<uint32_t, std::vector<size_t>> user_overlay_index;
    std::string game_id;
    std::string error;
    std::string exe_sha256;
    std::string disc_sha256;
    std::filesystem::path disc_path;
    std::filesystem::path effective_disc_path;
    uint32_t entry_phys = 0;
    uint64_t preparation_revision = 1;
    uint64_t ticket_revision = 0;
    std::map<std::filesystem::path, std::string> ticket_files;
    bool ticket_ready = false;
    bool catalog_stamped = false;
    std::map<std::filesystem::path, std::string> loaded_catalog;
    bool cached_prepare = false;
    bool provider_preparing = false;
    bool initialized = false;
    bool main_applied = false;
    bool disc_enabled = false;
    bool disc_guard_failed = false;
    /* The plan is a netplay match's own-view set: its hooks run only inside
     * psx_mod_render_local_view, its vblank/savestate callbacks not at all. */
    bool netplay_view_plan = false;
    const ModResolution::Plugin* current_plugin = nullptr;
    CPUState* current_function_cpu = nullptr;
    bool current_function_finished = false;
};

RuntimeMods& state() {
    static RuntimeMods value;
    return value;
}

std::filesystem::path media_cache_root() {
    std::filesystem::path root;
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) root = local;
#else
    if (const char* cache = std::getenv("XDG_CACHE_HOME")) root = cache;
    else if (const char* home_dir = std::getenv("HOME")) root = std::filesystem::path(home_dir) / ".cache";
#endif
    if (root.empty()) root = std::filesystem::temp_directory_path();
    return root / "psxrecomp/imports";
}

bool dependency_files(RuntimeMods& s, const std::filesystem::path& disc,
                      std::map<std::filesystem::path, std::string>& files) {
    files.clear();
    auto add = [&](const std::filesystem::path& path) {
        if (path.empty()) return true;
        std::string stamp;
        if (!host_file_identity(path, stamp)) return false;
        files[path] = std::move(stamp);
        return true;
    };
    if (!add(disc) || !add(resolve_disc_path(disc).data)) return false;
    std::error_code ec;
    size_t entries = 0;
    for (std::filesystem::recursive_directory_iterator it(s.manager.root(), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (++entries > 4096) return false;
        if (it->is_regular_file(ec) && !add(it->path())) return false;
    }
    if (ec) return false;
    for (const auto& [id, versions] : s.manager.packages()) {
        (void)versions;
        const auto* package = s.manager.selected_package(id);
        if (!package) continue;
        for (const auto& resource : package->resources) {
            if (!s.manager.feature_enabled(id, resource.feature_id) &&
                !s.manager.feature_implicitly_enabled(id, resource.feature_id)) continue;
            const auto path = s.manager.feature_resource_path(id, resource.feature_id, resource.id);
            if (!add(path)) return false;
        }
    }
    return add(s.effective_disc_path);
}

bool catalog_files(RuntimeMods& s, std::map<std::filesystem::path, std::string>& files) {
    files.clear();
    std::error_code ec;
    if (!std::filesystem::exists(s.manager.root(), ec)) return !ec;
    size_t entries = 0;
    for (std::filesystem::recursive_directory_iterator it(s.manager.root(), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (++entries > 4096) return false;
        if (it->path().filename() != "manifest.toml") continue;
        std::string stamp;
        if (!host_file_identity(it->path(), stamp)) return false;
        files[it->path()] = std::move(stamp);
    }
    return !ec;
}

bool prepared_ticket_matches(const std::filesystem::path& disc) {
    auto& s = state();
    if (!s.ticket_ready || s.main_applied || !mod_runtime_session_plan_fp().empty() ||
        disc != s.disc_path || s.ticket_revision != s.preparation_revision) return false;
    std::map<std::filesystem::path, std::string> files;
    if (!dependency_files(s, disc, files) || files != s.ticket_files) return false;
    auto manager = s.manager;
    if (!manager.prepare_resources(s.game_id, disc, media_cache_root(), nullptr, true)) return false;
    for (const auto& [id, versions] : s.manager.packages()) {
        (void)versions;
        const auto* package = s.manager.selected_package(id);
        if (!package) continue;
        for (const auto& resource : package->resources)
            if (manager.feature_resource_path(id, resource.feature_id, resource.id) !=
                s.manager.feature_resource_path(id, resource.feature_id, resource.id)) return false;
    }
    return true;
}

class ScopedFlag {
    bool& flag;
    const bool previous;
public:
    explicit ScopedFlag(bool& value) : flag(value), previous(value) { flag = true; }
    ~ScopedFlag() { flag = previous; }
    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;
};

/* Guest calls made from an entry hook can deliver VBlank callbacks before
 * returning. Every callback owns its resource/completion context; restoring
 * only entry hooks would let VBlank erase the interrupted plugin or complete
 * a function that belongs to another callback. */
class PluginCallbackScope {
    RuntimeMods& runtime;
    const ModResolution::Plugin* previous_plugin;
    CPUState* previous_cpu;
    bool previous_finished;
public:
    PluginCallbackScope(RuntimeMods& s, const ModResolution::Plugin* plugin,
                        CPUState* cpu = nullptr)
        : runtime(s), previous_plugin(s.current_plugin),
          previous_cpu(s.current_function_cpu), previous_finished(s.current_function_finished) {
        s.current_plugin = plugin;
        s.current_function_cpu = cpu;
        s.current_function_finished = false;
    }
    ~PluginCallbackScope() {
        runtime.current_plugin = previous_plugin;
        runtime.current_function_cpu = previous_cpu;
        runtime.current_function_finished = previous_finished;
    }
    PluginCallbackScope(const PluginCallbackScope&) = delete;
    PluginCallbackScope& operator=(const PluginCallbackScope&) = delete;
};

/* Function-entry hooks of the ACTIVE plan, flattened at plugin activation into
 * one table sorted by code key (address with the segment bits stripped, so a
 * KUSEG or KSEG1 PC reaches the same hook as KSEG0). The interpreter consults
 * this on every entry it dispatches, so the lookup is a binary search over
 * integers, never a per-plugin string map, and an empty table short-circuits
 * in the caller via g_psx_mod_function_entry_hooks. Plugin pointers refer into
 * RuntimeMods::plan; every plan replacement clears the table first. */
struct ActiveFunctionEntryHook {
    uint32_t key = 0;
    PSXModFunctionEntryCallback callback = nullptr;
    PSXModFunctionFilterCallback filter = nullptr;
    const ModResolution::Plugin* plugin = nullptr;
};

std::vector<ActiveFunctionEntryHook>& active_function_entry_hooks() {
    static std::vector<ActiveFunctionEntryHook> value;
    return value;
}
unsigned function_entry_depth;

std::vector<ActiveFunctionEntryHook>& active_guest_functions() {
    static std::vector<ActiveFunctionEntryHook> value;
    return value;
}

struct ActiveInstructionHook {
    uint32_t key, expected;
    PSXModFunctionEntryCallback callback;
    const ModResolution::Plugin* plugin;
};
std::vector<ActiveInstructionHook>& active_instruction_hooks() {
    static std::vector<ActiveInstructionHook> value;
    return value;
}

inline uint32_t function_entry_key(uint32_t address) {
    return address & 0x1FFFFFFFu;
}

/* Game-owned netplay hooks are compiled into the executable. Keep them apart
 * from the user mod plan, which every online match clears, so clearing that
 * plan cannot disable the game's own netplay support. */
std::vector<ActiveFunctionEntryHook>& game_netplay_entry_hooks() {
    static std::vector<ActiveFunctionEntryHook> value;
    return value;
}

void update_function_entry_count() {
    g_psx_mod_function_entry_hooks = (uint32_t)(
        active_function_entry_hooks().size() + game_netplay_entry_hooks().size());
}

void clear_function_entry_hooks() {
    active_function_entry_hooks().clear();
    update_function_entry_count();
    active_guest_functions().clear();
    g_psx_mod_guest_functions = 0;
    active_instruction_hooks().clear();
    g_psx_mod_instruction_hooks = 0;
}

void build_function_entry_hooks(const RuntimeMods& s) {
    clear_function_entry_hooks();
    if (!s.initialized || !s.plan.ok) return;
    auto& table = active_function_entry_hooks();
    for (const ModResolution::Plugin& plugin : s.plan.plugins)
        for (const ModFunctionEntryHook& hook : mod_function_entry_hooks(plugin.id))
            table.push_back({function_entry_key(hook.address), hook.callback,
                             hook.filter, &plugin});
    /* Stable: hooks sharing an address keep plan (plugin order) order. */
    std::stable_sort(table.begin(), table.end(),
                     [](const ActiveFunctionEntryHook& a,
                        const ActiveFunctionEntryHook& b) { return a.key < b.key; });
    update_function_entry_count();
    auto& functions = active_guest_functions();
    for (const auto& plugin : s.plan.plugins)
        for (const auto& function : mod_guest_functions(plugin.id))
            functions.push_back({function.address, function.callback, nullptr, &plugin});
    std::sort(functions.begin(), functions.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    g_psx_mod_guest_functions = (uint32_t)functions.size();
    auto& instructions = active_instruction_hooks();
    for (const auto& plugin : s.plan.plugins)
        for (const auto& hook : mod_instruction_hooks(plugin.id))
            instructions.push_back({hook.address, hook.expected, hook.callback, &plugin});
    std::stable_sort(instructions.begin(), instructions.end(),
                    [](const auto& a, const auto& b) { return a.key < b.key; });
    g_psx_mod_instruction_hooks = (uint32_t)instructions.size();
}

const ModPackage* selected_package(const std::string& id) {
    return state().manager.selected_package(id);
}

bool package_has_enabled_feature(const ModPackage& package) {
    return std::any_of(
        package.features.begin(), package.features.end(),
        [&](const ModFeature& feature) {
            return state().manager.feature_enabled(package.id, feature.id);
        });
}

/* In use = enabled by the player OR activated by another feature's
 * [[requirement]]. Removal must respect both; the enabled checkbox shows only
 * the player's own choice. */
bool package_in_use(const ModPackage& package) {
    return std::any_of(
        package.features.begin(), package.features.end(),
        [&](const ModFeature& feature) {
            return state().manager.feature_enabled(package.id, feature.id) ||
                   state().manager.feature_implicitly_enabled(package.id,
                                                              feature.id);
        });
}

std::string selected_value(const ModPackage& package, const ModOption& option) {
    const auto selection = state().manager.selections().find(package.id);
    if (selection != state().manager.selections().end()) {
        const auto value = selection->second.values.find(option.id);
        if (value != selection->second.values.end()) return value->second;
    }
    return option.default_value;
}

/* Resolve the manifest's disabled_by link against the CURRENT selection: the
 * named boolean sibling being true makes this option inert. Both the launcher
 * (greys the control) and psx_mod_option_value (returns the default instead of
 * a stale value) go through this, so the UI and the plugins can never disagree
 * about whether a control counts. */
bool option_is_disabled(const ModPackage& package, const ModOption& option) {
    if (option.disabled_by.empty()) return false;
    for (const ModOption& other : package.options) {
        if (other.feature_id != option.feature_id ||
            other.id != option.disabled_by)
            continue;
        return selected_value(package, other) == "true";
    }
    return false;
}

void build_disc_index(RuntimeMods& s) {
    s.raw_disc_index.clear();
    s.user_disc_index.clear();
    s.raw_overlay_index.clear();
    s.user_overlay_index.clear();
    for (size_t i = 0; i < s.plan.writes.size(); ++i) {
        const ModResolution::Write& write = s.plan.writes[i];
        if (write.target == ModPatchTarget::DiscRaw)
            s.raw_disc_index[(uint32_t)(write.location / 2352)].push_back(i);
        else if (write.target == ModPatchTarget::DiscUser)
            s.user_disc_index[(uint32_t)(write.location / 2048)].push_back(i);
    }
    for (size_t i = 0; i < s.plan.overlays.size(); ++i) {
        const ModResolution::Overlay& overlay = s.plan.overlays[i];
        const uint64_t sector_size =
            overlay.target == ModPatchTarget::DiscRaw ? 2352 : 2048;
        auto& index = overlay.target == ModPatchTarget::DiscRaw
            ? s.raw_overlay_index : s.user_overlay_index;
        const uint64_t first = overlay.location / sector_size;
        const uint64_t last =
            (overlay.location + overlay.payload.size() - 1) / sector_size;
        for (uint64_t lba = first; lba <= last; ++lba)
            index[(uint32_t)lba].push_back(i);
    }
}

void set_error(const std::string& error) {
    state().error = error;
}

void apply_main_write(const ModResolution::Write& write) {
    if (write.fields.empty()) {
        for (size_t i = 0; i < write.replacement.size(); ++i)
            psx_host_write_byte((uint32_t)write.location + (uint32_t)i,
                                write.replacement[i]);
        dirty_ram_mark_executable_range(
            (uint32_t)write.location & 0x1FFFFFFFu,
            (uint32_t)write.replacement.size());
        return;
    }
    for (const ModResolution::Write::Field& field : write.fields) {
        for (size_t i = 0; i < field.replacement.size(); ++i)
            psx_host_write_byte(
                (uint32_t)write.location +
                    (uint32_t)field.offset + (uint32_t)i,
                field.replacement[i]);
        dirty_ram_mark_executable_range(
            ((uint32_t)write.location +
             (uint32_t)field.offset) & 0x1FFFFFFFu,
            (uint32_t)field.replacement.size());
    }
}

bool restored_main_matches_plan(const RuntimeMods& s, uint32_t& failed_at) {
    std::map<uint32_t, uint8_t> desired;
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        if (write.fields.empty()) {
            for (size_t i = 0; i < write.replacement.size(); ++i)
                desired[(uint32_t)write.location + (uint32_t)i] =
                    write.replacement[i];
        } else {
            for (const ModResolution::Write::Field& field : write.fields) {
                for (size_t i = 0; i < field.replacement.size(); ++i)
                    desired[
                        (uint32_t)write.location +
                        (uint32_t)field.offset + (uint32_t)i] =
                        field.replacement[i];
            }
        }
    }

    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        for (size_t i = 0; i < write.expected.size(); ++i) {
            const uint32_t address =
                (uint32_t)write.location + (uint32_t)i;
            const uint8_t observed = psx_read_byte(address);
            if (observed == write.expected[i]) continue;
            const auto replacement = desired.find(address);
            if (replacement != desired.end() &&
                observed == replacement->second)
                continue;
            failed_at = address;
            return false;
        }
    }
    return true;
}

bool sha256_file(const std::filesystem::path& path, std::string& out,
                 std::string* error) {
    out.clear();
    if (path.empty()) return true;
    const DiscPathResolution resolved = resolve_disc_path(path);
    const std::filesystem::path input = resolved.data;
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    std::string extension = resolved.mount.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".chd") {
        PS1::ISOReader disc;
        if (!disc.Open(resolved.mount.string())) {
            if (error) *error =
                "cannot decode image fingerprint: " + resolved.mount.string();
            return false;
        }
        std::array<uint8_t, 2352> sector{};
        for (uint32_t lba = 0; lba < disc.GetSectorCount(); ++lba) {
            if (!disc.ReadRawSector(lba, sector.data())) {
                if (error) *error =
                    "cannot finish decoding image fingerprint: " +
                    resolved.mount.string();
                return false;
            }
            psx_sha256_update(&hash, sector.data(), sector.size());
        }
        uint8_t digest[32];
        psx_sha256_final(&hash, digest);
        std::ostringstream text;
        for (uint8_t byte : digest)
            text << std::hex << std::setw(2) << std::setfill('0')
                 << (unsigned)byte;
        out = text.str();
        return true;
    }

    std::array<uint8_t, 1024 * 1024> buffer{};
    std::ifstream file(input, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot fingerprint image: " + input.string();
        return false;
    }
    while (file) {
        file.read((char*)buffer.data(), (std::streamsize)buffer.size());
        const std::streamsize got = file.gcount();
        if (got > 0) psx_sha256_update(&hash, buffer.data(), (size_t)got);
    }
    if (!file.eof()) {
        if (error) *error =
            "cannot finish fingerprinting image: " + input.string();
        return false;
    }
    uint8_t digest[32];
    psx_sha256_final(&hash, digest);
    std::ostringstream text;
    for (uint8_t byte : digest)
        text << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    out = text.str();
    return true;
}

bool measured_disc_digest(const std::filesystem::path& path, std::string& digest, std::string* error) {
    HostLaunchTimingScope timing(HOST_LAUNCH_DISC_HASH);
    if (!path.empty()) psx_mod_counter_add("startup.disc_hash", 1);
    const bool ok = sha256_file(path, digest, error);
    if (ok) timing.success();
    return ok;
}

std::string sha256_text(const std::string& value) {
    psx_sha256_ctx hash; psx_sha256_init(&hash);
    psx_sha256_update(&hash, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    uint8_t bytes[32]; psx_sha256_final(&hash, bytes);
    std::ostringstream text;
    for (uint8_t byte : bytes) text << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return text.str();
}

// Offline only: semantics match sha256_file (CUE data / CHD raw sectors).
bool offline_disc_digest(const std::filesystem::path& path, std::string& digest,
                         std::string* error, bool cached_only) {
    digest.clear();
    if (path.empty()) return true;
    const auto resolved = resolve_disc_path(path);
    std::string stamp, mount_stamp;
    const bool reliable = host_file_identity(resolved.data, stamp) &&
        host_file_identity(resolved.mount, mount_stamp);
    const std::string key = "disc-sha-v2\n" + resolved.mount.generic_string() + "\n" +
        resolved.data.generic_string() + "\n" + stamp + "\n" + mount_stamp + "\n";
    const bool cacheable = reliable && key.size() <= 4096;
    const auto directory = media_cache_root() / "disc-digests-v2";
    const auto receipt = directory / (sha256_text(key) + ".txt");
    if (cacheable) {
        std::ifstream file(receipt, std::ios::binary);
        std::array<char, 4227> buffer{};
        file.read(buffer.data(), buffer.size());
        const std::string record(buffer.data(), size_t(file.gcount()));
        if (file.eof() && record.size() == key.size() + 130 &&
            record.compare(0, key.size(), key) == 0) {
            const std::string value = record.substr(key.size(), 64);
            const std::string body = key + value + "\n";
            if (std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
                record == body + sha256_text(body) + "\n") {
                digest = value;
                psx_mod_counter_add("startup.disc_digest_hit", 1);
                return true;
            }
        }
    }
    if (cached_only) return false;
    if (!measured_disc_digest(path, digest, error)) return false;
    std::string after, mount_after;
    if (!cacheable || !host_file_identity(resolved.data, after) || after != stamp ||
        !host_file_identity(resolved.mount, mount_after) || mount_after != mount_stamp) return true;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return true;
#if defined(_WIN32)
    const auto temporary = receipt.string() + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
#else
    const auto temporary = receipt.string() + "." + std::to_string(getpid()) + ".tmp";
#endif
    const std::string body = key + digest + "\n";
    { std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if (!(file << body << sha256_text(body) << '\n')) return true; }
#if defined(_WIN32)
    MoveFileExW(std::filesystem::path(temporary).c_str(), receipt.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    std::filesystem::rename(temporary, receipt, ec);
#endif
    return true;
}

std::filesystem::path raw_image_path(const std::filesystem::path& path,
                                     std::string* error) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension != ".cue") return path;
    std::ifstream cue(path);
    if (!cue) {
        if (error) *error = "cannot open disc CUE: " + path.string();
        return {};
    }
    std::string line;
    while (std::getline(cue, line)) {
        size_t at = line.find_first_not_of(" \t");
        if (at == std::string::npos || line.size() - at < 4) continue;
        std::string keyword = line.substr(at, 4);
        std::transform(keyword.begin(), keyword.end(), keyword.begin(),
            [](unsigned char c) { return (char)std::toupper(c); });
        if (keyword != "FILE") continue;
        at = line.find_first_not_of(" \t", at + 4);
        if (at == std::string::npos) continue;
        std::string name;
        if (line[at] == '"') {
            const size_t end = line.find('"', at + 1);
            if (end == std::string::npos) continue;
            name = line.substr(at + 1, end - at - 1);
        } else {
            const size_t end = line.find_first_of(" \t", at);
            name = line.substr(at, end - at);
        }
        return (path.parent_path() / name).lexically_normal();
    }
    if (error) *error = "disc CUE has no source file: " + path.string();
    return {};
}

bool sha256_disc_range(const std::filesystem::path& image,
                       ModPatchTarget target, uint64_t location, size_t size,
                       std::string& out, std::string* error) {
    std::string extension = image.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".chd") {
        PS1::ISOReader disc;
        if (!disc.Open(image.string())) {
            if (error) *error = "cannot open stock CHD range: " + image.string();
            return false;
        }
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        std::array<uint8_t, 2352> sector{};
        size_t remaining = size;
        uint64_t at = location;
        const uint64_t sector_size =
            target == ModPatchTarget::DiscRaw ? 2352u : 2048u;
        while (remaining != 0) {
            const uint64_t lba64 = at / sector_size;
            if (lba64 >= disc.GetSectorCount()) {
                if (error) *error = "overlay expected range exceeds stock CHD";
                return false;
            }
            const size_t within = (size_t)(at % sector_size);
            const bool read_ok =
                target == ModPatchTarget::DiscRaw
                    ? disc.ReadRawSector((uint32_t)lba64, sector.data())
                    : disc.ReadSector((uint32_t)lba64, sector.data());
            if (!read_ok) {
                if (error) *error = "cannot decode stock CHD overlay range";
                return false;
            }
            const size_t chunk =
                std::min(remaining, (size_t)sector_size - within);
            psx_sha256_update(&hash, sector.data() + within, chunk);
            at += chunk;
            remaining -= chunk;
        }
        uint8_t digest[32];
        psx_sha256_final(&hash, digest);
        std::ostringstream text;
        for (uint8_t byte : digest)
            text << std::hex << std::setw(2) << std::setfill('0')
                 << (unsigned)byte;
        out = text.str();
        return true;
    }

    const std::filesystem::path source = raw_image_path(image, error);
    if (source.empty()) return false;
    std::ifstream file(source, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open stock image range: " + source.string();
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff file_size = file.tellg();
    if (file_size < 0) {
        if (error) *error = "cannot size stock image: " + source.string();
        return false;
    }
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    std::array<uint8_t, 2048> bytes{};
    size_t remaining = size;
    uint64_t at = location;
    const bool raw_source = file_size > 0 &&
        ((uint64_t)file_size % 2352u) == 0;
    while (remaining != 0) {
        uint64_t physical = at;
        size_t chunk = remaining;
        if (target == ModPatchTarget::DiscUser && raw_source) {
            const uint64_t lba = at / 2048u;
            const size_t within = (size_t)(at % 2048u);
            physical = lba * 2352u + 24u + within;
            chunk = std::min(chunk, 2048u - within);
        }
        chunk = std::min(chunk, bytes.size());
        if (physical > (uint64_t)file_size ||
            chunk > (uint64_t)file_size - physical) {
            if (error) *error = "overlay expected range exceeds stock image";
            return false;
        }
        file.clear();
        file.seekg((std::streamoff)physical);
        if (!file.read((char*)bytes.data(), (std::streamsize)chunk)) {
            if (error) *error = "cannot read stock image overlay range";
            return false;
        }
        psx_sha256_update(&hash, bytes.data(), chunk);
        at += chunk;
        remaining -= chunk;
    }
    uint8_t digest[32];
    psx_sha256_final(&hash, digest);
    std::ostringstream text;
    for (uint8_t byte : digest)
        text << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    out = text.str();
    return true;
}

#if defined(_WIN32)
std::wstring quote_windows_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(c);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
#endif

bool run_xdelta_decode(const std::filesystem::path& executable,
                       const std::filesystem::path& source,
                       const std::filesystem::path& patch,
                       const std::filesystem::path& output,
                       std::string* error) {
#if defined(_WIN32)
    std::wstring command =
        quote_windows_argument(executable.wstring()) + L" -f -n -d -s " +
        quote_windows_argument(source.wstring()) + L" " +
        quote_windows_argument(patch.wstring()) + L" " +
        quote_windows_argument(output.wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.wstring().c_str(), mutable_command.data(),
                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        if (error) *error = "cannot start trusted xdelta3 decoder (Windows error " +
            std::to_string((unsigned long)GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        if (error) *error =
            "trusted xdelta3 decoder failed with exit code " + std::to_string(exit_code);
        return false;
    }
    return true;
#else
    const pid_t child = fork();
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), "-f", "-n", "-d", "-s",
              source.c_str(), patch.c_str(), output.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (child < 0) {
        if (error) *error = "cannot start trusted xdelta3 decoder";
        return false;
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        if (error) *error = "trusted xdelta3 decoder failed";
        return false;
    }
    return true;
#endif
}

bool valid_cached_disc(const std::filesystem::path& path,
                       const ModResolution::DerivedDisc& derived) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) &&
           std::filesystem::file_size(path, ec) == derived.output_size && !ec;
}

bool materialize_derived_disc(RuntimeMods& s, const ModResolution& plan,
                              std::filesystem::path& out, std::string* error) {
    out.clear();
    if (plan.derived_discs.empty()) return true;
    const ModResolution::DerivedDisc& derived = plan.derived_discs.front();
    std::string digest;
    if (!sha256_file(derived.patch, digest, error) ||
        digest != derived.patch_sha256) {
        if (error && error->empty())
            *error = derived.package_id + ": derived-disc patch checksum failed";
        else if (error && digest != derived.patch_sha256)
            *error = derived.package_id + ": derived-disc patch checksum failed";
        return false;
    }
    const std::filesystem::path cache_root = s.manager.root() / "cache";
    const std::filesystem::path cached = cache_root / (plan.fingerprint + ".bin");
    if (valid_cached_disc(cached, derived)) {
        out = cached;
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_root, ec);
    if (ec) {
        if (error) *error = "cannot create derived-disc cache: " + ec.message();
        return false;
    }
    const char* override_tool = std::getenv("PSXRECOMP_XDELTA3");
    const std::filesystem::path decoder =
        override_tool && override_tool[0]
            ? std::filesystem::path(override_tool)
#if defined(_WIN32)
            : s.manager.root().parent_path() / "xdelta3.exe";
#else
            : s.manager.root().parent_path() / "xdelta3";
#endif
    if (!std::filesystem::is_regular_file(decoder, ec)) {
        if (error) *error =
            "this mod needs the trusted xdelta3 decoder, but it is missing: " +
            decoder.string();
        return false;
    }
    const std::filesystem::path source = raw_image_path(s.disc_path, error);
    if (source.empty()) return false;
#if defined(_WIN32)
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = (unsigned long)getpid();
#endif
    const std::filesystem::path temporary =
        cache_root / (plan.fingerprint + ".tmp." + std::to_string(process_id));
    std::filesystem::remove(temporary, ec);
    std::fprintf(stdout, "psxrecomp: building derived disc for %s...\n",
                 derived.package_id.c_str());
    if (!run_xdelta_decode(decoder, source, derived.patch, temporary, error)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    if (!valid_cached_disc(temporary, derived)) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = derived.package_id +
            ": derived disc has the wrong output size";
        return false;
    }
    if (!sha256_file(temporary, digest, error) || digest != derived.output_sha256) {
        std::filesystem::remove(temporary, ec);
        if (error && digest != derived.output_sha256)
            *error = derived.package_id + ": derived disc checksum failed";
        return false;
    }
    std::filesystem::rename(temporary, cached, ec);
    if (ec) {
        std::filesystem::remove(cached, ec);
        ec.clear();
        std::filesystem::rename(temporary, cached, ec);
    }
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = "cannot publish derived-disc cache: " + ec.message();
        return false;
    }
    std::fprintf(stdout, "psxrecomp: cached derived disc %s\n", cached.string().c_str());
    out = cached;
    return true;
}

#if defined(RECOMP_LAUNCHER)
void copy_text(char* out, size_t capacity, const std::string& value) {
    if (!out || capacity == 0) return;
    std::snprintf(out, capacity, "%s", value.c_str());
}

int provider_package_count(void*) {
    return (int)state().manager.packages().size();
}

int provider_package_get(void*, int index, RecompLauncherCModPackage* out) {
    if (!out || index < 0) return 0;
    const auto& packages = state().manager.packages();
    if ((size_t)index >= packages.size()) return 0;
    auto item = packages.begin();
    std::advance(item, index);
    const ModPackage* package = selected_package(item->first);
    if (!package) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), package->id);
    copy_text(out->version, sizeof(out->version), package->version);
    copy_text(out->name, sizeof(out->name), package->name);
    copy_text(out->author, sizeof(out->author), package->author);
    out->author_link_count = std::min(
        (int)package->author_links.size(), RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX);
    for (int i = 0; i < out->author_link_count; ++i) {
        copy_text(out->author_links[i].name, sizeof(out->author_links[i].name),
                  package->author_links[(size_t)i].name);
        copy_text(out->author_links[i].url, sizeof(out->author_links[i].url),
                  package->author_links[(size_t)i].url);
    }
    copy_text(out->description, sizeof(out->description), package->description);
    copy_text(out->license, sizeof(out->license), package->license);
    copy_text(out->source_name, sizeof(out->source_name), package->source_name);
    copy_text(out->source_url, sizeof(out->source_url), package->source_url);
    out->enabled = package_has_enabled_feature(*package);
    out->option_count = (int)package->options.size();
    /* A bundled package is build output. Offering to remove it would succeed
     * and then be silently undone by the next build. */
    out->removable = !package_in_use(*package) &&
                     package->origin == ModPackageOrigin::Installed;
    return 1;
}

int provider_option_get(void*, const char* package_id, int index,
                        RecompLauncherCModOption* out) {
    if (!package_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package || (size_t)index >= package->options.size()) return 0;
    const ModOption& option = package->options[(size_t)index];
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), option.id);
    copy_text(out->label, sizeof(out->label), option.label);
    copy_text(out->description, sizeof(out->description), option.description);
    copy_text(out->group, sizeof(out->group), option.group);
    copy_text(out->value, sizeof(out->value), selected_value(*package, option));
    copy_text(out->default_value, sizeof(out->default_value), option.default_value);
    out->type = option.type == ModOptionType::Boolean ? RECOMP_MOD_OPTION_BOOLEAN :
                option.type == ModOptionType::Choice ? RECOMP_MOD_OPTION_CHOICE :
                                                       RECOMP_MOD_OPTION_INTEGER;
    out->min_value = option.min_value;
    out->max_value = option.max_value;
    out->step = option.step;
    out->choice_count = (int)option.choices.size();
    out->disabled = option_is_disabled(*package, option) ? 1 : 0;
    return 1;
}

int provider_choice_get(void*, const char* package_id, const char* option_id,
                        int index, RecompLauncherCModChoice* out) {
    if (!package_id || !option_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    const auto option = std::find_if(package->options.begin(), package->options.end(),
        [&](const ModOption& value) { return value.id == option_id; });
    if (option == package->options.end() || (size_t)index >= option->choices.size()) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->value, sizeof(out->value), option->choices[(size_t)index].value);
    copy_text(out->label, sizeof(out->label), option->choices[(size_t)index].label);
    return 1;
}

template <typename Callback>
int mutate(Callback callback);

bool provider_feature_at(int index, const ModPackage*& package,
                         const ModFeature*& feature) {
    if (index < 0) return false;
    for (const auto& [package_id, versions] : state().manager.packages()) {
        (void)versions;
        const ModPackage* selected = selected_package(package_id);
        if (!selected) continue;
        for (const ModFeature& candidate : selected->features) {
            if (index-- == 0) {
                package = selected;
                feature = &candidate;
                return true;
            }
        }
    }
    return false;
}

std::vector<const ModOption*> provider_feature_options(
    const ModPackage& package, const std::string& feature_id) {
    std::vector<const ModOption*> out;
    for (const ModOption& option : package.options)
        if (option.feature_id == feature_id) out.push_back(&option);
    return out;
}

bool diagnostic_matches(const ModResolution::Diagnostic& diagnostic,
                        const std::string& package_id,
                        const std::string& feature_id) {
    return (diagnostic.package_id == package_id &&
            diagnostic.feature_id == feature_id) ||
           (diagnostic.other_package_id == package_id &&
            diagnostic.other_feature_id == feature_id);
}

int provider_feature_count(void*) {
    int count = 0;
    for (const auto& [package_id, versions] : state().manager.packages()) {
        (void)versions;
        const ModPackage* package = selected_package(package_id);
        if (package) count += (int)package->features.size();
    }
    return count;
}

int provider_feature_get(void*, int index, RecompLauncherCModFeature* out) {
    if (!out) return 0;
    const ModPackage* package = nullptr;
    const ModFeature* feature = nullptr;
    if (!provider_feature_at(index, package, feature)) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), feature->id);
    copy_text(out->package_id, sizeof(out->package_id), package->id);
    copy_text(out->package_version, sizeof(out->package_version), package->version);
    copy_text(out->package_name, sizeof(out->package_name), package->name);
    copy_text(out->name, sizeof(out->name), feature->name);
    copy_text(out->author, sizeof(out->author),
              feature->author.empty() ? package->author : feature->author);
    out->author_link_count = std::min(
        (int)package->author_links.size(), RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX);
    for (int i = 0; i < out->author_link_count; ++i) {
        copy_text(out->author_links[i].name, sizeof(out->author_links[i].name),
                  package->author_links[(size_t)i].name);
        copy_text(out->author_links[i].url, sizeof(out->author_links[i].url),
                  package->author_links[(size_t)i].url);
    }
    copy_text(out->description, sizeof(out->description), feature->description);
    copy_text(out->source_name, sizeof(out->source_name), package->source_name);
    copy_text(out->source_url, sizeof(out->source_url), package->source_url);
    copy_text(out->group, sizeof(out->group), feature->group);
    out->hidden = feature->hidden ? 1 : 0;
    switch (feature->channel) {
        case ModChannel::Experimental:
            out->channel = RECOMP_MOD_CHANNEL_EXPERIMENTAL;
            break;
        case ModChannel::Developer:
            /* Only reachable on a local developer build: a shipped catalog
             * carries no developer-channel feature to report. */
            out->channel = RECOMP_MOD_CHANNEL_DEVELOPER;
            break;
        case ModChannel::Stable:
            out->channel = RECOMP_MOD_CHANNEL_STABLE;
            break;
    }
    out->enabled =
        state().manager.feature_enabled(package->id, feature->id) ? 1 : 0;
    out->option_count =
        (int)provider_feature_options(*package, feature->id).size();
    for (const ModResolution::Diagnostic& diagnostic :
         state().validation.diagnostics) {
        if (!diagnostic_matches(diagnostic, package->id, feature->id)) continue;
        out->has_error = 1;
        copy_text(out->status, sizeof(out->status), diagnostic.message);
        break;
    }
    return 1;
}

int provider_feature_option_get(void*, const char* package_id,
                                const char* feature_id, int index,
                                RecompLauncherCModOption* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    const auto options = provider_feature_options(*package, feature_id);
    if ((size_t)index >= options.size()) return 0;
    const ModOption& option = *options[(size_t)index];
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), option.id);
    copy_text(out->label, sizeof(out->label), option.label);
    copy_text(out->description, sizeof(out->description), option.description);
    copy_text(out->group, sizeof(out->group), option.group);
    copy_text(out->value, sizeof(out->value),
              state().manager.feature_option_value(
                  package_id, feature_id, option.id));
    copy_text(out->default_value, sizeof(out->default_value),
              option.default_value);
    out->type = option.type == ModOptionType::Boolean
        ? RECOMP_MOD_OPTION_BOOLEAN
        : option.type == ModOptionType::Choice
            ? RECOMP_MOD_OPTION_CHOICE : RECOMP_MOD_OPTION_INTEGER;
    out->min_value = option.min_value;
    out->max_value = option.max_value;
    out->step = option.step;
    out->choice_count = (int)option.choices.size();
    out->disabled = option_is_disabled(*package, option) ? 1 : 0;
    return 1;
}

int provider_feature_choice_get(void*, const char* package_id,
                                const char* feature_id,
                                const char* option_id, int index,
                                RecompLauncherCModChoice* out) {
    if (!package_id || !feature_id || !option_id || !out || index < 0)
        return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    const auto option = std::find_if(
        package->options.begin(), package->options.end(),
        [&](const ModOption& value) {
            return value.feature_id == feature_id && value.id == option_id;
        });
    if (option == package->options.end() ||
        (size_t)index >= option->choices.size()) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->value, sizeof(out->value),
              option->choices[(size_t)index].value);
    copy_text(out->label, sizeof(out->label),
              option->choices[(size_t)index].label);
    return 1;
}

int provider_feature_enable(void*, const char* package_id,
                            const char* feature_id, int enabled) {
    if (!package_id || !feature_id) return 0;
    return mutate([&](std::string& error) {
        const ModFeature* feature =
            state().manager.selected_feature(package_id, feature_id);
        if (feature && feature->legacy)
            return state().manager.set_enabled(
                package_id, enabled != 0, &error);
        return state().manager.set_feature_enabled(
            package_id, feature_id, enabled != 0, &error);
    });
}

int provider_feature_set_option(void*, const char* package_id,
                                const char* feature_id,
                                const char* option_id,
                                const char* value) {
    if (!package_id || !feature_id || !option_id || !value) return 0;
    return mutate([&](std::string& error) {
        const ModFeature* feature =
            state().manager.selected_feature(package_id, feature_id);
        if (feature && feature->legacy)
            return state().manager.set_option(
                package_id, option_id, value, &error);
        return state().manager.set_feature_option(
            package_id, feature_id, option_id, value, &error);
    });
}

int provider_feature_resource_count(void*, const char* package_id,
                                    const char* feature_id) {
    if (!package_id || !feature_id) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    return (int)std::count_if(
        package->resources.begin(), package->resources.end(),
        [&](const ModResource& resource) {
            return resource.feature_id == feature_id && !resource.hidden;
        });
}

int provider_feature_resource_get(void*, const char* package_id,
                                  const char* feature_id, int index,
                                  RecompLauncherCModResource* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    for (const ModResource& resource : package->resources) {
        if (resource.feature_id != feature_id || resource.hidden) continue;
        if (index-- != 0) continue;
        const std::filesystem::path path =
            state().manager.feature_resource_path(
                package_id, feature_id, resource.id);
        std::error_code ec;
        const bool directory =
            resource.format == "directory" || resource.format == "folder";
        const bool verified = !path.empty() &&
            (directory ? std::filesystem::is_directory(path, ec)
                       : std::filesystem::is_regular_file(path, ec));
        std::memset(out, 0, sizeof(*out));
        copy_text(out->id, sizeof(out->id), resource.id);
        copy_text(out->label, sizeof(out->label), resource.label);
        copy_text(out->description, sizeof(out->description),
                  resource.description);
        copy_text(out->path, sizeof(out->path), path.u8string());
        copy_text(out->status, sizeof(out->status),
                  path.empty() ? "Not selected" :
                      (verified ? "Selected" : "Selected path is missing"));
        copy_text(out->file_patterns, sizeof(out->file_patterns),
                  resource.file_patterns);
        copy_text(out->file_description, sizeof(out->file_description),
                  resource.file_description);
        out->required = resource.required ? 1 : 0;
        out->verified = verified ? 1 : 0;
        copy_text(out->format, sizeof(out->format), resource.format);
        return 1;
    }
    return 0;
}

int provider_feature_resource_set_path(void*, const char* package_id,
                                       const char* feature_id,
                                       const char* resource_id,
                                       const char* path) {
    if (!package_id || !feature_id || !resource_id || !path) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_feature_resource_path(
            package_id, feature_id, resource_id,
            std::filesystem::u8path(path), &error);
    });
}

int provider_diagnostic_count(void*, const char* package_id,
                              const char* feature_id) {
    if (!package_id || !feature_id) return 0;
    return (int)std::count_if(
        state().validation.diagnostics.begin(),
        state().validation.diagnostics.end(),
        [&](const ModResolution::Diagnostic& diagnostic) {
            return diagnostic_matches(diagnostic, package_id, feature_id);
        });
}

int provider_diagnostic_get(void*, const char* package_id,
                            const char* feature_id, int index,
                            RecompLauncherCModDiagnostic* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    for (const ModResolution::Diagnostic& diagnostic :
         state().validation.diagnostics) {
        if (!diagnostic_matches(diagnostic, package_id, feature_id)) continue;
        if (index-- != 0) continue;
        std::memset(out, 0, sizeof(*out));
        out->severity = 2;
        copy_text(out->resource, sizeof(out->resource), diagnostic.resource);
        copy_text(out->message, sizeof(out->message), diagnostic.message);
        const bool primary = diagnostic.package_id == package_id &&
                             diagnostic.feature_id == feature_id;
        copy_text(out->related_package_id, sizeof(out->related_package_id),
                  primary ? diagnostic.other_package_id :
                            diagnostic.package_id);
        copy_text(out->related_feature_id, sizeof(out->related_feature_id),
                  primary ? diagnostic.other_feature_id :
                            diagnostic.feature_id);
        return 1;
    }
    return 0;
}

int provider_version_count(void*, const char* package_id) {
    if (!package_id) return 0;
    const auto package = state().manager.packages().find(package_id);
    return package == state().manager.packages().end() ? 0 : (int)package->second.size();
}

int provider_version_get(void*, const char* package_id, int index,
                         RecompLauncherCModVersion* out) {
    if (!package_id || !out || index < 0) return 0;
    const auto package = state().manager.packages().find(package_id);
    if (package == state().manager.packages().end() ||
        (size_t)index >= package->second.size()) return 0;
    auto version = package->second.begin();
    std::advance(version, index);
    std::memset(out, 0, sizeof(*out));
    copy_text(out->version, sizeof(out->version), version->first);
    const ModPackage* selected = selected_package(package_id);
    out->selected = selected && selected->version == version->first;
    out->removable = (!out->selected || !selected ||
                      !package_in_use(*selected)) &&
                     version->second.origin == ModPackageOrigin::Installed;
    return 1;
}

template <typename Callback>
int mutate(Callback callback) {
    state().ticket_ready = false;
    ++state().preparation_revision;
    std::string error;
    if (!callback(error)) {
        set_error(error);
        return 0;
    }
    if (!state().disc_path.empty())
        state().validation = state().manager.resolve(
            state().game_id, state().exe_sha256, state().disc_sha256, true);
    else
        state().validation = {};
    state().error.clear();
    return 1;
}

int provider_install(void*, const char* path) {
    if (!path) return 0;
    return mutate([&](std::string& error) {
        std::string id, version;
        if (!state().manager.install_archive(path, &id, &version, &error)) return false;
        if (!state().manager.scan(&error)) return false;
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
        state().catalog_stamped = catalog_files(state(), state().loaded_catalog);
#endif
        return state().manager.select_version(id, version, &error);
    });
}

int provider_remove(void*, const char* id, const char* version) {
    if (!id || !version) return 0;
    return mutate([&](std::string& error) {
        if (!state().manager.remove_version(id, version, &error)) return false;
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
        state().catalog_stamped = catalog_files(state(), state().loaded_catalog);
#endif
        return true;
    });
}

int provider_enable(void*, const char* id, int enabled) {
    if (!id) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_enabled(id, enabled != 0, &error);
    });
}

int provider_select(void*, const char* id, const char* version) {
    if (!id || !version) return 0;
    return mutate([&](std::string& error) {
        return state().manager.select_version(id, version, &error);
    });
}

int provider_set_option(void*, const char* id, const char* option, const char* value) {
    if (!id || !option || !value) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_option(id, option, value, &error);
    });
}

int provider_commit(void*, const char* image_path) {
    HostLaunchTimingScope timing(HOST_LAUNCH_PROVIDER_COMMIT);
    std::string error;
    const bool ok = mod_runtime_prepare_for_launcher(
        image_path ? std::filesystem::path(image_path) : std::filesystem::path(), &error);
    if (!ok) {
        set_error(error);
        return 0;
    }
    state().error.clear();
    timing.success();
    return 1;
}

int provider_commit_netplay(void*, const char* image_path) {
    HostLaunchTimingScope timing(HOST_LAUNCH_PROVIDER_NETPLAY_COMMIT);
    std::string error;
    /* Without [netplay] content_negotiation every netplay session is vanilla. */
    if (!(mod_runtime_netplay_content_negotiation()
            ? mod_runtime_commit_for_netplay(image_path ? std::filesystem::path(image_path) :
                                             std::filesystem::path(), &error)
            : mod_runtime_clear_for_netplay(&error))) {
        set_error(error);
        return 0;
    }
    state().error.clear();
    timing.success();
    return 1;
}

const char* provider_error(void*) {
    return state().error.c_str();
}

RecompLauncherCModProvider provider = {
    nullptr,
    provider_package_count,
    provider_package_get,
    provider_option_get,
    provider_choice_get,
    provider_version_count,
    provider_version_get,
    provider_install,
    provider_remove,
    provider_enable,
    provider_select,
    provider_set_option,
    provider_commit,
    provider_error,
    provider_feature_count,
    provider_feature_get,
    provider_feature_option_get,
    provider_feature_choice_get,
    provider_feature_enable,
    provider_feature_set_option,
    provider_diagnostic_count,
    provider_diagnostic_get,
    nullptr, /* archive_extension — PSX defaults */
    nullptr, /* archive_description */
    provider_commit_netplay,
    provider_feature_resource_count,
    provider_feature_resource_get,
    provider_feature_resource_set_path,
};
#endif

} // namespace

static std::optional<ModPackageManager> offline_manager_before_netplay;
static bool netplay_content_negotiation_enabled = false;

void mod_runtime_set_netplay_content_negotiation(bool enabled) {
    netplay_content_negotiation_enabled = enabled;
}

bool mod_runtime_netplay_content_negotiation() {
    return netplay_content_negotiation_enabled;
}

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path,
                            std::string* error) {
    RuntimeMods& s = state();
    s.ticket_ready = false;
    ++s.preparation_revision;
    offline_manager_before_netplay.reset();
    mod_runtime_set_session_plan_fp({});
    s.manager.set_root({});
    clear_function_entry_hooks();
    s.disc_extents.clear();
    s.audio_tracks.clear();
    s.extent_start = 0;
    psx_mod_clear_function_replacements();
    s.plan = {};
    s.validation = {};
    s.raw_disc_index.clear();
    s.user_disc_index.clear();
    s.raw_overlay_index.clear();
    s.user_overlay_index.clear();
    s.game_id.clear();
    s.error.clear();
    s.exe_sha256.clear();
    s.disc_sha256.clear();
    s.disc_path.clear();
    s.effective_disc_path.clear();
    s.entry_phys = 0;
    s.initialized = false;
    s.main_applied = false;
    s.disc_enabled = false;
    s.disc_guard_failed = false;
    s.manager.set_root(root);
    s.game_id = game_id;
    s.entry_phys = game_entry_pc & 0x1FFFFFFFu;
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    std::map<std::filesystem::path, std::string> catalog_before;
    const bool catalog_before_stamped = catalog_files(s, catalog_before);
#endif
    if (!s.manager.scan(&s.error) || !s.manager.load_state(&s.error)) {
        if (error) *error = s.error;
        return false;
    }
    /* A manifest that fails to parse used to be skipped in silence, so a mod
     * author's typo produced a mod that simply did not exist. Name every one. */
    for (const std::string& scan_error : s.manager.scan_errors())
        std::fprintf(stderr, "psxrecomp: mod manifest ignored: %s\n",
                     scan_error.c_str());
    /* state.toml may name a package this catalog does not hold (removed,
     * stripped from a release, or a builtin the title excludes). resolve()
     * never visits it and save_state() keeps it; say so once. */
    for (const std::string& dormant : s.manager.dormant_selections())
        std::fprintf(stdout,
                     "psxrecomp: mod selection kept but inactive: %s is not "
                     "in this build's mod catalog\n",
                     dormant.c_str());
    /* psx.hd-textures has a shared directory-resource contract, independent of
     * the title package id. Keep owner files outside the build-owned bundled
     * tree, and give the launcher a useful Open folder action on first use. */
    std::string texture_game_id = game_id;
    for (char& c : texture_game_id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            c = '_';
    if (texture_game_id.empty()) texture_game_id = "game";
    for (const auto& entry : s.manager.packages()) {
        const ModPackage* package = s.manager.selected_package(entry.first);
        if (!package || !std::any_of(package->targets.begin(), package->targets.end(),
                [&](const ModTarget& target) {
                    return target.game_id == game_id || target.game_id == "*";
                })) continue;
        for (const ModPlugin& plugin : package->plugins) {
            if (plugin.id != "psx.hd-textures" ||
                !s.manager.feature_resource_path(package->id, plugin.feature_id, "pack").empty())
                continue;
            const auto resource = std::find_if(package->resources.begin(), package->resources.end(),
                [&](const ModResource& r) {
                    return r.feature_id == plugin.feature_id && r.id == "pack" &&
                        (r.format == "directory" || r.format == "folder");
                });
            if (resource == package->resources.end()) continue;
            std::error_code ec;
            const auto pack = std::filesystem::absolute(
                root / "texture-packs" / texture_game_id, ec);
            if (!ec) std::filesystem::create_directories(pack / "replacements", ec);
            if (!ec) std::filesystem::create_directories(pack / "dumps", ec);
            std::string directory_error;
            if (ec || !s.manager.set_feature_resource_path(package->id,
                    plugin.feature_id, "pack", pack, &directory_error)) {
                std::fprintf(stderr, "psxrecomp: texture pack folder: %s\n",
                    ec ? ec.message().c_str() : directory_error.c_str());
            }
        }
    }
    if (!sha256_file(exe_path, s.exe_sha256, &s.error)) {
        /* Release installs commonly do not carry a loose PS-X EXE; game-id and
         * expected-byte guards remain available in that case. */
        s.exe_sha256.clear();
        s.error.clear();
    }
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    s.catalog_stamped = catalog_before_stamped && catalog_files(s, s.loaded_catalog) &&
        catalog_before == s.loaded_catalog;
#endif
    s.initialized = true;
    return true;
}

bool mod_runtime_clear_for_netplay(std::string* error) {
    state().ticket_ready = false;
    ++state().preparation_revision;
    RuntimeMods& s = state();
    if (!s.initialized) {
        if (error) error->clear();
        return true;
    }
    clear_function_entry_hooks();
    s.disc_extents.clear();
    s.audio_tracks.clear();
    s.extent_start = 0;
    s.plan = {};
    psx_mod_clear_function_replacements();
    s.validation = {};
    s.raw_disc_index.clear();
    s.user_disc_index.clear();
    s.raw_overlay_index.clear();
    s.user_overlay_index.clear();
    s.effective_disc_path.clear();
    s.main_applied = false;
    s.disc_enabled = false;
    s.disc_guard_failed = false;
    s.netplay_view_plan = false;
    s.error.clear();
    if (error) error->clear();
    std::fprintf(stdout, "psxrecomp: mods cleared for netplay (vanilla session)\n");
    return true;
}


/* One CSV element is "<feature>" or "<feature>=<opt>~<val>[+<opt>~<val>]". */
static void feat_token_split(const std::string& token, std::string& name,
                             std::string& opts) {
    const size_t eq = token.find('=');
    if (eq == std::string::npos) {
        name = token;
        opts.clear();
    } else {
        name = token.substr(0, eq);
        opts = token.substr(eq + 1);
    }
}

static bool feat_csv_wants(const char* csv, const std::string& id) {
    if (!csv || !csv[0]) return true;
    const char* q = csv;
    while (*q) {
        const char* start = q;
        while (*q && *q != ',') ++q;
        std::string name, opts;
        feat_token_split(std::string(start, q), name, opts);
        if (name == id) return true;
        if (*q == ',') ++q;
    }
    return false;
}

static bool feat_csv_apply_options(ModPackageManager& mgr,
                                   const std::string& package_id,
                                   const std::string& feature_id,
                                   const char* csv,
                                   std::string* error) {
    if (!csv || !csv[0]) return true;
    const char* q = csv;
    while (*q) {
        const char* start = q;
        while (*q && *q != ',') ++q;
        std::string name, opts;
        feat_token_split(std::string(start, q), name, opts);
        if (*q == ',') ++q;
        if (name != feature_id || opts.empty()) continue;
        size_t pos = 0;
        while (pos < opts.size()) {
            size_t end = opts.find('+', pos);
            if (end == std::string::npos) end = opts.size();
            const std::string kv = opts.substr(pos, end - pos);
            pos = end + 1;
            const size_t tilde = kv.find('~');
            if (tilde == std::string::npos || tilde == 0) {
                if (error) *error = package_id + "/" + feature_id +
                    ": malformed option in netplay mod plan";
                return false;
            }
            const std::string key = kv.substr(0, tilde);
            const std::string val = kv.substr(tilde + 1);
            std::string err;
            if (!mgr.set_feature_option(package_id, feature_id, key, val, &err)) {
                if (error) *error = err.empty()
                    ? (package_id + "/" + feature_id +
                       ": unsupported netplay option " + key)
                    : err;
                return false;
            }
        }
        return true;
    }
    return true;
}

static bool feat_csv_all_known(const ModPackage& package, const char* csv,
                               std::string* error) {
    if (!csv || !csv[0]) return true;
    const char* q = csv;
    while (*q) {
        const char* start = q;
        while (*q && *q != ',') ++q;
        std::string name, opts;
        feat_token_split(std::string(start, q), name, opts);
        bool found = false;
        for (const ModFeature& feature : package.features) {
            if (feature.id == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (error) *error = package.id + ": unknown netplay feature " + name;
            return false;
        }
        if (*q == ',') ++q;
    }
    return true;
}

static bool apply_host_mod_plan(const PsxLobbyMatchCaps& caps, std::string* error) {
    RuntimeMods& s = state();
    if (!s.initialized) return true;
    for (int i = 0; i < caps.mod_count; ++i) {
        const PsxLobbyModPkg& pkg = caps.mods[i];
        if (!pkg.id[0]) continue;
        std::string err;
        if (pkg.ver[0] && !s.manager.select_version(pkg.id, pkg.ver, &err)) {
            if (error) *error = err.empty()
                ? (std::string(pkg.id) + " " + pkg.ver + " is not installed")
                : err;
            return false;
        }
    }
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (const auto& item : s.manager.packages()) {
            const std::string& package_id = item.first;
            const ModPackage* package = s.manager.selected_package(package_id);
            if (!package) continue;
            int required = -1;
            for (int i = 0; i < caps.mod_count; ++i) {
                if (package_id == caps.mods[i].id) {
                    required = i;
                    break;
                }
            }
            const bool legacy = package->features.size() == 1 &&
                                package->features.front().legacy;
            if (legacy) {
                const bool want = required >= 0;
                const bool have = package_has_enabled_feature(*package);
                if (want == have) continue;
                std::string err;
                if (!s.manager.set_enabled(package_id, want, &err) && want) {
                    if (error) *error = err;
                    return false;
                }
                changed = true;
                continue;
            }
            for (const ModFeature& feature : package->features) {
                const bool want = required >= 0 &&
                    feat_csv_wants(caps.mods[required].feats, feature.id);
                const bool have = s.manager.feature_enabled(package_id, feature.id);
                if (want == have) continue;
                std::string err;
                if (!s.manager.set_feature_enabled(package_id, feature.id, want, &err)) {
                    if (want) {
                        if (error) *error = err;
                        return false;
                    }
                    continue;
                }
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (int i = 0; i < caps.mod_count; ++i) {
        const PsxLobbyModPkg& pkg = caps.mods[i];
        if (!pkg.id[0]) continue;
        const ModPackage* package = s.manager.selected_package(pkg.id);
        if (!package) {
            if (error) *error = std::string(pkg.id) + " is not installed";
            return false;
        }
        const bool legacy = package->features.size() == 1 &&
                            package->features.front().legacy;
        if (!legacy && !feat_csv_all_known(*package, pkg.feats, error))
            return false;
        std::string err;
        if (legacy) {
            if (!s.manager.set_enabled(pkg.id, true, &err)) {
                if (error) *error = err;
                return false;
            }
            continue;
        }
        for (const ModFeature& feature : package->features) {
            const bool want = feat_csv_wants(pkg.feats, feature.id);
            if (!s.manager.set_feature_enabled(pkg.id, feature.id, want, &err) && want) {
                if (error) *error = err;
                return false;
            }
            if (want && !feat_csv_apply_options(s.manager, pkg.id, feature.id,
                                                  pkg.feats, error))
                return false;
        }
    }
    return true;
}

static std::string& session_plan_fp() {
    static std::string fp;
    return fp;
}

/* resolve() hashes and snapshots declared media before returning it. Only
 * that form has a path-independent identity; loose folders/files do not. */
static bool netplay_resources_verified(const ModResolution& plan,
                                       std::string* error = nullptr) {
    for (const auto& resource : plan.resources) {
        if (resource.bytes && resource.sha256.size() == 64) continue;
        if (error) *error = resource.package_id + "/" + resource.feature_id +
            ": netplay requires a verified size and SHA-256 for resource " +
            resource.id;
        return false;
    }
    return true;
}

bool mod_runtime_prepare_resources(const std::filesystem::path& disc_path, std::string* error) {
    HostLaunchTimingScope timing(HOST_LAUNCH_PREPARE_RESOURCES);
    RuntimeMods& s = state();
    if (!s.initialized) { timing.success(); return true; }
    const auto media_cache = media_cache_root();
    if (!s.manager.prepare_resources(s.game_id, disc_path, media_cache, &s.error, s.cached_prepare)) {
        if (error) *error = s.error;
        return false;
    }
    timing.success();
    return true;
}

bool mod_runtime_verify_session_plan_fp(std::string* error);

static void note_disc(RuntimeMods& s, const std::filesystem::path& disc_path) {
    if (disc_path == s.disc_path) return;
    std::string hash_error, digest;
    if (!sha256_file(disc_path, digest, &hash_error)) digest.clear();
    s.disc_path = disc_path;
    s.disc_sha256 = std::move(digest);
}

/* Capture before resolution: a resolver may mutate a resource while running. */
struct PreparedPlanInputs {
    std::filesystem::path disc_path;
    std::string disc_identity, data_identity;
    std::map<std::filesystem::path, std::string> dependencies;
    bool disc_stamped = false, stable = false;
};

static bool install_plan(RuntimeMods& s, ModResolution plan,
                         bool save_selection, bool verify_netplay,
                         std::string* error,
                         const PreparedPlanInputs* inputs = nullptr) {
    s.validation = plan;
    /* A derived activation is not in state.toml, so name it: a player (or a
     * test) reading the log can see why a hidden feature is running. */
    for (const ModResolution::ImplicitFeature& item : plan.implicit_features)
        std::fprintf(stdout,
                     "psxrecomp: mod feature %s/%s activated implicitly "
                     "(required by %s/%s)\n",
                     item.package_id.c_str(), item.feature_id.c_str(),
                     item.required_by_package_id.c_str(),
                     item.required_by_feature_id.c_str());
    if (!plan.ok) {
        s.error.clear();
        for (const std::string& item : plan.errors) {
            if (!s.error.empty()) s.error += "\n";
            s.error += item;
        }
        if (error) *error = s.error;
        return false;
    }
    if (verify_netplay && (!netplay_resources_verified(plan, &s.error) ||
                           !mod_runtime_verify_session_plan_fp(&s.error))) {
        if (error) *error = s.error;
        return false;
    }
    {
        HostLaunchTimingScope overlay_timing(HOST_LAUNCH_OVERLAY_VERIFY);
        for (const ModResolution::Overlay& overlay : plan.overlays) {
            if (overlay.expected_sha256.empty()) continue;
            std::string actual;
            if (!sha256_disc_range(
                    s.disc_path, overlay.target, overlay.location,
                    overlay.payload.size(), actual, &s.error) ||
                actual != overlay.expected_sha256) {
                if (s.error.empty())
                    s.error = overlay.package_id + "/" + overlay.feature_id +
                        ": stock overlay range checksum failed";
                if (error) *error = s.error;
                return false;
            }
        }
        overlay_timing.success();
    }
    std::filesystem::path effective_disc;
    {
        HostLaunchTimingScope derived_timing(HOST_LAUNCH_DERIVED_DISC);
        if (!materialize_derived_disc(s, plan, effective_disc, &s.error)) {
            if (error) *error = s.error;
            return false;
        }
        derived_timing.success();
    }
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    std::map<std::filesystem::path, std::string> after_resolve;
    std::string disc_after, data_after;
    const bool unchanged_disc = inputs && (inputs->disc_path.empty() ||
        (inputs->disc_stamped && host_file_identity(inputs->disc_path, disc_after) &&
         inputs->disc_identity == disc_after &&
         host_file_identity(resolve_disc_path(inputs->disc_path).data, data_after) &&
         inputs->data_identity == data_after));
    const bool unchanged_inputs = unchanged_disc && inputs->stable &&
        dependency_files(s, inputs->disc_path, after_resolve) &&
        inputs->dependencies == after_resolve;
#endif
    if (save_selection) {
        HostLaunchTimingScope save_timing(HOST_LAUNCH_SAVE_STATE);
        if (!s.manager.save_state(&s.error)) {
            if (error) *error = s.error;
            return false;
        }
        save_timing.success();
    }
    /* Hooks follow activation, never a bare commit: a new plan runs none of
     * its function-entry hooks until mod_runtime_activate_plugins(). */
    clear_function_entry_hooks();
    s.disc_extents.clear();
    s.audio_tracks.clear();
    s.extent_start = 0;
    s.plan = std::move(plan);
    psx_mod_clear_function_replacements();
    {
        HostLaunchTimingScope index_timing(HOST_LAUNCH_BUILD_DISC_INDEX);
        build_disc_index(s);
        index_timing.success();
    }
    s.effective_disc_path = std::move(effective_disc);
    s.main_applied = false;
    s.error.clear();
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    if (save_selection && (s.provider_preparing || s.cached_prepare) &&
        s.catalog_stamped && unchanged_inputs && session_plan_fp().empty()) {
        s.ticket_revision = s.preparation_revision;
        s.ticket_ready = dependency_files(s, inputs->disc_path, s.ticket_files);
    }
#endif
    return true;
}

bool mod_runtime_commit(const std::filesystem::path& disc_path, std::string* error, bool save_selection) {
    HostLaunchTimingScope timing(HOST_LAUNCH_COMMIT);
    RuntimeMods& s = state();
    s.netplay_view_plan = false;
    if (!s.initialized) { timing.success(); return true; }
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    std::map<std::filesystem::path, std::string> catalog;
    if (s.catalog_stamped && (!catalog_files(s, catalog) || catalog != s.loaded_catalog)) {
        s.ticket_ready = false;
        s.error = "Mod catalog changed after loading; reopen the launcher to rescan it.";
        if (error) *error = s.error;
        return false;
    }
    if (save_selection && prepared_ticket_matches(disc_path)) {
        if (!s.provider_preparing) s.ticket_ready = false;
        clear_function_entry_hooks();
        s.disc_extents.clear(); s.audio_tracks.clear(); s.extent_start = 0;
        s.main_applied = false;
        s.error.clear();
        psx_mod_counter_add("startup.plan_reuse", 1);
        timing.success();
        return true;
    }
#endif
    s.ticket_ready = false;
    PreparedPlanInputs inputs;
    inputs.disc_path = disc_path;
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    inputs.disc_stamped = disc_path.empty() ||
        (host_file_identity(disc_path, inputs.disc_identity) &&
         host_file_identity(resolve_disc_path(disc_path).data, inputs.data_identity));
#endif
#if !defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    if (disc_path != s.disc_path)
#endif
    {
        std::string hash_error, digest;
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
        const bool hashed = save_selection
            ? offline_disc_digest(disc_path, digest, &hash_error, s.cached_prepare)
            : measured_disc_digest(disc_path, digest, &hash_error);
#else
        const bool hashed = measured_disc_digest(disc_path, digest, &hash_error);
#endif
        if (!hashed && s.cached_prepare) return false;
        if (!hashed) digest.clear();
        s.disc_path = disc_path;
        s.disc_sha256 = std::move(digest);
    }
    if (!mod_runtime_prepare_resources(disc_path, error)) return false;
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    inputs.stable = dependency_files(s, disc_path, inputs.dependencies);
#else
    inputs.stable = false;
#endif
    ModResolution plan;
    {
        HostLaunchTimingScope resolve_timing(HOST_LAUNCH_RESOLVE);
        plan = s.manager.resolve(s.game_id, s.exe_sha256, s.disc_sha256);
        if (plan.ok) resolve_timing.success();
    }
    const bool ok = install_plan(s, std::move(plan), save_selection,
                                 !save_selection, error, &inputs);
    if (ok) timing.success();
    return ok;
}

bool mod_runtime_prepare_for_launcher(const std::filesystem::path& disc_path, std::string* error) {
    ScopedFlag flag(state().provider_preparing);
    return mod_runtime_commit(disc_path, error, true);
}

bool mod_runtime_try_prepare_cached(const std::filesystem::path& disc_path) {
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    try {
        auto& s = state();
        if (!s.initialized || s.main_applied || !session_plan_fp().empty()) return false;
        auto probe = s.manager;
        if (!probe.prepare_resources(s.game_id, disc_path, media_cache_root(), nullptr, true)) return false;
        ScopedFlag flag(s.cached_prepare);
        const bool ok = mod_runtime_commit(disc_path, nullptr, true);
        if (ok && s.ticket_ready) psx_mod_counter_add("startup.preload_hit", 1);
        return ok && s.ticket_ready;
    } catch (...) {
        // Preload is best effort before the launcher exists. The ordinary
        // provider path reports full-preparation errors once UI is available.
        state().ticket_ready = false;
        return false;
    }
#else
    (void)disc_path;
    return false;
#endif
}

void mod_runtime_set_session_plan_fp(const std::string& fp) {
    state().ticket_ready = false;
    ++state().preparation_revision;
    session_plan_fp() = fp;
}

const std::string& mod_runtime_session_plan_fp() {
    return session_plan_fp();
}

std::string mod_runtime_plan_fingerprint_portable() {
    RuntimeMods& s = state();
    if (!s.initialized) return {};
    ModResolution plan = s.manager.resolve(s.game_id, s.exe_sha256, std::string());
    if (!plan.ok || !netplay_resources_verified(plan)) return {};
    return plan.fingerprint;
}

bool mod_runtime_verify_session_plan_fp(std::string* error) {
    const std::string& want = session_plan_fp();
    if (want.empty()) return true;
    const std::string got = mod_runtime_plan_fingerprint_portable();
    if (!got.empty() && got == want) return true;
    if (error) {
        *error = "this machine resolves the session's mods differently from the host";
        if (!got.empty()) *error += " (host " + want.substr(0, 16) + ", here " + got.substr(0, 16) + ")";
    }
    return false;
}

bool mod_runtime_commit_for_netplay(const std::filesystem::path& disc_path,
                                    std::string* error) {
    RuntimeMods& s = state();
    const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
    if (!caps || !caps->valid)
        return mod_runtime_commit_for_direct_netplay(disc_path, error);
    if (caps->mod_count <= 0) {
        session_plan_fp().clear();
        return mod_runtime_clear_for_netplay(error);
    }
    if (!caps->mod_plan_fp[0]) {
        if (error) *error = "Netplay mod plan is missing its fingerprint.";
        return false;
    }
    ModPackageManager prior_manager = s.manager;
    const std::string prior_fp = session_plan_fp();
    mod_runtime_set_session_plan_fp(caps->mod_plan_fp);
    if (!apply_host_mod_plan(*caps, error) ||
        !mod_runtime_commit(disc_path, error, false)) {
        s.manager = std::move(prior_manager);
        mod_runtime_set_session_plan_fp(prior_fp);
        return false;
    }
    if (!offline_manager_before_netplay)
        offline_manager_before_netplay = std::move(prior_manager);
    return true;
}

bool mod_runtime_commit_for_direct_netplay(const std::filesystem::path& disc_path,
                                           std::string* error) {
    RuntimeMods& s = state();
    /* No mod runtime (no mods root): vanilla on every peer, no content gate. */
    if (!s.initialized) {
        session_plan_fp().clear();
        return true;
    }
    ModPackageManager prior_manager = s.manager;
    const std::string prior_fp = session_plan_fp();
    session_plan_fp().clear();
    // Resource preparation and immutable-byte verification are shared with
    // online sessions. No host selection or generated paths are saved offline.
    if (!mod_runtime_commit(disc_path, error, false)) {
        s.manager = std::move(prior_manager);
        session_plan_fp() = prior_fp;
        return false;
    }
    const std::string fp = mod_runtime_plan_fingerprint_portable();
    if (fp.size() != 64) {
        if (error) *error = "LAN mods did not produce a verified content fingerprint.";
        s.manager = std::move(prior_manager);
        session_plan_fp() = prior_fp;
        return false;
    }
    session_plan_fp() = fp;
    if (!offline_manager_before_netplay)
        offline_manager_before_netplay = std::move(prior_manager);
    return true;
}

void mod_runtime_end_netplay() {
    if (!offline_manager_before_netplay) return;
    RuntimeMods& s = state();
    mod_runtime_clear_for_netplay();
    s.manager = std::move(*offline_manager_before_netplay);
    offline_manager_before_netplay.reset();
    session_plan_fp().clear();
    s.validation = s.manager.resolve(s.game_id, s.exe_sha256, s.disc_sha256, true);
}

std::vector<std::string> mod_runtime_netplay_view_features(const ModResolution& plan) {
    /* A feature stays on online only when everything it contributes is a
     * local-view plugin: no EXE/disc write, overlay or derived disc (those
     * change the shared game) and no plugin without netplay = "local_view". */
    std::map<std::string, bool> ok;
    for (const ModResolution::Plugin& p : plan.plugins) {
        const std::string key = p.package_id + "/" + p.feature_id;
        auto it = ok.find(key);
        const bool good = p.netplay_local_view || p.netplay_input;
        ok[key] = it == ok.end() ? good : (it->second && good);
    }
    for (const ModResolution::Write& w : plan.writes)
        ok[w.package_id + "/" + w.feature_id] = false;
    for (const ModResolution::Overlay& o : plan.overlays)
        ok[o.package_id + "/" + o.feature_id] = false;
    for (const ModResolution::DerivedDisc& d : plan.derived_discs)
        ok[d.package_id + "/*"] = false;
    std::vector<std::string> out;
    for (const auto& [key, good] : ok) {
        if (!good) continue;
        const std::string pkg = key.substr(0, key.find('/'));
        if (ok.count(pkg + "/*")) continue;
        out.push_back(key);
    }
    return out;
}

bool mod_runtime_netplay_input_active() {
    const RuntimeMods& s = state();
    if (!s.netplay_view_plan || !s.plan.ok) return false;
    for (const ModResolution::Plugin& p : s.plan.plugins)
        if (p.netplay_input) return true;
    return false;
}

bool mod_runtime_netplay_view_active() {
    const RuntimeMods& s = state();
    return s.netplay_view_plan && s.plan.ok && !s.plan.plugins.empty();
}

bool mod_runtime_commit_netplay_view(const std::filesystem::path& disc_path,
                                     std::string* error) {
    RuntimeMods& s = state();
    std::string why;
    (void)mod_runtime_clear_for_netplay(&why);
    if (!s.initialized) return true;
    note_disc(s, disc_path);
    const ModResolution full =
        s.manager.resolve(s.game_id, s.exe_sha256, s.disc_sha256);
    if (!full.ok) return true; /* the player's set does not resolve: vanilla */
    const std::vector<std::string> keep = mod_runtime_netplay_view_features(full);
    if (keep.empty()) return true;
    /* The player's selection with every other feature off. */
    std::map<std::string, ModSelection> reduced = s.manager.selections();
    for (const auto& [id, versions] : s.manager.packages()) {
        (void)versions;
        const ModPackage* package = s.manager.selected_package(id);
        if (!package) continue;
        for (const ModFeature& f : package->features) {
            if (f.legacy) continue;
            const bool on = std::find(keep.begin(), keep.end(),
                                      id + "/" + f.id) != keep.end();
            ModFeatureSelection& fs = reduced[id].features[f.id];
            if (!on) { fs.enabled = false; fs.has_enabled = true; }
        }
    }
    std::map<std::string, ModSelection> player =
        s.manager.exchange_selections(std::move(reduced));
    ModResolution plan = s.manager.resolve(s.game_id, s.exe_sha256, s.disc_sha256);
    (void)s.manager.exchange_selections(std::move(player));
    if (!install_plan(s, std::move(plan), /*save*/ false, /*verify_netplay*/ false, &why)) {
        std::fprintf(stderr, "psxrecomp: netplay own-view mods unavailable (%s)\n",
                     why.c_str());
        return mod_runtime_clear_for_netplay(error);
    }
    s.netplay_view_plan = true;
    for (const std::string& k : keep)
        std::fprintf(stdout, "psxrecomp: netplay keeps %s for this player's own "
                     "view only\n", k.c_str());
    if (error) error->clear();
    return true;
}
const std::string& mod_runtime_fingerprint() {
    return state().plan.fingerprint;
}

const std::filesystem::path& mod_runtime_effective_disc_path() {
    return state().effective_disc_path;
}

#if defined(RECOMP_LAUNCHER)
const RecompLauncherCModProvider* mod_runtime_launcher_provider() {
#if defined(RECOMP_LAUNCHER_HAS_WORKER_MOD_COMMIT) && defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    provider.commit_worker_safe = 1;
#if defined(RECOMP_LAUNCHER_HAS_PREPARED_MOD_COMMIT)
    provider.try_commit = [](void*, const char* path) -> int {
        return prepared_ticket_matches(path ? std::filesystem::path(path) : std::filesystem::path()) ? 1 : 0;
    };
    provider.preparation_revision = [](void*) -> unsigned long long { return state().preparation_revision; };
#endif
#endif
    return &provider;
}

void mod_runtime_set_hide_hidden_features(bool hide) {
    provider.hide_hidden_features = hide ? 1 : 0;
}
#endif

} // namespace PSXRecompV4

extern "C" void mod_runtime_on_dispatch(uint32_t target) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || s.main_applied ||
        (target & 0x1FFFFFFFu) != s.entry_phys) return;

    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        for (size_t i = 0; i < write.expected.size(); ++i) {
            if (psx_read_byte((uint32_t)write.location + (uint32_t)i) !=
                write.expected[i]) {
                std::fprintf(stderr,
                    "psxrecomp: mod plan %s rejected at 0x%08X "
                    "(expected-byte guard failed; booting unmodified)\n",
                    s.plan.fingerprint.c_str(),
                    (unsigned)((uint32_t)write.location + (uint32_t)i));
                s.main_applied = true;
                return;
            }
        }
    }
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        apply_main_write(write);
    }
    s.main_applied = true;
    if (!s.plan.writes.empty())
        std::fprintf(stdout, "psxrecomp: applied mod plan %s\n",
                     s.plan.fingerprint.c_str());
}

extern "C" void mod_runtime_on_savestate_loaded(void) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok || s.netplay_view_plan) return;

    if (!s.main_applied) {
        uint32_t failed_at = 0;
        if (!restored_main_matches_plan(s, failed_at)) {
            std::fprintf(stderr,
                "psxrecomp: mod plan %s rejected after savestate restore at "
                "0x%08X (expected-byte guard failed)\n",
                s.plan.fingerprint.c_str(), (unsigned)failed_at);
            return;
        }
    }

    bool applied = false;
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        apply_main_write(write);
        applied = true;
    }
    s.main_applied = true;
    for (const ModResolution::Plugin& plugin : s.plan.plugins) {
        s.current_plugin = &plugin;
        mod_invoke_savestate_plugin(plugin.id);
        s.current_plugin = nullptr;
    }
    if (applied)
        std::fprintf(stdout,
            "psxrecomp: reapplied mod plan %s after savestate restore\n",
            s.plan.fingerprint.c_str());
}

extern "C" void mod_runtime_enable_disc_patches(void) {
    PSXRecompV4::state().disc_enabled = true;
}

extern "C" int psx_mod_append_disc_extent(const char* resource_id,
        uint64_t byte_offset, uint32_t sector_count, uint32_t* first_lba) {
    using namespace PSXRecompV4;
    if (first_lba) *first_lba = 0;
    auto& s = state();
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    if (!s.activating || !first_lba || !sector_count ||
        !psx_mod_current_resource_bytes(resource_id, &data, &size) ||
        byte_offset > size || uint64_t(sector_count)*2336 > size-byte_offset) return 0;
    try {
        uint32_t start = s.extent_start;
        if (s.disc_extents.empty()) {
            PS1::ISOReader reader;
            const auto& mount = s.effective_disc_path.empty() ? s.disc_path : s.effective_disc_path;
            if (mount.empty() || !reader.Open(mount.string()) || reader.TrackCount() >= 99) return 0;
            start = reader.GetSectorCount();
            if (!start) return 0;
        }
        const uint32_t lba = s.disc_extents.empty() ? start :
            s.disc_extents.back().lba + s.disc_extents.back().count;
        if (uint64_t(lba)+sector_count+150 > 450000) return 0;
        // Mode-2's duplicated subheader is part of the format contract.
        for (uint32_t i=0; i<sector_count; ++i) {
            const auto* sector = data+byte_offset+uint64_t(i)*2336;
            if (std::memcmp(sector,sector+4,4)) return 0;
        }
        s.disc_extents.push_back({lba,sector_count,data+byte_offset});
        s.extent_start = start;
        *first_lba = lba;
        return 1;
    } catch (...) { return 0; }
}

extern "C" uint32_t mod_runtime_disc_extent_start(void) {
    const auto& s=PSXRecompV4::state();
    return s.disc_enabled && !s.disc_extents.empty() ? s.extent_start : 0;
}

static int append_audio_track(uint32_t count, uint32_t source,
        const uint8_t* data, uint32_t* first_lba) {
    auto& s=PSXRecompV4::state();
    if(first_lba) *first_lba=0;
    if(!s.activating || !first_lba || !count || s.audio_tracks.size()>=98) return 0;
    const uint32_t start=s.audio_tracks.empty()?150:
        s.audio_tracks.back().lba+s.audio_tracks.back().count;
    if(uint64_t(start)+count+150>=450000) return 0;
    s.audio_tracks.push_back({start,count,source,data});
    *first_lba=start;
    return 1;
}
static int append_resource_audio_track(const char* resource_id,
        uint64_t offset, uint32_t count, uint32_t* first_lba) {
    if(first_lba) *first_lba=0;
    const uint8_t* data=nullptr; uint64_t size=0;
    if(!PSXRecompV4::state().activating || !first_lba || !count ||
       !psx_mod_current_resource_bytes(resource_id,&data,&size) ||
       offset>size || uint64_t(count)*2352>size-offset) return 0;
    return append_audio_track(count,0,data+offset,first_lba);
}
static int append_disc_audio_track(uint32_t track,
        uint32_t* first_lba, uint32_t* sector_count) {
    if(first_lba) *first_lba=0;
    if(sector_count) *sector_count=0;
    auto& s=PSXRecompV4::state();
    if(!s.activating || !first_lba || !sector_count) return 0;
    try {
        PS1::ISOReader reader;
        const auto& mount=s.effective_disc_path.empty()?s.disc_path:s.effective_disc_path;
        if(!reader.Open(mount.string()) || track<1 || track>uint32_t(reader.TrackCount()) ||
           !reader.TrackIsAudio(track)) return 0;
        const auto start=reader.TrackStartLBA(track);
        const auto end=track<uint32_t(reader.TrackCount())?
            reader.TrackPregapLBA(track+1):reader.GetSectorCount();
        if(end<=start || !append_audio_track(end-start,start,nullptr,first_lba)) return 0;
        *sector_count=end-start; return 1;
    } catch(...) { return 0; }
}
extern "C" int psx_mod_append_cdda_tracks(const PSXModCDDATrack* tracks,
        uint32_t count, uint32_t* first_lbas, uint32_t* sector_counts) {
    if(!count || count>98 || !first_lbas || !sector_counts) return 0;
    std::fill_n(first_lbas,count,0);std::fill_n(sector_counts,count,0);
    auto& s=PSXRecompV4::state();
    if(!s.activating || !tracks || s.audio_tracks.size()+count>98) return 0;
    const auto previous=s.audio_tracks.size();
    bool success=true;
    try {
        for(uint32_t i=0;i<count && success;++i) {
            const auto& t=tracks[i];
            if(t.resource_id) {
                success=!t.disc_track && append_resource_audio_track(t.resource_id,
                    t.byte_offset,t.sector_count,&first_lbas[i]);
                if(success)sector_counts[i]=t.sector_count;
            }else success=!t.byte_offset && !t.sector_count &&
                append_disc_audio_track(t.disc_track,&first_lbas[i],&sector_counts[i]);
        }
    }catch(...){success=false;}
    if(!success){
        s.audio_tracks.resize(previous);
        std::fill_n(first_lbas,count,0);std::fill_n(sector_counts,count,0);
    }
    return success;
}
extern "C" int mod_runtime_cdda_track_count(void) {
    const auto& s=PSXRecompV4::state();
    return s.disc_enabled && !s.audio_tracks.empty()?int(s.audio_tracks.size()+1):0;
}
extern "C" uint32_t mod_runtime_cdda_track_start(int track) {
    if(!mod_runtime_cdda_track_count()) return 0;
    const auto& tracks=PSXRecompV4::state().audio_tracks;
    if(track==0) return tracks.back().lba+tracks.back().count;
    return track>=2 && size_t(track-2)<tracks.size()?tracks[track-2].lba:0;
}
extern "C" int mod_runtime_read_cdda_sector(uint32_t lba, uint8_t* bytes,
        uint32_t size, uint32_t* source_lba) {
    if(!bytes || size<2352 || !source_lba || !mod_runtime_cdda_track_count()) return 0;
    for(const auto& t:PSXRecompV4::state().audio_tracks) if(lba>=t.lba && lba-t.lba<t.count) {
        if(t.data) { std::memcpy(bytes,t.data+uint64_t(lba-t.lba)*2352,2352); return 1; }
        *source_lba=t.source_lba+lba-t.lba; return 2;
    }
    return 0;
}
extern "C" uint32_t mod_runtime_disc_sector_count(uint32_t base_count) {
    const auto& s=PSXRecompV4::state();
    return mod_runtime_disc_extent_start() ?
        std::max(base_count,s.disc_extents.back().lba+s.disc_extents.back().count) : base_count;
}
extern "C" int mod_runtime_read_disc_extent(uint32_t lba, int raw,
        uint8_t* bytes, uint32_t size) {
    if (!bytes || size < (raw ? 2352u : 2048u) || !mod_runtime_disc_extent_start()) return 0;
    const auto& extents=PSXRecompV4::state().disc_extents;
    for (const auto& e:extents) if (lba>=e.lba && lba-e.lba<e.count) {
        const auto* sector=e.data+uint64_t(lba-e.lba)*2336;
        if (raw) {
            std::memset(bytes,0,16);std::memset(bytes+1,0xff,10);
            const uint32_t msf=lba+150;
            const auto bcd=[](uint32_t v) {return uint8_t((v/10)*16+v%10);};
            bytes[12]=bcd(msf/4500);bytes[13]=bcd(msf/75%60);bytes[14]=bcd(msf%75);bytes[15]=2;
            std::memcpy(bytes+16,sector,2336);
        } else std::memcpy(bytes,sector+8,2048);
        return 1;
    }
    return 0;
}

static void patch_committed_disc_sector(uint32_t lba, int raw_sector,
                                        uint8_t* bytes, uint32_t size);

/* The largest file a CD can hold (80-minute disc, 360,000 sectors). Mods may
 * grow an archive past its original extent, e.g. into a padding file. */
static constexpr uint32_t kMaxDiscFileBytes = 360000u * 2048u;

/* One 2048-byte user-data sector of the effective disc: the committed plan's
 * raw and user-data writes/overlays applied. Form 2 (XA) sectors are refused. */
static bool read_effective_user_sector(PS1::ISOReader& reader, uint32_t lba,
                                       uint8_t* sector) {
    uint8_t raw[2352];
    if (reader.ReadRawSector(lba, raw)) {
        if (raw[15] != 1 && (raw[15] != 2 || (raw[18] & 0x20u))) return false;
        patch_committed_disc_sector(lba, 1, raw, sizeof raw);
        std::memcpy(sector, raw + (raw[15] == 1 ? 16 : 24), 2048);
        if (raw[15] == 1) patch_committed_disc_sector(lba, 0, sector, 2048);
    } else {
        if (!reader.ReadSector(lba, sector)) return false;
        patch_committed_disc_sector(lba, 0, sector, 2048);
    }
    return !PSXRecompV4::state().disc_guard_failed;
}

/* ISO 9660 path lookup through the effective directory records. Mods may
 * relocate or grow a file by patching its directory record (e.g. an archive
 * extended into the following padding file); the emulated drive serves those
 * patched records, so host reads must resolve paths through them too. Names
 * compare case-insensitively without the ";1" version suffix. */
static bool find_effective_file(PS1::ISOReader& reader, const std::string& path,
                                uint32_t& lba, uint32_t& bytes, bool& directory) {
    uint8_t sector[2048];
    auto le32 = [](const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    };
    if (!read_effective_user_sector(reader, 16, sector) || sector[0] != 1 ||
        std::memcmp(sector + 1, "CD001", 5) != 0) return false;
    lba = le32(sector + 156 + 2);
    bytes = le32(sector + 156 + 10);
    directory = true;
    size_t at = 0;
    while (at < path.size()) {
        size_t next = path.find_first_of("/\\", at);
        if (next == std::string::npos) next = path.size();
        std::string want = path.substr(at, next - at);
        at = next + 1;
        if (want.empty()) continue;
        if (!directory) return false;
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return (char)std::toupper(c); });
        bool found = false;
        const uint32_t sectors = (bytes + 2047u) / 2048u;
        for (uint32_t i = 0; i < sectors && !found; ++i) {
            if (!read_effective_user_sector(reader, lba + i, sector)) return false;
            for (uint32_t p = 0; p < 2048u && sector[p] && !found;) {
                const uint32_t length = sector[p];
                if (length < 34 || p + length > 2048u ||
                    33u + sector[p + 32] > length) return false;
                std::string name(reinterpret_cast<const char*>(sector + p + 33),
                                 sector[p + 32]);
                name = name.substr(0, name.find(';'));
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return (char)std::toupper(c); });
                if (sector[p + 32] == 1 && (sector[p + 33] == 0 || sector[p + 33] == 1))
                    name.clear();
                if (!name.empty() && name == want) {
                    lba = le32(sector + p + 2);
                    bytes = le32(sector + p + 10);
                    directory = (sector[p + 25] & 2u) != 0;
                    found = true;
                }
                p += length;
            }
        }
        if (!found) return false;
    }
    return true;
}

extern "C" int psx_mod_read_disc_file(const char* path, void* buffer,
                                      uint32_t capacity, uint32_t* size) {
    using namespace PSXRecompV4;
    if (size) *size = 0;
    if (!path || !*path || !size || (!buffer && capacity)) return 0;
    try {
        const auto& s = state();
        const auto& mount = s.effective_disc_path.empty() ? s.disc_path : s.effective_disc_path;
        if (mount.empty()) return 0;
        PS1::ISOReader reader;
        uint32_t lba = 0, bytes = 0;
        bool directory = false;
        if (!reader.Open(mount.string()) ||
            !find_effective_file(reader, path, lba, bytes, directory) ||
            directory || !bytes || bytes > kMaxDiscFileBytes)
            return 0;
        if (!buffer) { *size = bytes; return 1; }
        if (capacity < bytes) return 0;
        uint8_t sector[2048];
        for (uint32_t offset = 0; offset < bytes; offset += 2048u) {
            if (!read_effective_user_sector(reader, lba + offset / 2048u, sector)) return 0;
            const uint32_t count = std::min(2048u, bytes - offset);
            std::memcpy(static_cast<uint8_t*>(buffer) + offset, sector, count);
        }
        *size = bytes;
        return 1;
    } catch (...) { return 0; }
}

extern "C" int psx_mod_disc_file_extent(const char* path, uint32_t* lba,
                                        uint32_t* size) {
    using namespace PSXRecompV4;
    if (lba) *lba = 0;
    if (size) *size = 0;
    if (!path || !*path || !lba || !size) return 0;
    try {
        const auto& s = state();
        const auto& mount = s.effective_disc_path.empty() ? s.disc_path : s.effective_disc_path;
        if (mount.empty()) return 0;
        PS1::ISOReader reader;
        uint32_t first = 0, bytes = 0;
        bool directory = false;
        if (!reader.Open(mount.string()) ||
            !find_effective_file(reader, path, first, bytes, directory) ||
            directory || !bytes || bytes > kMaxDiscFileBytes)
            return 0;
        *lba = first;
        *size = bytes;
        return 1;
    } catch (...) { return 0; }
}

namespace PSXRecompV4 {
/* Whole effective sectors, including the bytes after end-of-file in the last
 * sector: a sector-granular read by the game receives exactly these. */
bool mod_runtime_read_disc_file_sectors(const std::string& path, uint32_t max_bytes,
                                        std::vector<uint8_t>& padded, uint32_t& lba,
                                        uint32_t& size, std::string* error) {
    padded.clear();
    lba = size = 0;
    auto fail = [&](const std::string& why) {
        if (error) *error = path + ": " + why;
        padded.clear();
        return false;
    };
    try {
        const auto& s = state();
        const auto& mount = s.effective_disc_path.empty() ? s.disc_path : s.effective_disc_path;
        if (mount.empty()) return fail("no disc mounted");
        PS1::ISOReader reader;
        bool directory = false;
        if (!reader.Open(mount.string())) return fail("cannot open disc");
        if (!find_effective_file(reader, path, lba, size, directory) || directory)
            return fail("not found on the disc");
        if (!size || size > kMaxDiscFileBytes || (max_bytes && size > max_bytes))
            return fail("unsupported size " + std::to_string(size));
        const uint32_t sectors = (size + 2047u) / 2048u;
        padded.resize(size_t(sectors) * 2048u);
        for (uint32_t i = 0; i < sectors; ++i)
            if (!read_effective_user_sector(reader, lba + i, padded.data() + size_t(i) * 2048u))
                return fail("unreadable sector " + std::to_string(lba + i));
        return true;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
} // namespace PSXRecompV4

extern "C" void mod_runtime_activate_plugins(void) {
    using namespace PSXRecompV4;
    HostLaunchTimingScope timing(HOST_LAUNCH_PLUGIN_ACTIVATION);
    RuntimeMods& s = state();
    s.ticket_ready = false;
    psx_projection_reset_session();
    gpu_ws_set_native_scene_predicate(nullptr);
    psx_ram_reset_size_request();
    psx_mod_clear_function_replacements();
    gpu_hd_textures_shutdown();
    if (!s.initialized || !s.plan.ok) { timing.success(); return; }
    s.disc_extents.clear();
    s.audio_tracks.clear();
    s.extent_start = 0;
    s.activating = true;
    for (const ModResolution::Plugin& plugin : s.plan.plugins) {
        HostLaunchTimingScope plugin_timing(HOST_LAUNCH_PLUGIN_CALLBACK,
            plugin.package_id.c_str(), plugin.feature_id.c_str());
        PluginCallbackScope scope(s, &plugin);
        mod_invoke_activation_plugin(plugin.id);
        plugin_timing.success();
    }
    s.activating = false;
    build_function_entry_hooks(s);
    timing.success();
}

extern "C" void mod_runtime_on_vblank(void) {
    using namespace PSXRecompV4;
    ++g_mod_vblanks;
    RuntimeMods& s = state();
    /* An own-view plan never touches the shared simulation. */
    if (!s.initialized || !s.plan.ok || s.netplay_view_plan) return;
    for (const ModResolution::Plugin& plugin : s.plan.plugins) {
        PluginCallbackScope scope(s, &plugin);
        mod_invoke_vblank_plugin(plugin.id);
    }
}

extern "C" int psx_mod_game_started(void) {
    return fntrace_is_game_started();
}

extern "C" int psx_mod_option_value(const char* package_id,
                                    const char* feature_id,
                                    const char* option_id,
                                    char* out, uint32_t out_size) {
    using namespace PSXRecompV4;
    if (out && out_size) out[0] = '\0';
    if (!package_id || !feature_id || !option_id || !out || out_size == 0)
        return 0;
    RuntimeMods& s = state();
    /* Activation runs after the final plan commit, so a committed plan is the
     * precondition for a meaningful answer. Without one there is no selection
     * to read and the caller must fall back to its own default rather than
     * treat an empty string as a value. */
    if (!s.initialized || !s.plan.ok) return 0;
    /* Read the committed plan's selection, which includes features its
     * [[requirement]]s activated, not live launcher state. */
    const std::string value = s.manager.feature_option_value(
        s.plan, package_id, feature_id, option_id);
    if (value.empty()) return 0;
    if (value.size() + 1 > (size_t)out_size) return 0;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return 1;
}

extern "C" int psx_mod_current_resource_path(const char* resource_id,
                                             char* out, uint32_t out_size) {
    using namespace PSXRecompV4;
    if (out && out_size) out[0] = '\0';
    if (!resource_id || !resource_id[0] || !out || out_size == 0)
        return 0;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok || !s.current_plugin) return 0;
    for (const ModResolution::Resource& resource : s.plan.resources) {
        if (resource.package_id != s.current_plugin->package_id ||
            resource.feature_id != s.current_plugin->feature_id ||
            resource.id != resource_id)
            continue;
        const std::string text = resource.path.u8string();
        if (text.empty() || text.size() + 1 > (size_t)out_size) return 0;
        std::memcpy(out, text.c_str(), text.size() + 1);
        return 1;
    }
    return 0;
}

extern "C" int psx_mod_current_option_value(const char* option_id,
                                             char* out, uint32_t out_size) {
    using namespace PSXRecompV4;
    if (out && out_size) out[0] = '\0';
    const auto* plugin = state().current_plugin;
    if (!plugin) return 0;
    return psx_mod_option_value(plugin->package_id.c_str(), plugin->feature_id.c_str(),
                               option_id, out, out_size);
}

extern "C" int psx_mod_set_hd_texture_pack(const char* resource_id,
                                           int replacements_enabled, int dump_enabled) {
    char root[4096] = "";
    char error[512] = "";
    if (!psx_mod_current_resource_path(resource_id, root, sizeof(root))) {
        std::fprintf(stderr, "psxrecomp: HD textures require a selected pack folder\n");
        return 0;
    }
    if (!gpu_hd_textures_configure(root, replacements_enabled, dump_enabled,
                                  error, sizeof(error))) {
        std::fprintf(stderr, "psxrecomp: HD textures: %s\n", error);
        return 0;
    }
    std::fprintf(stdout, "psxrecomp: HD textures: %s (replacements %s, dumping %s)\n",
                 root, replacements_enabled ? "on" : "off", dump_enabled ? "on" : "off");
    return 1;
}

extern "C" int psx_mod_set_hd_texture_dump(int enabled) {
    GpuHdTextureDiag info{};
    gpu_hd_textures_get_diag(&info);
    if (!info.root || !info.root[0]) return 0;
    gpu_hd_textures_set_dump_enabled(enabled);
    return 1;
}

extern "C" int psx_mod_reload_hd_texture_pack(void) {
    char error[512] = "";
    const int ok = gpu_hd_textures_reload(error, sizeof(error));
    if (!ok) std::fprintf(stderr, "psxrecomp: HD textures: %s\n", error);
    return ok;
}

extern "C" uint8_t psx_mod_read_byte(uint32_t address) {
    return psx_read_byte(address);
}

extern "C" int psx_mod_current_resource_bytes(const char* resource_id,
                                               const uint8_t** bytes,
                                               uint64_t* size) {
    using namespace PSXRecompV4;
    if (bytes) *bytes = nullptr;
    if (size) *size = 0;
    if (!resource_id || !*resource_id || !bytes || !size) return 0;
    const auto& s = state();
    if (!s.initialized || !s.plan.ok || !s.current_plugin) return 0;
    for (const auto& resource : s.plan.resources) {
        if (resource.package_id == s.current_plugin->package_id &&
            resource.feature_id == s.current_plugin->feature_id &&
            resource.id == resource_id && resource.bytes) {
            *bytes = resource.bytes->data();
            *size = resource.bytes->size();
            return 1;
        }
    }
    return 0;
}

extern "C" void psx_mod_write_byte(uint32_t address, uint8_t value) {
    psx_host_write_byte(address, value);
}

extern "C" uint16_t psx_mod_read_half(uint32_t address) {
    return psx_read_half(address);
}

extern "C" void psx_mod_write_half(uint32_t address, uint16_t value) {
    psx_host_write_half(address, value);
}

extern "C" uint32_t psx_mod_read_word(uint32_t address) {
    return psx_read_word(address);
}

extern "C" void psx_mod_write_word(uint32_t address, uint32_t value) {
    psx_host_write_word(address, value);
}

extern "C" void psx_mod_write_code_word(uint32_t address, uint32_t value) {
    psx_host_write_word(address, value);
    dirty_ram_mark_executable_range(address & 0x1FFFFFFFu, 4u);
}

extern "C" uint32_t psx_mod_alloc_guest_memory(uint32_t size,
                                                uint32_t alignment) {
    return psx_mod_memory_alloc(size, alignment);
}

extern "C" uint32_t psx_mod_alloc_gpu_dma_memory(uint32_t size,
                                                  uint32_t alignment) {
    return psx_mod_gpu_dma_memory_alloc(size, alignment);
}

namespace {
struct ModCounter {
    const char* name = nullptr;   /* caller literal; compared by content */
    uint64_t count = 0;
    uint64_t last_frame = 0;
};
constexpr size_t kModCounterCap = 128;
ModCounter g_mod_counters[kModCounterCap];
size_t g_mod_counter_count = 0;
uint64_t g_mod_counter_overflow = 0;
}  // namespace

extern "C" void psx_mod_counter_add(const char* name, uint32_t delta) {
    if (!name || !name[0]) return;
    for (size_t i = 0; i < g_mod_counter_count; i++) {
        ModCounter& c = g_mod_counters[i];
        if (c.name == name || std::strcmp(c.name, name) == 0) {
            c.count += delta;
            c.last_frame = g_mod_vblanks;
            return;
        }
    }
    if (g_mod_counter_count == kModCounterCap) {
        g_mod_counter_overflow += delta;
        return;
    }
    ModCounter& c = g_mod_counters[g_mod_counter_count++];
    c.name = name;
    c.count = delta;
    c.last_frame = g_mod_vblanks;
}

/* Debug-server accessor: copies up to `cap` entries; returns the total count
 * of distinct counters and writes the overflow bucket. */
extern "C" int psx_mod_counters_snapshot(const char** names, uint64_t* counts,
                                         uint64_t* last_frames, int cap,
                                         uint64_t* overflow) {
    const int n = (int)g_mod_counter_count;
    for (int i = 0; i < n && i < cap; i++) {
        names[i] = g_mod_counters[i].name;
        counts[i] = g_mod_counters[i].count;
        last_frames[i] = g_mod_counters[i].last_frame;
    }
    if (overflow) *overflow = g_mod_counter_overflow;
    return n;
}

extern "C" int32_t psx_mod_widescreen_x_margin(void) {
    return (int32_t)psx_ws_x_margin();
}
extern "C" int32_t psx_mod_widescreen_view_x_margin(void) {
    return (int32_t)gpu_ws_configured_x_reveal();
}

extern "C" void psx_mod_tag_hud_primitive(uint32_t primitive, int edge) {
    gpu_ws_tag_hud_primitive(primitive, edge);
}
extern "C" void psx_mod_anchor_hud_primitive(uint32_t primitive, int edge) {
    gpu_ws_tag_hud_prim(primitive, edge);
}
extern "C" void psx_mod_tag_screen_mask_quad(uint32_t primitive) {
    gpu_ws_tag_screen_mask_quad(primitive);
}
extern "C" void psx_mod_tag_radial_screen_mask_quad(uint32_t primitive, float scale) {
    gpu_ws_tag_radial_screen_mask_quad(primitive, scale);
}

extern "C" void psx_mod_tag_world_primitive(uint32_t primitive, int is_world) {
    gpu_ws_tag_world_primitive(primitive, is_world);
}

extern "C" void psx_mod_set_adaptive_backdrop_preload(int enabled) {
    gpu_ws_set_adaptive_backdrop_preload(enabled);
}

/*
 * The presenter's own view of the scanned-out picture. Plugins that draw
 * overlay primitives need the real edge, and the visible width depends on the
 * GP1(06h) horizontal range, which GPUSTAT does not carry -- so a plugin
 * cannot derive this itself. Zero means "not established yet"; the header
 * tells callers to skip drawing rather than guess.
 */
extern "C" uint32_t psx_mod_display_width(void) {
    GpuDisplayInfo info;
    std::memset(&info, 0, sizeof(info));
    gpu_get_display_info(&info);
    return info.width;
}

extern "C" uint32_t psx_mod_display_height(void) {
    GpuDisplayInfo info;
    std::memset(&info, 0, sizeof(info));
    gpu_get_display_info(&info);
    return info.height;
}

extern "C" int psx_mod_register_function_entry_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback) {
    using namespace PSXRecompV4;
    if (!id || !address || !callback) return 0;
    return mod_register_function_entry_plugin(id, address, callback) ? 1 : 0;
}

extern "C" int psx_mod_register_function_filter_plugin(
    const char* id, uint32_t address, PSXModFunctionFilterCallback callback) {
    using namespace PSXRecompV4;
    if (!id || !address || !callback) return 0;
    return mod_register_function_filter_plugin(id, address, callback) ? 1 : 0;
}

extern "C" int psx_mod_finish_function(CPUState* cpu) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!cpu || s.current_function_cpu != cpu || !s.current_plugin) return 0;
    s.current_function_finished = true;
    return 1;
}

extern "C" int psx_mod_register_guest_function_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback) {
    return id && PSXRecompV4::mod_register_guest_function_plugin(id, address, callback);
}

extern "C" int psx_mod_local_view_scope(void);
/* A netplay own-view plan's hooks wait for the sandboxed own-view render; the
 * shared simulation never runs them. */
static int mod_plan_hooks_parked(void) {
    return PSXRecompV4::state().netplay_view_plan && !psx_mod_local_view_scope();
}

extern "C" int psx_mod_dispatch_guest_function(CPUState* cpu, uint32_t address) {
    using namespace PSXRecompV4;
    if (!g_psx_mod_guest_functions || !cpu || address >= 0xC0000000u) return 0;
    if (mod_plan_hooks_parked()) return 0;
    const auto& functions = active_guest_functions();
    const uint32_t key = function_entry_key(address);
    auto it = std::lower_bound(functions.begin(), functions.end(), key,
        [](const auto& function, uint32_t k) { return function.key < k; });
    if (it == functions.end() || it->key != key) return 0;
    PluginCallbackScope scope(state(), it->plugin, cpu);
    it->callback(cpu, address);
    cpu->pc = cpu->gpr[31];
    return 1;
}

extern "C" int psx_mod_register_instruction_plugin(const char* id, uint32_t address,
                                                    uint32_t expected, PSXModFunctionEntryCallback callback) {
    return id && PSXRecompV4::mod_register_instruction_plugin(id, address, expected, callback);
}

extern "C" void psx_mod_instruction(CPUState* cpu, uint32_t address, uint32_t instruction) {
    using namespace PSXRecompV4;
    if (!g_psx_mod_instruction_hooks || !cpu || address >= 0xC0000000u) return;
    if (mod_plan_hooks_parked()) return;
    const auto& hooks = active_instruction_hooks();
    const auto key = function_entry_key(address);
    auto it = std::lower_bound(hooks.begin(), hooks.end(), key,
                              [](const auto& h, uint32_t k) { return h.key < k; });
    for (; it != hooks.end() && it->key == key; ++it) {
        if (it->expected != instruction || psx_mod_read_word(address) != instruction) continue;
        PluginCallbackScope scope(state(), it->plugin, nullptr);
        const uint32_t pc = cpu->pc;
        it->callback(cpu, address);
        if (cpu->pc != pc) std::abort();
        cpu->gpr[0] = 0;
    }
}

static int game_netplay_function_entry(CPUState* cpu, uint32_t address);

extern "C" int psx_mod_function_entry(CPUState* cpu, uint32_t address) {
    using namespace PSXRecompV4;
    if (!g_psx_mod_function_entry_hooks || !cpu) return 0;
    RuntimeMods& s = state();
    if (mod_plan_hooks_parked()) return game_netplay_function_entry(cpu, address);
    const auto& table = active_function_entry_hooks();
    const uint32_t key = function_entry_key(address);
    auto it = std::lower_bound(
        table.begin(), table.end(), key,
        [](const ActiveFunctionEntryHook& hook, uint32_t k) { return hook.key < k; });
    for (; it != table.end() && it->key == key; ++it) {
        PluginCallbackScope scope(s, it->plugin, cpu);
        ++function_entry_depth;
        if (it->callback) it->callback(cpu, address);
        /* Either completion form skips the body: an entry callback that
         * called psx_mod_finish_function(), or a filter returning nonzero. */
        const bool handled = s.current_function_finished ||
            (it->filter && it->filter(cpu, address));
        --function_entry_depth;
        if (handled) {
            cpu->pc = cpu->gpr[31];
            return 1;
        }
    }
    return game_netplay_function_entry(cpu, address);
}

static int register_game_netplay_hook(uint32_t address,
                                      PSXModFunctionEntryCallback callback,
                                      PSXModFunctionFilterCallback filter) {
    using namespace PSXRecompV4;
    if (!address || (!callback && !filter)) return 0;
    auto& table = game_netplay_entry_hooks();
    const uint32_t key = function_entry_key(address);
    for (const auto& hook : table)
        if (hook.key == key && hook.callback == callback && hook.filter == filter) return 1;
    table.push_back({key, callback, filter, nullptr});
    std::stable_sort(table.begin(), table.end(),
                     [](const ActiveFunctionEntryHook& a,
                        const ActiveFunctionEntryHook& b) { return a.key < b.key; });
    update_function_entry_count();
    return 1;
}

extern "C" int psx_game_register_netplay_function_entry(
    uint32_t address, PSXModFunctionEntryCallback callback) {
    return callback ? register_game_netplay_hook(address, callback, nullptr) : 0;
}

extern "C" int psx_game_register_netplay_function_filter(
    uint32_t address, PSXModFunctionFilterCallback filter) {
    return filter ? register_game_netplay_hook(address, nullptr, filter) : 0;
}

/* Game netplay hooks run only in a netplay session, after the mod plan's
 * hooks (which a match clears). PSX_GAME_FILTER_OFFLINE_PROBE=1 runs them
 * offline too, to test a title's netplay path without opening a session. */
static int game_netplay_function_entry(CPUState* cpu, uint32_t address) {
    using namespace PSXRecompV4;
    const auto& table = game_netplay_entry_hooks();
    if (table.empty()) return 0;
    if (!psx_netplay_active()) {
        static int offline_probe = -1;
        if (offline_probe < 0) {
            const char* e = std::getenv("PSX_GAME_FILTER_OFFLINE_PROBE");
            offline_probe = (e && std::strcmp(e, "1") == 0) ? 1 : 0;
        }
        if (!offline_probe) return 0;
    }
    const uint32_t key = function_entry_key(address);
    auto it = std::lower_bound(
        table.begin(), table.end(), key,
        [](const ActiveFunctionEntryHook& hook, uint32_t k) { return hook.key < k; });
    for (; it != table.end() && it->key == key; ++it) {
        if (it->callback) it->callback(cpu, address);
        if (it->filter && it->filter(cpu, address)) {
            cpu->pc = cpu->gpr[31];
            return 1;
        }
    }
    return 0;
}

extern "C" int psx_mod_function_entry_active(void) {
    return PSXRecompV4::function_entry_depth != 0;
}

extern "C" void mod_runtime_function_entry_context_save(ModFunctionEntryContext *out) {
    out->depth = PSXRecompV4::function_entry_depth;
    out->plugin = PSXRecompV4::state().current_plugin;
}

extern "C" void mod_runtime_function_entry_context_restore(const ModFunctionEntryContext *in) {
    PSXRecompV4::function_entry_depth = in->depth;
    PSXRecompV4::state().current_plugin =
        static_cast<const PSXRecompV4::ModResolution::Plugin *>(in->plugin);
}

/* Apply the committed plan's disc writes and overlays to one sector. The
 * emulated drive reaches this only after mod_runtime_enable_disc_patches()
 * (reference reads before then must see the original image); host-side
 * preparation through psx_mod_read_disc_file() always sees the effective disc,
 * including during plugin activation, which runs before the drive is enabled. */
static void patch_committed_disc_sector(uint32_t lba, int raw_sector,
                                        uint8_t* bytes, uint32_t size) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || s.disc_guard_failed || !bytes || size == 0) return;
    /* Raw Mode2 Form1 reads are also the source of the 2048-byte logical
     * stream consumed by the emulated CD controller. Apply raw claims to the
     * complete sector, then user-data claims to its payload window. Form2/XA
     * and CDDA sectors deliberately do not receive disc_user overlays. */
    const bool has_mode2_form1_user_data =
        raw_sector && size >= 2072 && bytes[15] == 2 &&
        (bytes[18] & 0x20u) == 0;
    const ModPatchTarget target =
        raw_sector ? ModPatchTarget::DiscRaw : ModPatchTarget::DiscUser;
    const uint64_t base = (uint64_t)lba * size;
    const uint64_t end = base + size;
    const auto& index = raw_sector ? s.raw_disc_index : s.user_disc_index;
    const auto sector = index.find(lba);
    const auto& overlay_index =
        raw_sector ? s.raw_overlay_index : s.user_overlay_index;
    const auto overlay_sector = overlay_index.find(lba);
    if (sector == index.end() && overlay_sector == overlay_index.end()) {
        if (has_mode2_form1_user_data)
            patch_committed_disc_sector(lba, 0, bytes + 24, 2048);
        return;
    }
    if (sector != index.end()) {
        for (size_t write_index : sector->second) {
            const ModResolution::Write& write = s.plan.writes[write_index];
            if (write.target != target || write.location < base ||
                write.location + write.expected.size() > end) continue;
            const size_t offset = (size_t)(write.location - base);
            if (std::memcmp(bytes + offset, write.expected.data(),
                            write.expected.size()) != 0) {
                std::fprintf(stderr,
                    "psxrecomp: disc mod plan %s rejected at LBA %u+%zu "
                    "(expected-byte guard failed; disc overlay disabled)\n",
                    s.plan.fingerprint.c_str(), lba, offset);
                s.disc_guard_failed = true;
                return;
            }
        }
        for (size_t write_index : sector->second) {
            const ModResolution::Write& write = s.plan.writes[write_index];
            if (write.target != target || write.location < base ||
                write.location + write.expected.size() > end) continue;
            const size_t offset = (size_t)(write.location - base);
            if (write.fields.empty()) {
                std::memcpy(bytes + offset, write.replacement.data(),
                            write.replacement.size());
            } else {
                for (const ModResolution::Write::Field& field :
                     write.fields)
                    std::memcpy(
                        bytes + offset +
                            static_cast<size_t>(field.offset),
                        field.replacement.data(),
                        field.replacement.size());
            }
        }
    }
    if (overlay_sector != overlay_index.end()) {
        for (size_t overlay_index_value : overlay_sector->second) {
            const ModResolution::Overlay& overlay =
                s.plan.overlays[overlay_index_value];
            const uint64_t overlay_end =
                overlay.location + overlay.payload.size();
            const uint64_t copy_begin = std::max(base, overlay.location);
            const uint64_t copy_end = std::min(end, overlay_end);
            if (copy_begin >= copy_end) continue;
            const size_t destination = (size_t)(copy_begin - base);
            const size_t source = (size_t)(copy_begin - overlay.location);
            const size_t count = (size_t)(copy_end - copy_begin);
            std::memcpy(bytes + destination,
                        overlay.payload.data() + source, count);
        }
    }
    if (has_mode2_form1_user_data && !s.disc_guard_failed)
        patch_committed_disc_sector(lba, 0, bytes + 24, 2048);
}

extern "C" void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                               uint8_t* bytes, uint32_t size) {
    if (!PSXRecompV4::state().disc_enabled) return;
    patch_committed_disc_sector(lba, raw_sector, bytes, size);
}
