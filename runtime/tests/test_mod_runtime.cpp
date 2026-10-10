#include "mod_runtime.h"
#include "host_launch_timing.h"
#include "host_file_identity.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "psx_sha256.h"
#include "psx_lobby_client.h"

#include "gpu.h"
#include "gpu_hd_textures.h"
#include "cpu_state.h"

#include <array>
#include <csetjmp>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::array<uint8_t, 2 * 1024 * 1024> ram;
static int failures;
static int activation_calls;
static int plugin_calls;
static int restore_calls;
static PsxLobbyMatchCaps test_match_caps;
extern "C" const PsxLobbyMatchCaps* psx_lobby_match_caps(void) {
    return &test_match_caps;
}
static int game_netplay_entry_hits;
static int game_netplay_filter_hits;
static int test_netplay_active;
extern "C" int psx_netplay_active(void) { return test_netplay_active; }
static void test_game_netplay_entry(CPUState*, uint32_t) {
    game_netplay_entry_hits++;
}
static int test_game_netplay_filter(CPUState* cpu, uint32_t) {
    game_netplay_filter_hits++;
    cpu->gpr[2] = 0x1234u;
    return 1;
}

extern "C" uint8_t psx_read_byte(uint32_t address) {
    return ram[address & 0x1fffffu];
}

extern "C" void psx_write_byte(uint32_t address, uint8_t value) {
    ram[address & 0x1fffffu] = value;
}

extern "C" uint16_t psx_read_half(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint16_t)(ram[offset] | ((uint16_t)ram[offset + 1] << 8));
}

extern "C" void psx_write_half(uint32_t address, uint16_t value) {
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
}

extern "C" uint32_t psx_read_word(uint32_t address) {
    const uint32_t offset = address & 0x1fffffu;
    return (uint32_t)ram[offset] |
           ((uint32_t)ram[offset + 1] << 8) |
           ((uint32_t)ram[offset + 2] << 16) |
           ((uint32_t)ram[offset + 3] << 24);
}

extern "C" void psx_write_word(uint32_t address, uint32_t value) {
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
    ram[offset + 2] = (uint8_t)(value >> 16);
    ram[offset + 3] = (uint8_t)(value >> 24);
}

/* Mod writes are host stores (memory.c psx_host_write_*). */
extern "C" void psx_host_write_byte(uint32_t address, uint8_t value) {
    psx_write_byte(address, value);
}

extern "C" void psx_host_write_half(uint32_t address, uint16_t value) {
    psx_write_half(address, value);
}

extern "C" void psx_host_write_word(uint32_t address, uint32_t value) {
    psx_write_word(address, value);
}

extern "C" uint32_t psx_mod_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t) {
    return 0;
}
extern "C" void psx_ram_reset_size_request(void) {}
static int hd_shutdown_calls, hd_configure_calls, hd_dump_value;
static int hd_replacements_value, hd_reload_calls;
static std::string hd_root;
extern "C" void gpu_hd_textures_shutdown(void) { ++hd_shutdown_calls; hd_root.clear(); }
extern "C" int gpu_hd_textures_configure(const char* root, int replacements, int dump,
                                          char*, size_t) {
    ++hd_configure_calls;
    hd_root = root;
    hd_replacements_value = replacements;
    hd_dump_value = dump;
    return 1;
}
extern "C" void gpu_hd_textures_set_dump_enabled(int enabled) {
    hd_dump_value = enabled;
}
extern "C" int gpu_hd_textures_active(void) {
    return !hd_root.empty() && (hd_replacements_value || hd_dump_value);
}
extern "C" void gpu_hd_textures_get_diag(GpuHdTextureDiag* out) {
    *out = {};
    out->root = hd_root.c_str();
}
extern "C" int gpu_hd_textures_reload(char*, size_t) { ++hd_reload_calls; return 1; }
static void test_hd_activation(void) {
    char replacements[16], dump[16];
    if (!psx_mod_current_option_value("replacements", replacements, sizeof(replacements)) ||
        !psx_mod_current_option_value("dump", dump, sizeof(dump))) { ++failures; return; }
    if (!psx_mod_set_hd_texture_pack("pack", std::string(replacements) == "true",
                                    std::string(dump) == "true")) ++failures;
}
extern "C" void psx_projection_reset_session(void) {}
extern "C" void gpu_ws_set_native_scene_predicate(int (*)(void)) {}
extern "C" int psx_ws_x_margin(void) { return 0; }

/* Stand-in for the GPU's display geometry. psx_mod_display_width/height must
 * report exactly what the presenter reports -- a plugin drawing an overlay
 * uses this as the screen edge, so a substituted or rounded value would put
 * HUD elements in the wrong place. */
static GpuDisplayInfo g_test_display;
extern "C" void gpu_get_display_info(GpuDisplayInfo* out) {
    *out = g_test_display;
}

extern "C" void dirty_ram_mark_executable_range(uint32_t, uint32_t) {}
extern "C" int fntrace_is_game_started(void) { return 1; }
/* Widescreen tag passthroughs mod_runtime forwards to the GPU; unused here. */
extern "C" void gpu_ws_tag_hud_primitive(uint32_t, int) {}
extern "C" void gpu_ws_tag_world_primitive(uint32_t, int) {}
extern "C" void gpu_ws_set_adaptive_backdrop_preload(int) {}
extern "C" int gpu_ws_configured_x_reveal(void) { return 0; }
extern "C" void gpu_ws_tag_hud_prim(uint32_t, int) {}
extern "C" void gpu_ws_tag_screen_mask_quad(uint32_t) {}
extern "C" void gpu_ws_tag_radial_screen_mask_quad(uint32_t, float) {}

static CPUState* interrupted_entry_cpu;
static void test_vblank_plugin(void) {
    plugin_calls++;
    if (interrupted_entry_cpu && psx_mod_finish_function(interrupted_entry_cpu)) failures++;
}

/* Function-entry hooks: one owned by the plan's active plugin, one by a
 * plugin whose feature is disabled, one by an id no package selects. */
static int active_entry_hits;
static int disabled_entry_hits;
static int unselected_entry_hits;
static uint32_t active_entry_last;
static int entry_test_mode, nested_result;
static std::jmp_buf callback_escape;
static uint32_t nested_pc;
static void test_active_entry(CPUState* cpu, uint32_t address) {
    active_entry_hits++;
    active_entry_last = address;
    if (entry_test_mode == 1) {
        CPUState unrelated{};
        if (psx_mod_finish_function(&unrelated) || !psx_mod_finish_function(cpu)) failures++;
        cpu->gpr[2] = 0x12345678u;
    } else if (entry_test_mode == 2 || entry_test_mode == 3) {
        const int mode = entry_test_mode;
        if (mode == 3 && !psx_mod_finish_function(cpu)) failures++;
        CPUState nested{}; nested.gpr[31] = 0x80004000u;
        entry_test_mode = mode == 2 ? 1 : 0;
        nested_result = psx_mod_function_entry(&nested, address);
        nested_pc = nested.pc;
        entry_test_mode = mode;
    } else if (entry_test_mode == 4) {
        /* A native call made by a hook can cross a guest VBlank. Its
         * callbacks must neither inherit nor discard the interrupted hook. */
        interrupted_entry_cpu = cpu;
        mod_runtime_on_vblank();
        interrupted_entry_cpu = nullptr;
        if (!psx_mod_finish_function(cpu)) failures++;
        cpu->gpr[2] = 0x87654321u;
    } else if (entry_test_mode == 5) {
        /* Guest scheduling/RFE can abandon the callback's C++ scope. */
        std::longjmp(callback_escape, 1);
    } else if (entry_test_mode == 6) {
        if (!psx_mod_finish_function(cpu)) failures++;
        ModFunctionEntryContext interrupted{};
        mod_runtime_function_entry_context_save(&interrupted);
        if (setjmp(callback_escape) == 0) {
            CPUState nested{};
            entry_test_mode = 5;
            psx_mod_function_entry(&nested, address);
            failures++; /* the nested callback must escape */
        }
        mod_runtime_function_entry_context_restore(&interrupted);
        if (!psx_mod_function_entry_active() || !psx_mod_finish_function(cpu)) failures++;
        entry_test_mode = 6;
    }
}
static void test_disabled_entry(CPUState*, uint32_t) { disabled_entry_hits++; }
static void test_unselected_entry(CPUState*, uint32_t) { unselected_entry_hits++; }

static bool filter_handles;
static bool filter_context_active;
static unsigned active_filter_hits;
static unsigned inactive_filter_hits;
static uint32_t active_filter_last;
static int test_active_filter(CPUState* cpu, uint32_t address) {
    ++active_filter_hits;
    active_filter_last = address;
    filter_context_active = psx_mod_function_entry_active() != 0;
    if (!filter_handles) return 0;
    cpu->gpr[2] = 42;
    return 1;
}
static int test_inactive_filter(CPUState*, uint32_t) {
    ++inactive_filter_hits;
    return 1;
}
static int guest_function_hits;
static int instruction_hits;
static int instruction_shared_hits, instruction_overlay_hits;
static void test_instruction(CPUState* cpu, uint32_t) {
    instruction_hits++;
    cpu->gpr[5] = 0x80301008u;
    if (psx_mod_finish_function(cpu)) failures++;
}
static void test_shared_instruction(CPUState* cpu, uint32_t) {
    ++instruction_shared_hits;
    if (cpu->gpr[5] != 0x80301008u) ++failures;
    cpu->gpr[6] = 0x12345678u;
}
static void test_overlay_instruction(CPUState* cpu, uint32_t) {
    ++instruction_overlay_hits;
    cpu->gpr[7] = 0x87654321u;
}
static void test_guest_function(CPUState* cpu, uint32_t) {
    guest_function_hits++;
    const uint32_t ra = cpu->gpr[31];
    CPUState nested{};
    nested.gpr[31] = 0x80007000u;
    const int mode = entry_test_mode;
    entry_test_mode = 1;
    if (!psx_mod_function_entry(&nested, 0x80003000u) || nested.pc != nested.gpr[31]) failures++;
    entry_test_mode = mode;
    interrupted_entry_cpu = cpu;
    mod_runtime_on_vblank();
    interrupted_entry_cpu = nullptr;
    if (!psx_mod_finish_function(cpu)) failures++;
    cpu->gpr[2] = cpu->gpr[4] + 7u;
    if (cpu->gpr[31] != ra) failures++;
}


static void test_activation_plugin(void) {
    activation_calls++;
}

static const uint8_t* media_snapshot;
static uint64_t media_size;
static int media_access, foreign_media_access;
static uint32_t mounted_lba;
static int extent_ok, invalid_extent, foreign_extent;
static int audio_ok, audio_invalid, audio_foreign, audio_data_rejected;
static uint32_t audio_lba, audio_lba2;
static void test_disc_audio_plugin() {
    PSXModCDDATrack tracks[]={{nullptr,0,0,2},{nullptr,0,0,3}};
    uint32_t starts[2]{},counts[2]{};
    if(!psx_mod_append_cdda_tracks(tracks,2,starts,counts) ||
       starts[0]!=150 || starts[1]!=153 || counts[0]!=3 || counts[1]!=2) failures++;
}
static void test_media_plugin(void) {
    media_access = psx_mod_current_resource_bytes("rom", &media_snapshot, &media_size);
    const uint8_t* foreign = nullptr;
    uint64_t size = 0;
    foreign_media_access = psx_mod_current_resource_bytes("private", &foreign, &size);
    extent_ok=psx_mod_append_disc_extent("xa",0,2,&mounted_lba);
    uint32_t bad=99;
    invalid_extent=psx_mod_append_disc_extent("xa",1,2,&bad);
    foreign_extent=psx_mod_append_disc_extent("private",0,1,&bad);
    PSXModCDDATrack sources[]={{"xa",0,1,0},{"xa",2320,1,0}};
    uint32_t starts[2]{},counts[2]{};
    audio_ok=psx_mod_append_cdda_tracks(sources,2,starts,counts);
    audio_lba=starts[0];audio_lba2=starts[1];
    sources[1].byte_offset=2321;
    audio_invalid=psx_mod_append_cdda_tracks(sources,2,starts,counts);
    if(starts[0] || counts[0]) failures++;
    sources[1]={"private",0,1,0};
    audio_foreign=psx_mod_append_cdda_tracks(sources,2,starts,counts);
    sources[1]={nullptr,0,0,1};
    audio_data_rejected=!psx_mod_append_cdda_tracks(sources,2,starts,counts);

}

extern "C" {
void* iso_open(const char*);
void iso_close(void*);
int iso_read_raw_sector(void*,uint32_t,uint8_t*,int);
int iso_read_sector(void*,uint32_t,uint8_t*,int);
int iso_read_subq(void*,uint32_t,uint8_t*,int,int*);
uint32_t iso_sector_count(void*);
int iso_track_count(void*);
uint32_t iso_track_start_lba(void*,int);
int iso_track_is_audio(void*,int);
int iso_cdda_track_count(void*);
uint32_t iso_cdda_track_start_lba(void*,int);
uint32_t iso_cdda_sector_count(void*);
int iso_cdda_track_is_audio(void*,int);
int iso_read_cdda_sector(void*,uint32_t,uint8_t*,int);
}

static int big_ram_activations;
static int option_changed_hits = 0;
static std::string option_changed_value;
static int option_changed_refuse = 0;  /* 1 = the plugin answers "next start" */
static int test_option_changed(const char* option_id, const char* value) {
    char cur[32];
    option_changed_hits++;
    /* Runs in the plugin's context: current_option_value answers the new value. */
    if (std::string(option_id) == "size" &&
        psx_mod_current_option_value("size", cur, sizeof(cur)) == 1)
        option_changed_value = cur;
    (void)value;
    return option_changed_refuse ? 0 : 1;
}
static void test_big_ram_activation(void) {
    big_ram_activations++;
}

static void test_restore_plugin(void) {
    restore_calls++;
    if (ram[0x1000] != 0xa1) failures++; /* main plan must be reapplied first */
}

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

static void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

static std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

static uint64_t launch_timing_total() {
    uint64_t total = 0;
    host_launch_timing_snapshot(nullptr, 0, &total, nullptr);
    return total;
}

static void check_commit_timing(uint64_t before, bool ok, bool hashed) {
    HostLaunchTimingEvent events[HOST_LAUNCH_TIMING_CAPACITY]{};
    const auto n = host_launch_timing_snapshot(events, HOST_LAUNCH_TIMING_CAPACITY,
                                              nullptr, nullptr);
    const HostLaunchTimingEvent* commit = nullptr;
    for (uint32_t i = 0; i < n; ++i)
        if (events[i].seq > before && events[i].stage == HOST_LAUNCH_COMMIT)
            commit = &events[i];
    check(commit && commit->ok == unsigned(ok), "real commit outcome retained by C snapshot");
    if (!commit) return;
    uint32_t stages = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& e = events[i];
        if (e.seq <= before || e.parent_id != commit->scope_id) continue;
        stages |= 1u << e.stage;
        check(e.start_us >= commit->start_us &&
              e.start_us + e.duration_us <= commit->start_us + commit->duration_us,
              "real commit child measurement nested inside total");
    }
    check(bool(stages & (1u << HOST_LAUNCH_DISC_HASH)) == hashed,
          "disc hash timing follows actual digest reuse");
    const uint32_t required = (1u << HOST_LAUNCH_PREPARE_RESOURCES) |
        (1u << HOST_LAUNCH_RESOLVE);
    check((stages & required) == required, "real preparation and resolution observed");
    if (ok) {
        const uint32_t final = (1u << HOST_LAUNCH_OVERLAY_VERIFY) |
            (1u << HOST_LAUNCH_DERIVED_DISC) | (1u << HOST_LAUNCH_SAVE_STATE) |
            (1u << HOST_LAUNCH_BUILD_DISC_INDEX);
        check((stages & final) == final, "real successful commit final stages observed");
    } else {
        check(!(stages & (1u << HOST_LAUNCH_BUILD_DISC_INDEX)),
              "rejected commit does not invent final-stage completion");
    }
}

extern "C" int psx_mod_counters_snapshot(const char**, uint64_t*, uint64_t*, int, uint64_t*);
static uint64_t startup_counter(const char* requested) {
    const char* names[128]{}; uint64_t counts[128]{}, frames[128]{};
    const int count = psx_mod_counters_snapshot(names, counts, frames, 128, nullptr);
    for (int i = 0; i < count && i < 128; ++i)
        if (std::string(names[i]) == requested) return counts[i];
    return 0;
}

int main() {
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-runtime-cache-test";
    const auto isolated_cache = root / "test-cache";
#if defined(_WIN32)
    check(_wputenv_s(L"LOCALAPPDATA", isolated_cache.c_str()) == 0,
          "isolate audited digest receipts from owner cache");
#else
    check(setenv("XDG_CACHE_HOME", isolated_cache.c_str(), 1) == 0,
          "isolate audited digest receipts from owner cache");
#endif
#if defined(_WIN32)
    const char* configured_cache = std::getenv("LOCALAPPDATA");
#else
    const char* configured_cache = std::getenv("XDG_CACHE_HOME");
#endif
    check(configured_cache && fs::path(configured_cache) == isolated_cache,
          "CRT getenv observes the isolated audited cache");
#else
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-runtime-test";
#endif
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::vector<uint8_t> stock(8 * 2352, 0);
    std::vector<uint8_t> overlay(3000);
    for (size_t i = 0; i < overlay.size(); ++i)
        overlay[i] = (uint8_t)(i * 17u + 3u);
    const fs::path stock_path = root / "stock.bin";
    write_bytes(stock_path, stock);
    write_bytes(root / "audio.bin", std::vector<uint8_t>(2 * 2352, 0x5a));
    const fs::path cue_path = root / "stock.cue";
    write_text(cue_path,
        "FILE \"stock.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"audio.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:00\n");
    write_bytes(root / "packages/runtime.test/1.0.0/assets/overlay.bin",
                overlay);
    write_text(root / "packages/runtime.test/1.0.0/manifest.toml",
        "format_version = 5\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "name = \"Runtime Test\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-RUNTIME\"\n"
        "disc_sha256 = \"" + sha256_hex(stock) + "\"\n"
        "[[feature]]\n"
        "id = \"main-code\"\n"
        "name = \"Main Code\"\n"
        "[[feature]]\n"
        "id = \"disc-byte\"\n"
        "name = \"Disc Byte\"\n"
        "[[feature]]\n"
        "id = \"asset-overlay\"\n"
        "name = \"Asset Overlay\"\n"
        "[[feature]]\n"
        "id = \"user-byte\"\n"
        "name = \"User Byte\"\n"
        "[[feature]]\n"
        "id = \"dynamic-main\"\n"
        "name = \"Dynamic Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-main\"\n"
        "name = \"Sparse Main\"\n"
        "[[feature]]\n"
        "id = \"sparse-flag\"\n"
        "name = \"Sparse Flag\"\n"
        "[[feature]]\n"
        "id = \"sparse-disc\"\n"
        "name = \"Sparse Disc\"\n"
        "[[feature]]\n"
        "id = \"vblank-plugin\"\n"
        "name = \"VBlank Plugin\"\n"
        "[[option]]\n"
        "feature = \"dynamic-main\"\n"
        "id = \"count\"\n"
        "label = \"Count\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 254\n"
        "default = 42\n"
        "[[option]]\n"
        "feature = \"sparse-main\"\n"
        "id = \"frames\"\n"
        "label = \"Frames\"\n"
        "type = \"integer\"\n"
        "min = 0\n"
        "max = 99\n"
        "default = 2\n"
        "[[patch]]\n"
        "feature = \"main-code\"\n"
        "target = \"main_exe\"\n"
        "address = 2147487744\n"
        "expected = \"01020304\"\n"
        "replace = \"a1a2a3a4\"\n"
        "[[patch]]\n"
        "feature = \"disc-byte\"\n"
        "target = \"disc_raw\"\n"
        "offset = 4714\n"
        "expected = \"aa\"\n"
        "replace = \"bb\"\n"
        "[[patch]]\n"
        "feature = \"user-byte\"\n"
        "target = \"disc_user\"\n"
        "offset = 6154\n"
        "expected = \"cc\"\n"
        "replace = \"dd\"\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488000\n"
        "expected = \"0000\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\" }\n"
        "[[patch]]\n"
        "feature = \"dynamic-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488002\n"
        "expected = \"0100\"\n"
        "replace_from = { option = \"count\", encoding = \"u16le\", addend = 1 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, option = \"frames\", encoding = \"u8\" }]\n"
        "when_integer = { option = \"frames\", op = \"gt\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-main\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 0, replace = \"01\" }, "
        "{ offset = 2, replace = \"00\" }]\n"
        "when_integer = { option = \"frames\", op = \"eq\", value = 0 }\n"
        "[[patch]]\n"
        "feature = \"sparse-flag\"\n"
        "target = \"main_exe\"\n"
        "address = 2147488256\n"
        "expected = \"02000132\"\n"
        "fields = [{ offset = 1, replace = \"42\" }]\n"
        "[[patch]]\n"
        "feature = \"sparse-disc\"\n"
        "target = \"disc_user\"\n"
        "offset = 6164\n"
        "expected = \"11223344\"\n"
        "fields = [{ offset = 0, replace = \"aa\" }, "
        "{ offset = 2, replace = \"bb\" }]\n"
        "[[overlay]]\n"
        "feature = \"asset-overlay\"\n"
        "target = \"disc_raw\"\n"
        "offset = 11408\n"
        "file = \"assets/overlay.bin\"\n"
        "sha256 = \"" + sha256_hex(overlay) + "\"\n"
        "expected_sha256 = \"" +
            sha256_hex(std::vector<uint8_t>(overlay.size(), 0)) + "\"\n"
        "[[plugin]]\n"
        "feature = \"vblank-plugin\"\n"
        "id = \"runtime.test-vblank\"\n"
        "[[feature]]\n"
        "id = \"entry-disabled\"\n"
        "name = \"Disabled Entry\"\n"
        "[[plugin]]\n"
        "feature = \"entry-disabled\"\n"
        "id = \"runtime.test-disabled-entry\"\n");
    write_text(root / "state.toml",
        "format_version = 2\n"
        "[[package]]\n"
        "id = \"runtime.test\"\n"
        "version = \"1.0.0\"\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"main-code\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"disc-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"asset-overlay\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"user-byte\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"dynamic-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "count = 42\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-main\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "frames = 0\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-flag\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"sparse-disc\"\n"
        "enabled = true\n"
        "[[feature]]\n"
        "package_id = \"runtime.test\"\n"
        "id = \"vblank-plugin\"\n"
        "enabled = true\n");

    std::string error;
    PSXRecompV4::mod_clear_plugins_for_tests();
    check(PSXRecompV4::mod_register_activation_plugin(
              "runtime.test-vblank", test_activation_plugin),
          "runtime test activation hook must register");
    check(PSXRecompV4::mod_register_vblank_plugin(
              "runtime.test-vblank", test_vblank_plugin),
          "runtime test plugin must register");
    check(psx_mod_register_function_entry_plugin(
              "runtime.test-vblank", 0x80003000u, test_active_entry) == 1,
          "active plugin's function-entry hook must register");
    check(psx_mod_register_function_entry_plugin(
              "runtime.test-disabled-entry", 0x80003000u, test_disabled_entry) == 1,
          "disabled feature's function-entry hook must register");
    check(psx_mod_register_function_entry_plugin(
              "runtime.unselected-entry", 0x80003000u, test_unselected_entry) == 1,
          "unselected function-entry hook must register");
    CPUState entry_cpu{};
    check(psx_mod_register_instruction_plugin("runtime.test-vblank", 0x80003004u,
              0x90A30014u, test_instruction), "register guarded instruction callback");
    check(!psx_mod_register_instruction_plugin("runtime.test-vblank", 0xA0003004u,
              0x90A30014u, test_instruction) &&
          !psx_mod_register_instruction_plugin("runtime.other", 0x80003005u,
              0x90A30014u, test_instruction), "reject aliased duplicate and unaligned sites");
    check(psx_mod_register_instruction_plugin("runtime.test-vblank", 0xA0003004u,
              0x90A30014u, test_shared_instruction), "overlay contexts may share an instruction word");
    check(psx_mod_register_instruction_plugin("runtime.test-vblank", 0x80003004u,
              0x90A30015u, test_overlay_instruction), "overlay versions may share an address");
    check(!psx_mod_register_instruction_plugin("runtime.test-vblank", 0x80003004u,
              0x90A30014u, test_shared_instruction), "shared callback exact duplicates remain rejected");
    psx_mod_write_word(0x80003004u, 0x90A30014u);
    check(psx_mod_register_guest_function_plugin(
              "runtime.test-vblank", 0x8FFF0000u, test_guest_function),
          "trusted callback registers outside the hardware map");
    check(!psx_mod_register_guest_function_plugin(
              "runtime.other", 0xAFFF0000u, test_guest_function) &&
          !psx_mod_register_guest_function_plugin("runtime.other", 0x80003000u, test_guest_function) &&
          !psx_mod_register_guest_function_plugin("runtime.other", 0x8FFF0001u, test_guest_function) &&
          !psx_mod_register_guest_function_plugin("runtime.other", 0xCFFF0000u, test_guest_function),
          "guest callbacks refuse alias collisions, game addresses, misalignment and KSEG2");
    check(psx_mod_register_guest_function_plugin(
              "runtime.unselected-entry", 0x8FFF0004u, test_unselected_entry),
          "unselected callback implementation can register");
    check(psx_mod_register_savestate_plugin("runtime.test-vblank", test_restore_plugin),
          "restore callback must register through the C API");
    check(!psx_mod_register_savestate_plugin("runtime.test-vblank", test_restore_plugin),
          "duplicate restore callback must be rejected");
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    const auto first_commit_timing = launch_timing_total();
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error),
          "CUE and its data-track BIN must have the same mod target identity");
    check_commit_timing(first_commit_timing, true, true);
    /* Committed but not yet activated: no hook may run. */
    psx_mod_instruction(&entry_cpu, 0x80003004u, 0x90A30014u);
    check(instruction_hits == 0 && !g_psx_mod_instruction_hooks, "instruction hooks await activation");
    check(!psx_mod_dispatch_guest_function(&entry_cpu, 0x8FFF0000u) &&
              g_psx_mod_guest_functions == 0,
          "committed guest functions remain unavailable until activation");
    psx_mod_function_entry(&entry_cpu, 0x80003000u);
    check(g_psx_mod_function_entry_hooks == 0 && active_entry_hits == 0,
          "function-entry hooks must not run before plugin activation");
    mod_runtime_activate_plugins();
    check(activation_calls == 1,
          "resolved trusted plugin must activate before runtime startup");
    entry_cpu.pc = 0x80004000u;
    psx_mod_instruction(&entry_cpu, 0x80003004u, 0x90A30014u);
    psx_mod_instruction(&entry_cpu, 0xA0003004u, 0x90A30014u);
    check(instruction_hits == 2 && entry_cpu.gpr[5] == 0x80301008u &&
          entry_cpu.pc == 0x80004000u, "instruction aliases preserve control flow and edit registers");
    check(instruction_shared_hits == 2 && instruction_overlay_hits == 0 &&
          entry_cpu.gpr[6] == 0x12345678u && g_psx_mod_instruction_hooks == 3,
          "both matching contexts run; other overlay word remains inactive");
    psx_mod_instruction(&entry_cpu, 0x80003004u, 0x90A30015u);
    psx_mod_write_word(0x80003004u, 0x90A30015u);
    psx_mod_instruction(&entry_cpu, 0x80003004u, 0x90A30014u);
    check(instruction_hits == 2, "fetched and live instruction guards reject changed code");
    check(instruction_shared_hits == 2 && instruction_overlay_hits == 0,
          "shared callbacks also require fetched and live word agreement");
    psx_mod_instruction(&entry_cpu, 0xA0003004u, 0x90A30015u);
    check(instruction_overlay_hits == 1 && entry_cpu.gpr[7] == 0x87654321u &&
          instruction_hits == 2 && instruction_shared_hits == 2,
          "overlay replacement selects only its instruction word");
    psx_mod_write_word(0x80003004u, 0x90A30014u);
    mod_runtime_on_vblank();
    check(plugin_calls == 1,
          "resolved trusted plugin must run on guest VBlank");
    check(g_psx_mod_function_entry_hooks == 1,
          "activation must flatten exactly the active plan's entry hooks");
    psx_mod_function_entry(&entry_cpu, 0x80003000u);
    check(active_entry_hits == 1 && active_entry_last == 0x80003000u,
          "active plan's function-entry hook must run at its address");
    psx_mod_function_entry(&entry_cpu, 0x00003000u);
    psx_mod_function_entry(&entry_cpu, 0xA0003000u);
    check(active_entry_hits == 3 && active_entry_last == 0xA0003000u,
          "function-entry hooks must match every segment alias of the code address");
    psx_mod_function_entry(&entry_cpu, 0x80003004u);
    psx_mod_function_entry(&entry_cpu, 0x80203000u);
    check(active_entry_hits == 3,
          "function-entry hooks must not run for other addresses or RAM mirrors");
    check(disabled_entry_hits == 0 && unselected_entry_hits == 0,
          "hooks of plugins the plan does not activate must never run");
    /* A replaced plan drops every hook until its own activation. */
    check(PSXRecompV4::mod_runtime_clear_for_netplay(&error), error.c_str());
    psx_mod_instruction(&entry_cpu, 0x80003004u, 0x90A30014u);
    check(instruction_hits == 2 && !g_psx_mod_instruction_hooks, "clearing removes instruction hooks");
    check(instruction_shared_hits == 2 && instruction_overlay_hits == 1,
          "clearing removes every shared-address variant");
    check(!psx_mod_dispatch_guest_function(&entry_cpu, 0x8FFF0000u) &&
              g_psx_mod_guest_functions == 0,
          "clearing the plan drops guest callback availability");
    psx_mod_function_entry(&entry_cpu, 0x80003000u);
    check(g_psx_mod_function_entry_hooks == 0 && active_entry_hits == 3,
          "clearing the plan must drop its function-entry hooks");
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error), error.c_str());
    psx_mod_function_entry(&entry_cpu, 0x80003000u);
    check(active_entry_hits == 3,
          "a re-committed plan must not run hooks before activation");
    mod_runtime_activate_plugins();
    psx_mod_function_entry(&entry_cpu, 0x80003000u);
    check(active_entry_hits == 4 && disabled_entry_hits == 0 &&
              unselected_entry_hits == 0,
          "re-activation restores exactly the active plan's hooks");
    /* The lobby rematch, in the order main.cpp's start_mod_session() drives
     * it: a netplay match (plan cleared, then activated) and an offline
     * rematch that commits and activates exactly like a first boot. */
    check(PSXRecompV4::mod_runtime_clear_for_netplay(&error), error.c_str());
    mod_runtime_activate_plugins();
    mod_runtime_on_vblank();
    psx_mod_function_entry(&entry_cpu, 0x80003000u);
    check(activation_calls == 2 && plugin_calls == 1 &&
              g_psx_mod_function_entry_hooks == 0 && active_entry_hits == 4,
          "a netplay session must stay vanilla: no activation, VBlank or hook");
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error), error.c_str());
    mod_runtime_activate_plugins();
    check(activation_calls == 3,
          "an offline rematch after netplay must run activation callbacks");
    mod_runtime_on_vblank();
    check(plugin_calls == 2,
          "an offline rematch after netplay must run VBlank plugins");
    psx_mod_function_entry(&entry_cpu, 0x80003000u);
    check(g_psx_mod_function_entry_hooks == 1 && active_entry_hits == 5 &&
              disabled_entry_hits == 0 && unselected_entry_hits == 0,
          "an offline rematch after netplay must arm exactly its plan's hooks");

    check(psx_mod_register_function_filter_plugin(
              "runtime.test-vblank", 0x80003008u, test_active_filter) == 1,
          "active plan's return filter must register");
    check(psx_mod_register_function_filter_plugin(
              "runtime.test-vblank", 0xA0003008u, test_active_filter) == 0,
          "filter aliases must not register duplicate hooks");
    check(psx_mod_register_function_filter_plugin(
              "runtime.test-disabled-entry", 0x80003008u, test_inactive_filter) == 1 &&
              psx_mod_register_function_filter_plugin(
                  "runtime.unselected-entry", 0x80003008u, test_inactive_filter) == 1,
          "inactive filter implementations must register without running");
    entry_cpu.pc = 0x80003008u;
    entry_cpu.gpr[31] = 0x80004000u;
    entry_cpu.gpr[2] = 99;
    check(!psx_mod_function_entry(&entry_cpu, entry_cpu.pc) && active_filter_hits == 0,
          "new filters must wait for active-plan table rebuild");
    mod_runtime_activate_plugins();
    check(g_psx_mod_function_entry_hooks == 2 &&
              !psx_mod_function_entry(&entry_cpu, entry_cpu.pc) &&
              active_filter_hits == 1 && filter_context_active &&
              entry_cpu.pc == 0x80003008u && entry_cpu.gpr[2] == 99,
          "unhandled active filter must preserve native fallthrough");
    filter_handles = true;
    entry_cpu.pc = 0xA0003008u;
    check(psx_mod_function_entry(&entry_cpu, entry_cpu.pc) == 1 &&
              active_filter_hits == 2 && active_filter_last == 0xA0003008u &&
              entry_cpu.pc == entry_cpu.gpr[31] && entry_cpu.gpr[2] == 42 &&
              inactive_filter_hits == 0 && !psx_mod_function_entry_active(),
          "handled alias filter must publish return registers and pc=$ra");
    check(PSXRecompV4::mod_runtime_clear_for_netplay(&error), error.c_str());
    check(!psx_mod_function_entry(&entry_cpu, 0x80003008u) &&
              active_filter_hits == 2 && g_psx_mod_function_entry_hooks == 0,
          "clearing the active plan must also disarm return filters");
    check(PSXRecompV4::mod_runtime_commit(cue_path, &error), error.c_str());
    mod_runtime_activate_plugins();

    entry_cpu.gpr[31] = 0x80005000u;
    check(!psx_mod_finish_function(&entry_cpu) && !psx_mod_function_entry(nullptr, 0x80003000u),
          "completion outside an entry callback or with no CPU is refused");
    entry_test_mode = 1;
    check(psx_mod_function_entry(&entry_cpu, 0x80003000u) == 1 &&
              entry_cpu.pc == 0x80005000u && entry_cpu.gpr[2] == 0x12345678u,
          "explicit completion returns handled and publishes guest return PC/results");
    entry_test_mode = 2; entry_cpu.pc = 0;
    check(!psx_mod_function_entry(&entry_cpu, 0x80003000u) && entry_cpu.pc == 0 &&
              nested_result == 1 && nested_pc == 0x80004000u,
          "inner completion cannot finish an ordinary outer callback");
    entry_test_mode = 3;
    check(psx_mod_function_entry(&entry_cpu, 0x80003000u) == 1 &&
              entry_cpu.pc == 0x80005000u && nested_result == 0,
          "outer completion survives a nested ordinary callback");
    entry_test_mode = 4;
    check(psx_mod_function_entry(&entry_cpu, 0x80003000u) == 1 &&
              entry_cpu.pc == 0x80005000u && entry_cpu.gpr[2] == 0x87654321u,
          "entry completion survives VBlank callbacks during a native call");
    entry_test_mode = 0;

    entry_test_mode = 5;
    if (setjmp(callback_escape) == 0) {
        psx_mod_function_entry(&entry_cpu, 0x80003000u);
        check(false, "callback must escape without its native cleanup");
    }
    check(psx_mod_function_entry_active(), "escape reproduces the stranded capture gate");
    const ModFunctionEntryContext no_callback{};
    mod_runtime_function_entry_context_restore(&no_callback);
    check(!psx_mod_function_entry_active() && !psx_mod_finish_function(&entry_cpu),
          "scheduler landing releases the gate and clears abandoned completion ownership");
    entry_test_mode = 6;
    check(psx_mod_function_entry(&entry_cpu, 0x80003000u) == 1 &&
              entry_cpu.pc == entry_cpu.gpr[31] && !psx_mod_function_entry_active(),
          "nested exception landing preserves the live outer callback and completion");
    entry_test_mode = 0;

    entry_cpu.gpr[4] = 35u;
    const uint32_t guest_sp = entry_cpu.gpr[29];
    check(g_psx_mod_guest_functions == 1 &&
              psx_mod_dispatch_guest_function(&entry_cpu, 0x8FFF0000u) &&
              entry_cpu.gpr[2] == 42u && entry_cpu.pc == 0x80005000u &&
              entry_cpu.gpr[29] == guest_sp,
          "active callback returns its result through normal guest PC/stack state");
    check(psx_mod_dispatch_guest_function(&entry_cpu, 0x0FFF0000u) &&
              psx_mod_dispatch_guest_function(&entry_cpu, 0xAFFF0000u) &&
              guest_function_hits == 3,
          "guest callback aliases retain nested entry and VBlank scope");
    check(!psx_mod_dispatch_guest_function(&entry_cpu, 0x8FFF0004u) &&
              !psx_mod_dispatch_guest_function(&entry_cpu, 0x8FFF0001u) &&
              !psx_mod_dispatch_guest_function(&entry_cpu, 0xCFFF0000u) &&
              !psx_mod_dispatch_guest_function(nullptr, 0x8FFF0000u) &&
              unselected_entry_hits == 0,
          "unselected, unknown, misaligned and invalid callback requests fall through");

    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_dispatch(0x80001000);
    check(ram[0x1000] == 1, "patch must wait for the configured entry point");
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4,
          "main-EXE patch must apply before entry execution");
    check(ram[0x1100] == 42 && ram[0x1101] == 0 &&
              ram[0x1102] == 43 && ram[0x1103] == 0,
          "dynamic main-EXE patches must encode all sites before entry");
    check(ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "adjacent sparse fields must compose while preserving guard-only "
          "bytes");

    /* A full-machine savestate replaces main RAM after the entry-point plan
     * has already applied. Loading a stock checkpoint must not silently turn
     * the currently enabled main-EXE features back off. */
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    const int activation_before_restore = activation_calls;
    const int vblank_before_restore = plugin_calls;
    mod_runtime_on_savestate_loaded();
    check(restore_calls == 1 && activation_calls == activation_before_restore &&
              plugin_calls == vblank_before_restore,
          "restore must rebind hooks without activation or a gameplay VBlank");
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "savestate restore must reapply the complete enabled main plan");

    std::array<uint8_t, 2352> sector{};
    sector[10] = 0xaa;
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xaa, "disc overlay must stay off during reference reads");
    mod_runtime_enable_disc_patches();
    mod_runtime_patch_disc_sector(2, 1, sector.data(), (uint32_t)sector.size());
    check(sector[10] == 0xbb, "raw disc overlay must patch matching sectors");

    std::array<uint8_t, 2352> overlay_sector{};
    mod_runtime_patch_disc_sector(
        4, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[1999] == 0 &&
              overlay_sector[2000] == overlay[0] &&
              overlay_sector[2351] == overlay[351],
          "file overlay must patch the tail of its first sector");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        5, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector.front() == overlay[352] &&
              overlay_sector.back() == overlay[2703],
          "file overlay must patch complete middle sectors");
    overlay_sector.fill(0);
    mod_runtime_patch_disc_sector(
        6, 1, overlay_sector.data(), (uint32_t)overlay_sector.size());
    check(overlay_sector[0] == overlay[2704] &&
              overlay_sector[295] == overlay[2999] &&
              overlay_sector[296] == 0,
          "file overlay must patch the head of its final sector");

    std::array<uint8_t, 2352> mode2_sector{};
    mode2_sector[15] = 2;
    mode2_sector[18] = 0;
    mode2_sector[24 + 10] = 0xcc;
    mode2_sector[24 + 20] = 0x11;
    mode2_sector[24 + 21] = 0x22;
    mode2_sector[24 + 22] = 0x33;
    mode2_sector[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, mode2_sector.data(), (uint32_t)mode2_sector.size());
    check(mode2_sector[24 + 10] == 0xdd,
          "disc_user operations must apply to raw Mode2 Form1 user data");
    check(mode2_sector[24 + 20] == 0xaa &&
              mode2_sector[24 + 21] == 0x22 &&
              mode2_sector[24 + 22] == 0xbb &&
              mode2_sector[24 + 23] == 0x44,
          "sparse disc writes must validate a complete guard and modify only "
          "owned fields");
    std::array<uint8_t, 2352> audio_sector{};
    audio_sector[24 + 10] = 0xcc;
    mod_runtime_patch_disc_sector(
        3, 1, audio_sector.data(), (uint32_t)audio_sector.size());
    check(audio_sector[24 + 10] == 0xcc,
          "disc_user operations must not modify CDDA/non-data sectors");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 2; ram[0x1103] = 0; /* second dynamic guard is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1100] == 0 && ram[0x1101] == 0,
          "one failed generated guard must leave the complete main plan untouched");

    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x33; /* guard-only byte is wrong */
    mod_runtime_on_dispatch(0x80002000);
    check(ram[0x1000] == 1 && ram[0x1003] == 4 &&
              ram[0x1200] == 2 && ram[0x1201] == 0 &&
              ram[0x1202] == 1 && ram[0x1203] == 0x33,
          "a failed sparse guard-only byte must leave the complete main plan "
          "untouched");

    /* Loading can be requested while the boot executable is still running,
     * before the configured game entry point has dispatched. The restored
     * checkpoint itself supplies the bytes used to validate and apply the
     * plan in that case. */
    check(PSXRecompV4::mod_runtime_initialize(
              root, "SLUS-RUNTIME", 0x80002000, {}, &error),
          error.c_str());
    check(PSXRecompV4::mod_runtime_commit(stock_path, &error), error.c_str());
    ram[0x1000] = 1; ram[0x1001] = 2; ram[0x1002] = 3; ram[0x1003] = 4;
    ram[0x1100] = 0; ram[0x1101] = 0;
    ram[0x1102] = 1; ram[0x1103] = 0;
    ram[0x1200] = 2; ram[0x1201] = 0;
    ram[0x1202] = 1; ram[0x1203] = 0x32;
    mod_runtime_on_savestate_loaded();
    check(ram[0x1000] == 0xa1 && ram[0x1003] == 0xa4 &&
              ram[0x1100] == 42 && ram[0x1102] == 43 &&
              ram[0x1200] == 1 && ram[0x1201] == 0x42 &&
              ram[0x1202] == 0 && ram[0x1203] == 0x32,
          "pre-entry savestate restore must validate and apply the main plan");

    mod_runtime_enable_disc_patches();
    std::array<uint8_t, 2352> bad_sparse_disc{};
    bad_sparse_disc[15] = 2;
    bad_sparse_disc[18] = 0;
    bad_sparse_disc[24 + 10] = 0xcc;
    bad_sparse_disc[24 + 20] = 0x11;
    bad_sparse_disc[24 + 21] = 0x99; /* guard-only byte is wrong */
    bad_sparse_disc[24 + 22] = 0x33;
    bad_sparse_disc[24 + 23] = 0x44;
    mod_runtime_patch_disc_sector(
        3, 1, bad_sparse_disc.data(),
        (uint32_t)bad_sparse_disc.size());
    check(bad_sparse_disc[24 + 10] == 0xcc &&
              bad_sparse_disc[24 + 20] == 0x11 &&
              bad_sparse_disc[24 + 22] == 0x33,
          "a failed sparse disc guard must leave every write in the sector "
          "untouched");

    /* Implicit requirement through the real commit/activation path: the
     * hidden required feature's activation plugin runs, the state.toml commit
     * writes never gains the derived activation (it is not a choice),
     * and a requirement on an absent package makes the launch commit fail
     * rather than boot the requiring build without it. */
    {
        const fs::path req_root = root / "requirement-runtime";
        write_text(req_root / "bundled/runtime.ram/1.0.0/manifest.toml",
            "format_version = 5\n"
            "id = \"runtime.ram\"\n"
            "version = \"1.0.0\"\n"
            "name = \"Runtime RAM\"\n"
            "[[target]]\n"
            "game_id = \"*\"\n"
            "[[feature]]\n"
            "id = \"big-ram\"\n"
            "name = \"Big RAM\"\n"
            "hidden = true\n"
            "[[option]]\n"
            "feature = \"big-ram\"\n"
            "id = \"size\"\n"
            "label = \"Size\"\n"
            "type = \"choice\"\n"
            "default = \"eight\"\n"
            "[[option.choice]]\nvalue = \"eight\"\nlabel = \"8 MB\"\n"
            "[[plugin]]\n"
            "feature = \"big-ram\"\n"
            "id = \"runtime.big-ram\"\n");
        write_text(req_root / "bundled/runtime.mode/1.0.0/manifest.toml",
            "format_version = 7\n"
            "id = \"runtime.mode\"\n"
            "version = \"1.0.0\"\n"
            "name = \"Runtime Mode\"\n"
            "[[target]]\n"
            "game_id = \"SLUS-REQ\"\n"
            "[[feature]]\n"
            "id = \"mode\"\n"
            "name = \"Mode\"\n"
            "[[option]]\n"
            "feature = \"mode\"\n"
            "id = \"extras\"\n"
            "label = \"Extras\"\n"
            "type = \"choice\"\n"
            "default = \"none\"\n"
            "[[option.choice]]\nvalue = \"none\"\nlabel = \"None\"\n"
            "[[option.choice]]\nvalue = \"full\"\nlabel = \"Full\"\n"
            "[[requirement]]\n"
            "feature = \"mode\"\n"
            "package = \"runtime.ram\"\n"
            "requires_feature = \"big-ram\"\n"
            "when = { extras = \"full\" }\n");
        const std::string state_text =
            "format_version = 2\n"
            "\n[[feature]]\npackage_id = \"runtime.mode\"\nid = \"mode\"\n"
            "enabled = true\n"
            "[feature.values]\nextras = \"full\"\n";
        write_text(req_root / "state.toml", state_text);
        const auto read_state = [&]() {
            std::ifstream in(req_root / "state.toml", std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        };

        big_ram_activations = 0;
        check(PSXRecompV4::mod_register_activation_plugin(
                  "runtime.big-ram", test_big_ram_activation),
              "big-ram activation hook must register");
        check(PSXRecompV4::mod_runtime_initialize(
                  req_root, "SLUS-REQ", 0x80002000, {}, &error),
              error.c_str());
        check(PSXRecompV4::mod_runtime_commit(stock_path, &error),
              "a satisfied implicit requirement must commit");
        mod_runtime_activate_plugins();
        check(big_ram_activations == 1,
              "the implicitly required feature's plugin must activate");
        char value[32];
        check(psx_mod_option_value("runtime.ram", "big-ram", "size", value,
                                   sizeof(value)) == 1 &&
                  std::string(value) == "eight",
              "an implicit feature's plugin must read its option values");
        /* In-game overlay P5: a live option change reaches the plugin and
         * every later read, and is refused while netplay owns the plan. */
        check(psx_mod_set_option_live("runtime.ram", "big-ram", "size", "four") == 0,
              "without an option-changed callback a live set waits for restart");
        check(psx_mod_option_value("runtime.ram", "big-ram", "size", value, sizeof(value)) == 1 &&
                  std::string(value) == "eight",
              "a restart-only change must not reach the running plugin");
        check(!psx_mod_option_live_capable("runtime.ram", "big-ram"),
              "no callback yet: not live-capable");
        check(psx_mod_register_option_changed_plugin("runtime.big-ram", test_option_changed),
              "option-changed hook must register");
        check(psx_mod_option_live_capable("runtime.ram", "big-ram"),
              "a registered callback makes the feature live-capable");
        check(psx_mod_set_option_live("runtime.ram", "big-ram", "size", "four") == 1 &&
                  option_changed_hits == 1 && option_changed_value == "four",
              "the plugin must see the change in its own context");
        check(psx_mod_option_value("runtime.ram", "big-ram", "size", value, sizeof(value)) == 1 &&
                  std::string(value) == "four",
              "a live override must answer later reads");
        /* A callback that refuses ("nothing changes until next start") must
         * leave the value the session runs with untouched. */
        option_changed_refuse = 1;
        check(psx_mod_set_option_live("runtime.ram", "big-ram", "size", "two") == 0 &&
                  option_changed_hits == 2,
              "a refused live set reports refused after asking the plugin");
        check(psx_mod_option_value("runtime.ram", "big-ram", "size", value, sizeof(value)) == 1 &&
                  std::string(value) == "four",
              "a refused live set must not change the value later reads see");
        option_changed_refuse = 0;
        check(psx_mod_set_option_live("runtime.ram", "no-such-feature", "size", "x") == 0,
              "features outside the plan are not live-settable");
        check(psx_mod_save_selection() == 1, "saving the selection mid-session must succeed");
        test_netplay_active = 1;
        check(psx_mod_set_option_live("runtime.ram", "big-ram", "size", "four") == -1,
              "netplay must refuse live option changes");
        test_netplay_active = 0;
        /* commit rewrites state.toml in canonical form; what matters is that
         * the player's choice survives and the derived one is never added. */
        const std::string after = read_state();
        check(after.find("runtime.ram") == std::string::npos &&
                  after.find("extras = \"full\"") != std::string::npos,
              "committing must not write the derived activation to state.toml");

        /* Condition false: the required plugin stays off. */
        write_text(req_root / "state.toml",
            "format_version = 2\n"
            "\n[[feature]]\npackage_id = \"runtime.mode\"\nid = \"mode\"\n"
            "enabled = true\n"
            "[feature.values]\nextras = \"none\"\n");
        big_ram_activations = 0;
        check(PSXRecompV4::mod_runtime_initialize(
                  req_root, "SLUS-REQ", 0x80002000, {}, &error),
              error.c_str());
        check(PSXRecompV4::mod_runtime_commit(stock_path, &error),
              error.c_str());
        mod_runtime_activate_plugins();
        check(big_ram_activations == 0,
              "a requirement whose condition is false must not activate");

        /* Required package absent (e.g. the title excluded the builtin). */
        write_text(req_root / "state.toml", state_text);
        fs::remove_all(req_root / "bundled/runtime.ram", ec);
        check(PSXRecompV4::mod_runtime_initialize(
                  req_root, "SLUS-REQ", 0x80002000, {}, &error),
              error.c_str());
        std::string commit_error;
        check(!PSXRecompV4::mod_runtime_commit(stock_path, &commit_error) &&
                  commit_error.find("runtime.mode/mode requires "
                                    "runtime.ram/big-ram") != std::string::npos,
              "an unmet requirement must fail the launch commit loudly");
    }

    /*
     * Display geometry passthrough. Ape Escape scans out 384 while its mode
     * width is 368, which is precisely why a plugin cannot derive this from
     * GPUSTAT: the horizontal range that makes the difference is write-only.
     */
    g_test_display.width = 384;
    g_test_display.height = 240;
    check(psx_mod_display_width() == 384u,
          "psx_mod_display_width must report the presenter's visible width, "
          "not the coarse mode width");
    check(psx_mod_display_height() == 240u,
          "psx_mod_display_height must report the presenter's visible height");

    g_test_display.width = 0;
    g_test_display.height = 0;
    check(psx_mod_display_width() == 0u && psx_mod_display_height() == 0u,
          "unestablished display geometry must report zero so callers skip "
          "drawing instead of guessing");

    /* Source-owned ISO: nested file spanning two sectors, without a derived
     * disc. Host reads must choose the original mount and leave sizes honest. */
    std::vector<uint8_t> iso(24*2048);
    auto le32 = [&](size_t at,uint32_t n) { for(unsigned i=0;i<4;++i) iso[at+i]=(uint8_t)(n>>(i*8)); };
    auto record = [&](size_t at,uint32_t lba,uint32_t bytes,bool directory,const std::string& name) {
        iso[at]=(uint8_t)((33+name.size()+1)&~size_t(1));
        le32(at+2,lba); le32(at+10,bytes); iso[at+25]=directory?2:0;
        iso[at+28]=1;iso[at+31]=1;iso[at+32]=(uint8_t)name.size();
        std::copy(name.begin(),name.end(),iso.begin()+at+33);
    };
    iso[16*2048]=1;std::copy_n("CD001",5,iso.begin()+16*2048+1);iso[16*2048+6]=1;
    record(16*2048+156,20,2048,true,std::string(1,'\0'));
    record(20*2048,21,2048,true,"S0");
    record(21*2048,22,3000,false,"LEVEL.NSF;1");
    for(unsigned i=0;i<3000;++i)iso[22*2048+i]=(uint8_t)(i*7);
    const auto disc_root=root/"host-reader";
    const auto iso_path=disc_root/"original.iso";
    write_bytes(iso_path,iso);
    check(PSXRecompV4::mod_runtime_initialize(disc_root,"READER",0,{},&error),"reader initialize");
    check(PSXRecompV4::mod_runtime_commit(iso_path,&error),"reader mount original ISO");
    uint32_t bytes=0;
    check(psx_mod_read_disc_file("S0/LEVEL.NSF",nullptr,0,&bytes) && bytes==3000,"query original nested file size");
    std::vector<uint8_t> result(3000);
    check(!psx_mod_read_disc_file("S0/LEVEL.NSF",result.data(),2999,&bytes) && bytes==0,"undersized destination rejected");
    check(psx_mod_read_disc_file("S0/LEVEL.NSF",result.data(),(uint32_t)result.size(),&bytes) &&
          bytes==3000 && std::equal(result.begin(),result.end(),iso.begin()+22*2048),"complete original file bytes");
    check(!psx_mod_read_disc_file("S0/MISSING.NSF",nullptr,0,&bytes) && bytes==0,"missing file explicit");
    check(!psx_mod_read_disc_file("S0",nullptr,0,&bytes),"directory rejected");
    std::vector<uint8_t> raw_iso(24*2352);
    for(unsigned i=0;i<24;++i) {
        raw_iso[i*2352+15]=2;
        std::copy_n(iso.begin()+i*2048,2048,raw_iso.begin()+i*2352+24);
    }
    const auto raw_path=disc_root/"original.bin";
    write_bytes(raw_path,raw_iso);
    check(PSXRecompV4::mod_runtime_commit(raw_path,&error),"reader mount raw disc");
    check(psx_mod_read_disc_file("S0/LEVEL.NSF",result.data(),(uint32_t)result.size(),&bytes) &&
          std::equal(result.begin(),result.end(),iso.begin()+22*2048),"raw and ISO reads identical");
    /* Verified bytes are scoped to the committed feature, and do not depend
     * on reopening an owner file after launch. No launcher is needed here. */
    const auto media_root = root / "verified-media";
    std::vector<uint8_t> rom(64, 0);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    rom[32] = 0x5a;
    const auto rom_path = media_root / "owner.z64";
    write_bytes(rom_path, rom);
    std::vector<uint8_t> xa(2336*2);
    xa[2]=xa[6]=0x64;xa[8]=0x52;
    xa[2336+2]=xa[2336+6]=0xE4;xa[2336+8]=0x63;
    const auto xa_path=media_root/"owner.xa";write_bytes(xa_path,xa);
    write_text(media_root / "packages/media.test/1.0.0/manifest.toml",
        "format_version = 8\nid = \"media.test\"\nversion = \"1.0.0\"\nname = \"Media Test\"\n"
        "[[target]]\ngame_id = \"READER\"\n"
        "[[feature]]\nid = \"active\"\nname = \"Active\"\ndefault_enabled = true\n"
        "[[feature]]\nid = \"private\"\nname = \"Private\"\n"
        "[[resource]]\nfeature = \"active\"\nid = \"rom\"\nlabel = \"ROM\"\n"
        "format = \"n64-rom\"\nrequired = true\nsize = 64\nsha256 = \"" + sha256_hex(rom) + "\"\n"
        "[[resource]]\nfeature = \"active\"\nid = \"xa\"\nlabel = \"XA\"\n"
        "format = \"file\"\nrequired = true\nsize = 4672\nsha256 = \"" + sha256_hex(xa) + "\"\n"
        "[[resource]]\nfeature = \"private\"\nid = \"private\"\nlabel = \"Private\"\n"
        "format = \"n64-rom\"\nrequired = true\nsize = 64\nsha256 = \"" + sha256_hex(rom) + "\"\n"
        "[[plugin]]\nfeature = \"active\"\nid = \"media.test.plugin\"\n");
    write_text(media_root / "state.toml",
        "format_version = 2\n[[feature]]\npackage_id = \"media.test\"\nid = \"active\"\nenabled = true\n"
        "[feature.resources]\nrom = \"" + rom_path.generic_string() + "\"\nxa = \"" + xa_path.generic_string() + "\"\n");
    check(psx_mod_register_activation_plugin("media.test.plugin", test_media_plugin), "register media consumer");
    check(PSXRecompV4::mod_runtime_initialize(media_root, "READER", 0, {}, &error), "media initialize");
    check(PSXRecompV4::mod_runtime_commit(iso_path, &error), "verify and commit owner media");
    const std::string host_media_fp = PSXRecompV4::mod_runtime_plan_fingerprint_portable();
    check(host_media_fp.size() == 64, "verified external media has a portable netplay identity");
    test_match_caps = {};
    check(PSXRecompV4::mod_runtime_commit_for_direct_netplay(iso_path, &error), "LAN retains verified selected content");
    check(PSXRecompV4::mod_runtime_session_plan_fp() == host_media_fp, "LAN announces full portable plan identity");
    PSXRecompV4::mod_runtime_end_netplay();
    check(PSXRecompV4::mod_runtime_session_plan_fp().empty(), "LAN return retires session identity");
    const auto guest_rom_path = media_root / "different-machine" / "guest.z64";
    write_bytes(guest_rom_path, rom);
    const std::string guest_state =
        "format_version = 2\n[[feature]]\npackage_id = \"media.test\"\nid = \"active\"\nenabled = false\n"
        "[feature.resources]\nrom = \"" + guest_rom_path.generic_string() + "\"\nxa = \"" + xa_path.generic_string() + "\"\n";
    write_text(media_root / "state.toml", guest_state);
    auto read_guest_state = [&]() {
        std::ifstream in(media_root / "state.toml", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string guest_state_bytes = read_guest_state();
    check(PSXRecompV4::mod_runtime_initialize(media_root, "READER", 0, {}, &error), "guest media initialize");
    test_match_caps.valid = 1;
    test_match_caps.mod_count = 1;
    std::snprintf(test_match_caps.mods[0].id, sizeof(test_match_caps.mods[0].id), "media.test");
    std::snprintf(test_match_caps.mods[0].ver, sizeof(test_match_caps.mods[0].ver), "1.0.0");
    std::snprintf(test_match_caps.mods[0].feats, sizeof(test_match_caps.mods[0].feats), "active");
    std::snprintf(test_match_caps.mod_plan_fp, sizeof(test_match_caps.mod_plan_fp), "%s", host_media_fp.c_str());
    check(PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "host plan enables guest media with different local paths");
    check(PSXRecompV4::mod_runtime_plan_fingerprint_portable() == host_media_fp, "guest content identity matches host");
    check(read_guest_state() == guest_state_bytes, "online plan preserves offline selection and resource paths byte for byte");
    const std::string committed_fp = PSXRecompV4::mod_runtime_fingerprint();
    test_match_caps.mod_plan_fp[0] = host_media_fp[0] == 'a' ? 'b' : 'a';
    check(!PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "mismatched host content fingerprint refused");
    check(PSXRecompV4::mod_runtime_fingerprint() == committed_fp, "mismatch leaves previous committed plan intact");
    test_match_caps.mod_plan_fp[0] = '\0';
    check(!PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "missing host fingerprint refused");
    std::snprintf(test_match_caps.mod_plan_fp, sizeof(test_match_caps.mod_plan_fp), "%s", host_media_fp.c_str());
    auto bad_rom = rom; bad_rom[33] ^= 1; write_bytes(guest_rom_path, bad_rom);
    check(!PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "changed donor media refused before online launch");
    check(read_guest_state() == guest_state_bytes, "failed online launch leaves offline state untouched");
    write_bytes(guest_rom_path, rom);
    check(PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "valid donor media recovers after refused launch");
    const uint8_t* unavailable = nullptr;
    uint64_t unavailable_size = 0;
    check(!psx_mod_current_resource_bytes("rom", &unavailable, &unavailable_size), "bytes unavailable outside plugin callback");
    mod_runtime_activate_plugins();
    check(media_access && media_size == rom.size() &&
          std::equal(rom.begin(), rom.end(), media_snapshot), "plugin receives exact verified bytes");
    check(!foreign_media_access, "inactive feature cannot supply bytes to another feature");
    check(extent_ok && mounted_lba==24 && !invalid_extent && !foreign_extent,"activation mounts only owned bounded resource extents");
    check(audio_ok && audio_lba==150 && audio_lba2==151 && !audio_invalid &&
          !audio_foreign && audio_data_rejected,"audio playlist validates bounds, ownership and audio source kind");
    uint32_t bad_lba=1;
    check(!psx_mod_append_disc_extent("xa",0,1,&bad_lba)&&!bad_lba,"late mounting refused");
    void* mounted=iso_open(iso_path.string().c_str());
    check(mounted&&iso_sector_count(mounted)==24,"BIOS sees original disc before activation is enabled");
    check(iso_cdda_track_count(mounted)==1,"audio override is invisible during BIOS boot");
    mod_runtime_enable_disc_patches();
    check(iso_cdda_track_count(mounted)==3 && iso_cdda_track_start_lba(mounted,2)==150 &&
          iso_cdda_track_start_lba(mounted,3)==151 && iso_cdda_sector_count(mounted)==152 &&
          !iso_cdda_track_is_audio(mounted,1) && iso_cdda_track_is_audio(mounted,3) &&
          !iso_cdda_track_is_audio(mounted,4),"audio-only TOC and lead-out stay separate from XA extents");
    check(iso_read_cdda_sector(mounted,150,sector.data(),sector.size()) &&
          std::equal(sector.begin(),sector.begin()+2352,xa.begin()),"lossless external CD audio sector");
    check(iso_read_cdda_sector(mounted,151,sector.data(),sector.size()) &&
          std::equal(sector.begin(),sector.begin()+2352,xa.begin()+2320),"audio track boundary resolves next resource slice");
    check(!iso_read_cdda_sector(mounted,152,sector.data(),sector.size()) &&
          !iso_read_cdda_sector(mounted,149,sector.data(),sector.size()) &&
          !iso_read_cdda_sector(mounted,150,sector.data(),2351),"audio end, data placeholder and short buffer rejected");
    std::array<uint8_t,12> subq{};int valid=0;
    check(iso_sector_count(mounted)==26&&iso_track_count(mounted)==2&&
          iso_track_start_lba(mounted,2)==24&&!iso_track_is_audio(mounted,2),"appended extents expose one data track and leadout");
    check(iso_read_raw_sector(mounted,24,sector.data(),sector.size())&&sector[12]==0&&sector[13]==2&&sector[14]==0x24&&sector[15]==2&&
          std::equal(xa.begin(),xa.begin()+2336,sector.begin()+16),"native raw sector header and exact XA subheader/audio bytes");
    check(iso_read_sector(mounted,25,sector.data(),sector.size())&&sector[0]==0x63,"cooked access resolves the same extent");
    check(!iso_read_sector(mounted,26,sector.data(),sector.size())&&
          !iso_read_raw_sector(mounted,24,sector.data(),2351),"extent end and short raw buffers rejected");
    check(iso_read_subq(mounted,24,subq.data(),subq.size(),&valid)&&valid&&subq[0]==0x41&&subq[1]==2&&subq[5]==0,"appended data track has native subchannel position");
    check(iso_read_sector(mounted,22,sector.data(),sector.size())&&sector[1]==7,"base sectors unchanged");
    xa[8]=0xff;write_bytes(xa_path,xa);
    check(iso_read_raw_sector(mounted,24,sector.data(),sector.size())&&sector[24]==0x52,"streaming uses verified snapshot after external file changes");
    mod_runtime_activate_plugins();
    check(mounted_lba==24&&iso_sector_count(mounted)==26,"reactivation has stable LBAs and no duplicate extents");
    check(iso_cdda_track_count(mounted)==3 && iso_cdda_sector_count(mounted)==152 &&
          iso_read_cdda_sector(mounted,150,sector.data(),sector.size()) && sector[8]==0x52,
          "reactivation retains audio identity and immutable snapshot");
    rom[32] ^= 1;
    write_bytes(guest_rom_path, rom);
    check(media_snapshot && media_snapshot[32] == 0x5a, "committed bytes survive owner file changes");
    check(!PSXRecompV4::mod_runtime_commit({}, &error), "new commit rejects modified media");
    check(PSXRecompV4::mod_runtime_clear_for_netplay(&error), "netplay clears donor plan");
    check(iso_sector_count(mounted)==24&&iso_track_count(mounted)==1&&
          !iso_read_raw_sector(mounted,24,sector.data(),sector.size()),"clearing plan removes all donor sectors and restores TOC");
    check(iso_cdda_track_count(mounted)==1 && iso_cdda_sector_count(mounted)==24,
          "clearing plan also restores original audio TOC");
    iso_close(mounted);
    PSXRecompV4::mod_runtime_end_netplay();
    check(PSXRecompV4::mod_runtime_session_plan_fp().empty(), "leaving online clears host fingerprint");
    check(PSXRecompV4::mod_runtime_commit(iso_path, &error), "offline commit after netplay uses original disabled media selection");
    mod_runtime_activate_plugins();
    const uint8_t* ended_bytes = nullptr; uint64_t ended_size = 0;
    check(!psx_mod_current_resource_bytes("rom", &ended_bytes, &ended_size), "online media is retired after session");
    {
        PSXRecompV4::ModPackageManager offline;
        offline.set_root(media_root);
        check(offline.scan(&error) && offline.load_state(&error) && !offline.feature_enabled("media.test", "active"),
              "later offline save keeps original disabled feature rather than host selection");
    }
    /* Path-only files are still usable offline, but never portable online. */
    const auto loose_root = root / "unverified-media";
    write_text(loose_root / "packages/loose.test/1.0.0/manifest.toml",
        "format_version = 8\nid = \"loose.test\"\nversion = \"1.0.0\"\nname = \"Loose\"\n"
        "[[target]]\ngame_id = \"READER\"\n[[feature]]\nid = \"active\"\nname = \"Active\"\ndefault_enabled = true\n"
        "[[resource]]\nfeature = \"active\"\nid = \"file\"\nlabel = \"File\"\nformat = \"file\"\nrequired = true\n");
    write_text(loose_root / "state.toml",
        "format_version = 2\n[[feature]]\npackage_id = \"loose.test\"\nid = \"active\"\nenabled = true\n"
        "[feature.resources]\nfile = \"" + rom_path.generic_string() + "\"\n");
    check(PSXRecompV4::mod_runtime_initialize(loose_root, "READER", 0, {}, &error), "loose resource initialize");
    check(PSXRecompV4::mod_runtime_commit(iso_path, &error), "unverified file remains usable offline");
    check(PSXRecompV4::mod_runtime_plan_fingerprint_portable().empty(), "unverified resource has no online fingerprint");
    const auto rejected_commit_timing = launch_timing_total();
    check(!PSXRecompV4::mod_runtime_commit(iso_path, &error, false), "online commit rejects unverified resource");
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    check_commit_timing(rejected_commit_timing, false, true);
#else
    check_commit_timing(rejected_commit_timing, false, false);
#endif
    check(!PSXRecompV4::mod_runtime_commit_for_direct_netplay(iso_path, &error), "LAN also refuses unverified resources");
    test_match_caps = {};
    test_match_caps.valid = 1;
    check(PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "explicit empty host plan remains vanilla");
    // A guest may own the source without ever enabling/preparing the host's
    // feature offline. Preparation must precede the portable hash comparison.
    {
        const auto prepared_root = root / "prepared-netplay";
        const auto prepared_asset = prepared_root / "generated" / "rom.z64";
        int prepare_calls = 0;
        check(PSXRecompV4::mod_register_media_preparer("test.netplay.prepare",
            [&](const PSXRecompV4::ModPrepareContext&,
                std::map<std::string, fs::path>& output, std::string&) {
                ++prepare_calls; write_bytes(prepared_asset, rom);
                output["rom"] = prepared_asset; return true;
            }), "register netplay preparation fixture");
        write_text(prepared_root / "packages/prepared.test/1.0.0/manifest.toml",
            "format_version = 9\nid = \"prepared.test\"\nversion = \"1.0.0\"\nname = \"Prepared\"\nprepare = \"test.netplay.prepare\"\n"
            "[[target]]\ngame_id = \"READER\"\n[[feature]]\nid = \"active\"\nname = \"Active\"\ndefault_enabled = true\n"
            "[[resource]]\nfeature = \"active\"\nid = \"rom\"\nlabel = \"ROM\"\nformat = \"n64-rom\"\nhidden = true\nrequired = true\nsize = 64\nsha256 = \"" + sha256_hex(rom) + "\"\n");
        PSXRecompV4::mod_runtime_end_netplay(); test_match_caps = {};
        check(PSXRecompV4::mod_runtime_initialize(prepared_root, "READER", 0, {}, &error) &&
              PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "direct launch prepares its selected resources");
        const std::string prepared_fp = PSXRecompV4::mod_runtime_session_plan_fp();
        PSXRecompV4::mod_runtime_end_netplay();
        const std::string disabled = "format_version = 2\n[[feature]]\npackage_id = \"prepared.test\"\nid = \"active\"\nenabled = false\n";
        write_text(prepared_root / "state.toml", disabled);
        check(PSXRecompV4::mod_runtime_initialize(prepared_root, "READER", 0, {}, &error), "unprepared guest initialize");
        test_match_caps.valid = 1; test_match_caps.mod_count = 1;
        std::snprintf(test_match_caps.mod_plan_fp, sizeof(test_match_caps.mod_plan_fp), "%s", prepared_fp.c_str());
        std::snprintf(test_match_caps.mods[0].id, sizeof(test_match_caps.mods[0].id), "prepared.test");
        std::snprintf(test_match_caps.mods[0].ver, sizeof(test_match_caps.mods[0].ver), "1.0.0");
        std::snprintf(test_match_caps.mods[0].feats, sizeof(test_match_caps.mods[0].feats), "active");
        check(PSXRecompV4::mod_runtime_commit_for_netplay(iso_path, &error), "host enables and prepares guest feature before verification");
        check(prepare_calls == 2 && PSXRecompV4::mod_runtime_plan_fingerprint_portable() == prepared_fp,
              "prepared guest resolves identical verified bytes");
        std::ifstream saved(prepared_root / "state.toml");
        check(std::string(std::istreambuf_iterator<char>(saved), {}) == disabled, "prepared online launch preserves disabled offline choice");
        PSXRecompV4::mod_runtime_end_netplay(); test_match_caps = {};
    }
    // The audio timeline skips INDEX00 pregaps and maps into the unchanged
    // mounted BIN, including a second audio track sharing the same file.
    const auto audio_root=root/"disc-audio";
    std::vector<uint8_t> pcm(8*2352);
    for(unsigned i=0;i<pcm.size();++i)pcm[i]=uint8_t(i/2352+17);
    const auto pcm_path=audio_root/"audio.bin";write_bytes(pcm_path,pcm);
    const auto audio_cue_path=audio_root/"disc.cue";
    write_text(audio_cue_path,"FILE \""+raw_path.generic_string()+"\" BINARY\n"
        "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
        "FILE \"audio.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:00:02\n"
        "  TRACK 03 AUDIO\n    INDEX 00 00:00:05\n    INDEX 01 00:00:06\n");
    write_text(audio_root/"packages/audio.test/1.0.0/manifest.toml",
        "format_version = 8\nid = \"audio.test\"\nversion = \"1.0.0\"\nname = \"Audio\"\n"
        "[[target]]\ngame_id = \"READER\"\n[[feature]]\nid = \"active\"\nname = \"Active\"\ndefault_enabled = true\n"
        "[[plugin]]\nfeature = \"active\"\nid = \"audio.test.plugin\"\n");
    PSXRecompV4::mod_runtime_end_netplay();
    check(psx_mod_register_activation_plugin("audio.test.plugin",test_disc_audio_plugin),"register mounted audio fixture");
    check(PSXRecompV4::mod_runtime_initialize(audio_root,"READER",0,{},&error) &&
          PSXRecompV4::mod_runtime_commit(audio_cue_path,&error),"mount audio fixture");
    mod_runtime_activate_plugins();mod_runtime_enable_disc_patches();
    mounted=iso_open(audio_cue_path.string().c_str());
    check(mounted && iso_cdda_sector_count(mounted)==155 &&
          iso_read_cdda_sector(mounted,150,sector.data(),sector.size()) &&
          std::all_of(sector.begin(),sector.begin()+2352,[](auto b){return b==19;}),"first mounted audio track excludes pregap");
    check(iso_read_cdda_sector(mounted,153,sector.data(),sector.size()) &&
          std::all_of(sector.begin(),sector.begin()+2352,[](auto b){return b==23;}),"shared-file second track maps its INDEX01");
    check(iso_read_sector(mounted,22,sector.data(),sector.size()) && sector[1]==7,"audio remapping preserves data reads");
    check(!iso_read_cdda_sector(mounted,155,sector.data(),sector.size()),"mounted audio leadout is exclusive");
    iso_close(mounted);
    /* Plugin activation (native asset preparation) runs before the emulated
     * drive's disc patches are enabled. Host reads must already return the
     * committed plan's effective bytes, or a prepared cache would silently
     * serve original assets under a modded plan. The drive path stays gated. */
    {
        const auto patched_root = root / "host-reader-patched";
        const auto patched_iso = patched_root / "original.iso";
        /* A byte past the original 3000-byte extent; a mod that grows the file
         * through its directory record exposes it. */
        std::vector<uint8_t> grown_iso = iso;
        grown_iso[22 * 2048 + 3500] = 0x77;
        write_bytes(patched_iso, grown_iso);
        write_text(patched_root / "packages/reader.patch/1.0.0/manifest.toml",
            "format_version = 5\n"
            "id = \"reader.patch\"\n"
            "version = \"1.0.0\"\n"
            "name = \"Reader Patch\"\n"
            "[[target]]\n"
            "game_id = \"READER\"\n"
            "disc_sha256 = \"" + sha256_hex(grown_iso) + "\"\n"
            "[[feature]]\n"
            "id = \"asset\"\n"
            "name = \"Asset\"\n"
            "[[feature]]\n"
            "id = \"grow\"\n"
            "name = \"Grow\"\n"
            "[[patch]]\n"
            "feature = \"grow\"\n"
            "target = \"disc_user\"\n"
            "offset = " + std::to_string(21 * 2048 + 10) + "\n"
            "expected = \"b80b0000\"\n"
            "replace = \"a00f0000\"\n"
            "[[feature]]\n"
            "id = \"huge\"\n"
            "name = \"Huge\"\n"
            "[[patch]]\n"
            "feature = \"huge\"\n"
            "target = \"disc_user\"\n"
            "offset = " + std::to_string(21 * 2048 + 10) + "\n"
            "expected = \"b80b0000\"\n"
            "replace = \"00f82204\"\n"
            "[[patch]]\n"
            "feature = \"asset\"\n"
            "target = \"disc_user\"\n"
            "offset = " + std::to_string(22 * 2048 + 5) + "\n"
            "expected = \"23\"\n"
            "replace = \"5a\"\n");
        write_text(patched_root / "state.toml",
            "format_version = 2\n"
            "[[feature]]\n"
            "package_id = \"reader.patch\"\n"
            "id = \"asset\"\n"
            "enabled = true\n"
            "[[feature]]\n"
            "package_id = \"reader.patch\"\n"
            "id = \"grow\"\n"
            "enabled = true\n");
        check(PSXRecompV4::mod_runtime_initialize(patched_root, "READER", 0, {}, &error),
              "patched reader initialize");
        check(PSXRecompV4::mod_runtime_commit(patched_iso, &error), "patched reader commit");
        check(psx_mod_read_disc_file("S0/LEVEL.NSF", nullptr, 0, &bytes) && bytes == 4000,
              "host disc reads resolve paths through patched directory records");
        std::vector<uint8_t> effective(4000);
        check(psx_mod_read_disc_file("S0/LEVEL.NSF", effective.data(),
                                     (uint32_t)effective.size(), &bytes) &&
                  bytes == 4000 && effective[5] == 0x5a && effective[3500] == 0x77 &&
                  std::equal(effective.begin() + 6, effective.begin() + 3000,
                             iso.begin() + 22 * 2048 + 6),
              "host disc reads apply the committed plan before the drive is enabled");
        check(!psx_mod_read_disc_file("S0", nullptr, 0, &bytes),
              "effective lookup still rejects directories");
        std::array<uint8_t, 2048> drive_sector{};
        std::copy_n(iso.begin() + 22 * 2048, 2048, drive_sector.begin());
        mod_runtime_patch_disc_sector(22, 0, drive_sector.data(), (uint32_t)drive_sector.size());
        check(drive_sector[5] == 0x23,
              "the emulated drive path stays original until disc patches are enabled");
        mod_runtime_enable_disc_patches();
        mod_runtime_patch_disc_sector(22, 0, drive_sector.data(), (uint32_t)drive_sector.size());
        check(drive_sector[5] == 0x5a, "enabled drive path applies the same plan");
        /* Grown archives may exceed 64 MiB (an extended MMX6 ROCK_X6.DAT is
         * 69,400,576 bytes); the bound is CD capacity, not a fixed cap. */
        write_text(patched_root / "state.toml",
            "format_version = 2\n"
            "[[feature]]\n"
            "package_id = \"reader.patch\"\n"
            "id = \"huge\"\n"
            "enabled = true\n");
        check(PSXRecompV4::mod_runtime_initialize(patched_root, "READER", 0, {}, &error) &&
                  PSXRecompV4::mod_runtime_commit(patched_iso, &error), "huge plan commit");
        check(psx_mod_read_disc_file("S0/LEVEL.NSF", nullptr, 0, &bytes) && bytes == 69400576u,
              "files grown past 64 MiB keep their effective size");
    }
    {
        const fs::path hd_mods = root / "hd-mods";
        write_text(hd_mods / "bundled" / "runtime.hd" / "1.0.0" / "manifest.toml",
            "format_version = 5\nid = \"runtime.hd\"\nversion = \"1.0.0\"\n"
            "name = \"HD\"\nresolver = \"declarative\"\nsave_compatibility = \"shared\"\n"
            "[[target]]\ngame_id = \"SCUS-94236\"\n"
            "[[feature]]\nid = \"textures\"\nname = \"HD\"\ndefault_enabled = false\n"
            "[[resource]]\nfeature = \"textures\"\nid = \"pack\"\nlabel = \"Pack\"\n"
            "format = \"directory\"\nrequired = true\n"
            "[[option]]\nfeature = \"textures\"\nid = \"replacements\"\n"
            "label = \"Load\"\ntype = \"boolean\"\ndefault = \"true\"\n"
            "[[option]]\nfeature = \"textures\"\nid = \"dump\"\n"
            "label = \"Dump\"\ntype = \"boolean\"\ndefault = \"false\"\n"
            "[[plugin]]\nfeature = \"textures\"\nid = \"psx.hd-textures\"\n");
        write_text(hd_mods / "state.toml",
            "format_version = 2\n[[feature]]\npackage_id = \"runtime.hd\"\n"
            "id = \"textures\"\nenabled = true\n[feature.values]\ndump = \"true\"\n");
        check(psx_mod_register_activation_plugin("psx.hd-textures", test_hd_activation),
              "HD activation plugin registers");
        check(PSXRecompV4::mod_runtime_initialize(hd_mods, "SCUS-94236", 0, {}, &error),
              "HD mods initialize");
        const fs::path expected = fs::absolute(hd_mods / "texture-packs" / "SCUS-94236");
        check(fs::is_directory(expected / "dumps") && fs::is_directory(expected / "replacements"),
              "default HD directories exist before first launch, even without dumping");
        check(PSXRecompV4::mod_runtime_commit(stock_path, &error),
              "default pack resource commits without manual folder selection");
        const int shutdown_before = hd_shutdown_calls;
        mod_runtime_activate_plugins();
        check(hd_shutdown_calls == shutdown_before + 1 && hd_configure_calls == 1 &&
                  hd_root == expected.string() && hd_replacements_value == 1 && hd_dump_value == 1,
              "HD activation clears previous session and reads owning committed options");
        char value[16] = "old";
        check(!psx_mod_current_option_value("dump", value, sizeof(value)) && value[0] == '\0',
              "owning option context does not escape the trusted callback");
        check(!psx_mod_set_hd_texture_pack("pack", 1, 1),
              "pack configuration requires the trusted resource callback context");
        hd_replacements_value = 0;
        check(psx_mod_set_hd_texture_dump(0) && hd_dump_value == 0,
              "live dump control forwards the requested switch");
        check(!gpu_hd_textures_active() && psx_mod_set_hd_texture_dump(1) && hd_dump_value == 1,
              "dump-only session can turn capture back on after turning it off");
        check(psx_mod_reload_hd_texture_pack() && hd_reload_calls == 1,
              "pack reload forwards to renderer");
        check(PSXRecompV4::mod_runtime_clear_for_netplay(&error), "HD plan clears");
        mod_runtime_activate_plugins();
        check(hd_shutdown_calls == shutdown_before + 2 && hd_configure_calls == 1,
              "disabled HD feature leaves no previous session pack active");
        check(!psx_mod_set_hd_texture_dump(1), "live dump control refuses an unconfigured session");
        const fs::path custom = hd_mods / fs::u8path(u8"chosen pack # % \u65e5\u672c");
        fs::create_directories(custom);
        write_text(hd_mods / "state.toml",
            "format_version = 2\n[[feature]]\npackage_id = \"runtime.hd\"\n"
            "id = \"textures\"\nenabled = true\n[feature.resources]\npack = \"" +
            custom.generic_u8string() + "\"\n");
        check(PSXRecompV4::mod_runtime_initialize(hd_mods, "SCUS-94236", 0, {}, &error) &&
                  PSXRecompV4::mod_runtime_commit(stock_path, &error),
              "chosen Unicode pack path resolves and persists through commit");
        mod_runtime_activate_plugins();
        check(hd_root == custom.generic_u8string(), "renderer receives the chosen folder as UTF-8");
        check(PSXRecompV4::mod_runtime_initialize(hd_mods, "SCUS-94236", 0, {}, &error) &&
                  PSXRecompV4::mod_runtime_commit(stock_path, &error),
              "committed Unicode pack path reloads without corruption");
        mod_runtime_activate_plugins();
        check(hd_root == custom.generic_u8string(), "Unicode pack root survives the complete session cycle");
    }
    /* A compiled game hook survives mod-plan clearing, but runs only online. */
    check(psx_game_register_netplay_function_entry(
              0x80004000u, test_game_netplay_entry) == 1,
          "trusted game netplay entry must register");
    check(psx_game_register_netplay_function_entry(
              0x80004000u, test_game_netplay_entry) == 1,
          "duplicate game netplay registration must be idempotent");
    check(psx_game_register_netplay_function_filter(
              0x80005000u, test_game_netplay_filter) == 1,
          "trusted game netplay filter must register");
    CPUState game_cpu{};
    psx_mod_function_entry(&game_cpu, 0x80004000u);
    game_cpu.gpr[31] = 0x80006000u;
    check(!psx_mod_function_entry(&game_cpu, 0x80005000u),
          "game netplay filter must not consume offline function");
    check(game_netplay_entry_hits == 0,
          "game netplay hook must not run offline");
    check(PSXRecompV4::mod_runtime_clear_for_netplay(&error), error.c_str());
    test_netplay_active = 1;
    psx_mod_function_entry(&game_cpu, 0xA0004000u);
    check(game_netplay_entry_hits == 1 && g_psx_mod_function_entry_hooks == 2,
          "game hook must survive netplay mod clear and match code aliases");
    check(psx_mod_function_entry(&game_cpu, 0xA0005000u) == 1 &&
              game_cpu.pc == 0x80006000u && game_cpu.gpr[2] == 0x1234u &&
              game_netplay_filter_hits == 1,
          "game netplay filter must consume call and resume at return PC");
    psx_mod_function_entry(&game_cpu, 0x80004004u);
    check(game_netplay_entry_hits == 1,
          "game hook must ignore other addresses");
    test_netplay_active = 0;
    psx_mod_function_entry(&game_cpu, 0x80004000u);
    check(game_netplay_entry_hits == 1,
          "game hook must disarm when netplay stops");
    check(!psx_mod_function_entry(&game_cpu, 0x80005000u) &&
              game_netplay_filter_hits == 1,
          "game filter must disarm when netplay stops");
    // Reliable identity detects byte replacement even when size/mtime are restored.
    {
        const auto identity_path = root / "identity.bin";
        write_bytes(identity_path, {1, 2, 3, 4});
        const auto original_time = fs::last_write_time(identity_path);
        std::string before, after;
        const bool reliable = PSXRecompV4::host_file_identity(identity_path, before);
        write_bytes(identity_path, {4, 3, 2, 1});
        fs::last_write_time(identity_path, original_time);
        check(!reliable || (PSXRecompV4::host_file_identity(identity_path, after) && before != after),
              "file identity rejects same-size restored-mtime replacement");
        check(!PSXRecompV4::host_file_identity(root, after), "directories are never file cache identities");
        check(!PSXRecompV4::host_file_identity(root / "absent.bin", after), "missing identity is a cache miss");
    }
#if defined(PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE)
    {
        const auto cached_root = root / "audited-cache";
        const auto output_path = root / "audited-output.z64";
        const auto input_path = root / "audited-input.bin";
        const auto disc = root / "audited-disc.iso";
        fs::copy_file(iso_path, disc, fs::copy_options::overwrite_existing);
        write_bytes(input_path, {1,2,3,4});
        std::string input_receipt, output_receipt;
        int prepare_calls = 0;
        bool throw_probe = false, mutate_on_resolve = false, throw_resolver = false;
        const std::string manifest =
            "format_version = 9\nid = \"cache.test\"\nversion = \"1.0.0\"\nname = \"Cache\"\nprepare = \"test.cache.prepare\"\nresolver = \"builtin:test.cache.resolve\"\n"
            "[[target]]\ngame_id = \"READER\"\n"
            "[[feature]]\nid = \"active\"\nname = \"Active\"\ndefault_enabled = true\n"
            "[[resource]]\nfeature = \"active\"\nid = \"source\"\nlabel = \"Source\"\ninput_only = true\nshared_source = \"cache.original\"\nrequired = true\n"
            "[[resource]]\nfeature = \"active\"\nid = \"rom\"\nlabel = \"ROM\"\nformat = \"n64-rom\"\nhidden = true\nrequired = true\nsize = 64\nsha256 = \"" + sha256_hex(rom) + "\"\n"
            "[[feature]]\nid = \"second\"\nname = \"Second\"\ndefault_enabled = false\n"
            "[[resource]]\nfeature = \"second\"\nid = \"rom\"\nlabel = \"ROM\"\nformat = \"n64-rom\"\nhidden = true\nrequired = true\nsize = 64\nsha256 = \"" + sha256_hex(rom) + "\"\n";
        check(PSXRecompV4::mod_register_builtin_resolver("test.cache.resolve",
            [&](const auto&, const auto&, const auto&, auto&, auto&) {
                if (throw_resolver) throw std::runtime_error("resolver fixture exception");
                if (mutate_on_resolve) write_bytes(input_path, {7,7,7,7});
                return true;
            }), "register resolver for mutation and unwind regression");
        const auto manifest_path = cached_root / "packages/cache.test/1.0.0/manifest.toml";
        write_text(manifest_path, manifest);
        check(PSXRecompV4::mod_register_media_preparer("test.cache.prepare",
            [&](const PSXRecompV4::ModPrepareContext&, std::map<std::string, fs::path>& outputs, std::string&) {
                ++prepare_calls;
                write_bytes(output_path, rom);
                PSXRecompV4::host_file_identity(input_path, input_receipt);
                PSXRecompV4::host_file_identity(output_path, output_receipt);
                outputs["rom"] = output_path;
                return true;
            },
            [&](const PSXRecompV4::ModPrepareContext& context, std::map<std::string, fs::path>& outputs, std::string&) {
                if (throw_probe) throw std::runtime_error("probe fixture exception");
                std::string input, output;
                if (context.feature_id == "second" ||
                    !PSXRecompV4::host_file_identity(input_path, input) || input != input_receipt ||
                    !PSXRecompV4::host_file_identity(output_path, output) || output != output_receipt) return false;
                outputs["rom"] = output_path; return true;
            }), "register audited preparation and cheap identity probe");
        PSXRecompV4::ModPackageManager source_selection(cached_root);
        check(source_selection.scan(&error) &&
              source_selection.set_feature_resource_path("cache.test", "active", "source", input_path, &error) &&
              source_selection.save_state(&error), "persist shared input selection for audited fixture");
        PSXRecompV4::mod_runtime_end_netplay(); test_match_caps = {};
        check(PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error), "audited initialize");
        check(!PSXRecompV4::mod_runtime_try_prepare_cached(disc) && prepare_calls == 0,
              "cold cached-only launch does not prepare missing receipt");
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) && prepare_calls == 1,
              "cold launcher creates verified preparation");
        const auto reused = startup_counter("startup.plan_reuse");
        const auto hashed = startup_counter("startup.disc_hash");
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) &&
              PSXRecompV4::mod_runtime_commit(disc, &error) && prepare_calls == 1 &&
              startup_counter("startup.plan_reuse") == reused + 2 && startup_counter("startup.disc_hash") == hashed,
              "duplicate provider and main commit reuse a prepared plan without converter or hash");
        check(PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error) &&
              PSXRecompV4::mod_runtime_try_prepare_cached(disc) && prepare_calls == 1 &&
              startup_counter("startup.disc_hash") == hashed,
              "warm initialize preloads receipts with no preparer or raw hash");
        mod_runtime_activate_plugins();
        check(PSXRecompV4::mod_runtime_commit(disc, &error) && prepare_calls == 2,
              "activation invalidates the preboot ticket");
        check(PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error) &&
              PSXRecompV4::mod_runtime_try_prepare_cached(disc), "restore warm plan for source invalidation");
        const auto input_time = fs::last_write_time(input_path);
        write_bytes(input_path, {4,3,2,1}); fs::last_write_time(input_path, input_time);
        check(!PSXRecompV4::mod_runtime_try_prepare_cached(disc), "external shared input replacement invalidates title receipt");
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) && prepare_calls == 3,
              "changed source goes through full preparation");
        auto corrupt_rom = rom; corrupt_rom[32] ^= 1; write_bytes(output_path, corrupt_rom);
        // Even if the probe claims the file is current, the first warm resolve
        // must verify output bytes against the declared SHA.
        PSXRecompV4::host_file_identity(output_path, output_receipt);
        check(PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error) &&
              !PSXRecompV4::mod_runtime_try_prepare_cached(disc) && prepare_calls == 3,
              "warm output corruption fails immutable-byte verification");
        fs::remove(output_path);
        check(!PSXRecompV4::mod_runtime_try_prepare_cached(disc) && prepare_calls == 3,
              "missing output fails without invoking converter");
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) && prepare_calls == 4,
              "full preparation repairs missing output");
        throw_probe = true;
        check(!PSXRecompV4::mod_runtime_try_prepare_cached(disc), "throwing cached probe fails safely");
        throw_probe = false;
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error), "cached flag restored after failure");
        // Probe results are transactional across multiple enabled features.
        PSXRecompV4::ModPackageManager transactional(cached_root);
        check(transactional.scan(&error) && transactional.load_state(&error) &&
              transactional.set_feature_enabled("cache.test", "second", true, &error) &&
              transactional.set_feature_resource_path("cache.test", "active", "rom", rom_path, &error), "enable second probe fixture");
        const auto original_output = transactional.feature_resource_path("cache.test", "active", "rom");
        check(!transactional.prepare_resources("READER", disc, root / "cache", &error, true) &&
              transactional.feature_resource_path("cache.test", "active", "rom") == original_output,
              "later cache miss discards earlier pending output selections");
        // A throw inside resolve must restore cached-only/provider modes.
        check(PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error), "invalidate ticket for resolver exception fixture");
        throw_resolver = true;
        check(!PSXRecompV4::mod_runtime_try_prepare_cached(disc),
              "best-effort preload contains resolver exceptions before launcher UI");
        throw_resolver = false;
        fs::remove(output_path);
        const auto before_unwind = prepare_calls;
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) && prepare_calls == before_unwind + 1,
              "cached-only mode restored after resolve exception");
        // A dependency changed during resolution must never publish readiness.
        check(PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error), "invalidate ticket for resolver mutation fixture");
        const auto before_mutation = prepare_calls;
        mutate_on_resolve = true;
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error), "full resolution can finish with concurrent input mutation");
        mutate_on_resolve = false;
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) && prepare_calls == before_mutation + 2,
              "unstable resolve never publishes a reusable ticket");
        // Altering a syntactically valid digest must fail its receipt checksum.
        std::filesystem::path cache_dir;
#if defined(_WIN32)
        if (const char* local = std::getenv("LOCALAPPDATA")) cache_dir = local;
#else
        if (const char* cache = std::getenv("XDG_CACHE_HOME")) cache_dir = cache;
        else if (const char* home_dir = std::getenv("HOME")) cache_dir = fs::path(home_dir) / ".cache";
#endif
        if (cache_dir.empty()) cache_dir = fs::temp_directory_path();
        cache_dir /= "psxrecomp/imports/disc-digests-v2";
        bool corrupted_receipt = false;
        for (const auto& entry : fs::directory_iterator(cache_dir)) {
            std::ifstream saved(entry.path(), std::ios::binary);
            std::string receipt(std::istreambuf_iterator<char>(saved), {});
            if (receipt.find(disc.generic_string()) == std::string::npos || receipt.size() < 130) continue;
            const size_t digest_offset = receipt.size() - 130;
            receipt[digest_offset] = receipt[digest_offset] == '0' ? '1' : '0';
            write_text(entry.path(), receipt); corrupted_receipt = true;
        }
        check(corrupted_receipt && PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error) &&
              !PSXRecompV4::mod_runtime_try_prepare_cached(disc), "corrupt well-formed digest receipt misses warm startup");
        const auto before_corrupt_hash = startup_counter("startup.disc_hash");
        check(PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) &&
              startup_counter("startup.disc_hash") == before_corrupt_hash + 1,
              "full launch repairs corrupted digest with authoritative source hash");
        // Catalog changes cannot certify stale in-memory parsed manifests.
        write_text(manifest_path, manifest + "\n# external catalog mutation\n");
        check(!PSXRecompV4::mod_runtime_prepare_for_launcher(disc, &error) &&
              error.find("catalog changed") != std::string::npos,
              "changed manifest requires rescan instead of resealing stale package");
        check(PSXRecompV4::mod_runtime_initialize(cached_root, "READER", 0, {}, &error), "explicit initialize rescans changed catalog");
        const int before_netplay = prepare_calls;
        const auto net_hash = startup_counter("startup.disc_hash");
        check(PSXRecompV4::mod_runtime_commit_for_direct_netplay(disc, &error) &&
              prepare_calls == before_netplay + 1 && startup_counter("startup.disc_hash") > net_hash,
              "netplay ignores preboot ticket and hashes the source fully");
        check(!PSXRecompV4::mod_runtime_try_prepare_cached(disc), "active netplay never preloads an offline ticket");
        PSXRecompV4::mod_runtime_end_netplay();
        // Legacy preparers have no cheap receipt and must never claim warm readiness.
        const auto legacy_root = root / "legacy-cache";
        int legacy_calls = 0;
        check(PSXRecompV4::mod_register_media_preparer("test.legacy.prepare",
            [&](const PSXRecompV4::ModPrepareContext&, std::map<std::string, fs::path>& outputs, std::string&) {
                ++legacy_calls; outputs["rom"] = output_path; return true;
            }), "register legacy preparer without probe");
        std::string legacy_manifest = manifest;
        legacy_manifest.replace(legacy_manifest.find("test.cache.prepare"), std::string("test.cache.prepare").size(), "test.legacy.prepare");
        write_text(legacy_root / "packages/cache.test/1.0.0/manifest.toml", legacy_manifest);
        check(PSXRecompV4::mod_runtime_initialize(legacy_root, "READER", 0, {}, &error) &&
              !PSXRecompV4::mod_runtime_try_prepare_cached(disc) && legacy_calls == 0,
              "legacy preparer always misses cached-only startup");
    }
#else
    {
        const auto defaults = root / "default-hashing";
        fs::create_directories(defaults);
        check(PSXRecompV4::mod_runtime_initialize(defaults, "READER", 0, {}, &error), "default hashing fixture initialize");
        const auto hash_count = startup_counter("startup.disc_hash");
        check(PSXRecompV4::mod_runtime_commit(iso_path, &error) &&
              PSXRecompV4::mod_runtime_commit(iso_path, &error) &&
              startup_counter("startup.disc_hash") == hash_count + 1,
              "default runtime preserves hash-only-on-selected-path-change behavior");
        check(!PSXRecompV4::mod_runtime_try_prepare_cached(iso_path), "default runtime does not opt into cached startup");
    }
#endif
    fs::remove_all(root, ec);
    if (failures) return 1;
    std::cout << "mod runtime tests passed\n";
    return 0;
}

/* render_pass.c: no sandboxed local view runs in this test. */
extern "C" int psx_mod_local_view_scope(void) { return 0; }
