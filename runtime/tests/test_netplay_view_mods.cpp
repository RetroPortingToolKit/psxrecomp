#include "mod_runtime.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "psx_sha256.h"

#include "gpu.h"
#include "cpu_state.h"
#include "psx_lobby_client.h"
#include "gpu_hd_textures.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/* Netplay own-view mods (mod_runtime_commit_netplay_view): a feature whose
 * only contribution is a [[plugin]] with netplay = "local_view" stays on in a
 * match, per player; its hooks run only inside the sandboxed own-view render
 * (psx_mod_local_view_scope), never in the shared simulation; everything
 * else is cleared; state.toml is never rewritten. */
static std::array<uint8_t, 2 * 1024 * 1024> ram;
static int failures;
static int test_netplay_active = 1;
static int test_scope;
extern "C" int psx_netplay_active(void) { return test_netplay_active; }
extern "C" int psx_mod_local_view_scope(void) { return test_scope; }
extern "C" const PsxLobbyMatchCaps* psx_lobby_match_caps(void) { return nullptr; }
/* No renderer or external pack exists in this mod-policy fixture. */
extern "C" void gpu_hd_textures_shutdown(void) {}
extern "C" int gpu_hd_textures_configure(const char*, int, int, char*, size_t) { return 0; }
extern "C" void gpu_hd_textures_get_diag(GpuHdTextureDiag* out) { *out = {}; }
extern "C" void gpu_hd_textures_set_dump_enabled(int) {}
extern "C" int gpu_hd_textures_reload(char*, size_t) { return 0; }
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

static std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static int view_hits, sim_hits, view_activations, sim_activations, vblanks;
static void view_entry(CPUState*, uint32_t) { view_hits++; }
static void sim_entry(CPUState*, uint32_t) { sim_hits++; }
static void view_activate(void) { view_activations++; }
static void sim_activate(void) { sim_activations++; }
static void view_vblank(void) { vblanks++; }

static std::string package(const std::string& id, const std::string& feature,
                           const std::string& plugin, const std::string& extra) {
    return "format_version = 7\n"
           "id = \"" + id + "\"\nversion = \"1.0.0\"\nname = \"T\"\n"
           "[[target]]\ngame_id = \"SLUS-VIEW\"\n"
           "[[feature]]\nid = \"" + feature + "\"\nname = \"F\"\n"
           "[[plugin]]\nfeature = \"" + feature + "\"\nid = \"" + plugin + "\"\n" +
           extra;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "psx_netplay_view_mods_test";
    fs::remove_all(root);
    write_text(root / "bundled/view.pkg/1.0.0/manifest.toml",
               package("view.pkg", "wide", "view.wide", "netplay = \"local_view\"\n"));
    write_text(root / "bundled/sim.pkg/1.0.0/manifest.toml",
               package("sim.pkg", "cheat", "sim.cheat", ""));
    write_text(root / "bundled/bad.pkg/1.0.0/manifest.toml",
               package("bad.pkg", "x", "bad.x", "netplay = \"everywhere\"\n"));
    const std::string state =
        "format_version = 2\n"
        "[[feature]]\npackage_id = \"view.pkg\"\nid = \"wide\"\nenabled = true\n"
        "[[feature]]\npackage_id = \"sim.pkg\"\nid = \"cheat\"\nenabled = true\n";
    write_text(root / "state.toml", state);

    check(psx_mod_register_function_entry_plugin("view.wide", 0x80003000u, view_entry),
          "register view hook");
    check(psx_mod_register_function_entry_plugin("sim.cheat", 0x80003100u, sim_entry),
          "register sim hook");
    check(psx_mod_register_activation_plugin("view.wide", view_activate) &&
              psx_mod_register_activation_plugin("sim.cheat", sim_activate),
          "register activations");
    check(psx_mod_register_vblank_plugin("view.wide", view_vblank), "register vblank");

    std::string error;
    check(PSXRecompV4::mod_runtime_initialize(root, "SLUS-VIEW", 0x80002000u, {}, &error),
          error.c_str());

    /* Offline: both run in the game. */
    check(PSXRecompV4::mod_runtime_commit({}, &error), error.c_str());
    check(!PSXRecompV4::mod_runtime_netplay_view_active(), "offline plan is not own-view");
    mod_runtime_activate_plugins();
    CPUState cpu{};
    psx_mod_function_entry(&cpu, 0x80003000u);
    psx_mod_function_entry(&cpu, 0x80003100u);
    check(view_hits == 1 && sim_hits == 1, "offline hooks run in the game");

    /* Netplay: only the local-view feature remains, parked outside the scope. */
    view_hits = sim_hits = view_activations = sim_activations = vblanks = 0;
    const std::string saved = read_text(root / "state.toml");
    check(PSXRecompV4::mod_runtime_commit_netplay_view({}, &error), error.c_str());
    check(PSXRecompV4::mod_runtime_netplay_view_active(), "own-view plan active");
    mod_runtime_activate_plugins();
    check(view_activations == 1 && sim_activations == 0,
          "only the local-view plugin activates in a match");
    psx_mod_function_entry(&cpu, 0x80003000u);
    psx_mod_function_entry(&cpu, 0x80003100u);
    mod_runtime_on_vblank();
    check(view_hits == 0 && sim_hits == 0 && vblanks == 0,
          "the shared simulation runs no mod hook or vblank");
    test_scope = 1;
    psx_mod_function_entry(&cpu, 0x80003000u);
    psx_mod_function_entry(&cpu, 0x80003100u);
    test_scope = 0;
    check(view_hits == 1 && sim_hits == 0,
          "inside the own-view render only the local-view hook runs");
    check(read_text(root / "state.toml") == saved, "state.toml untouched by a match");

    /* A plan whose own-view feature also writes the game does not qualify. */
    PSXRecompV4::ModResolution plan;
    plan.plugins.push_back({"p", "a.pkg", "f", true});
    plan.plugins.push_back({"q", "b.pkg", "g", true});
    PSXRecompV4::ModResolution::Write w;
    w.package_id = "b.pkg";
    w.feature_id = "g";
    plan.writes.push_back(w);
    const auto keep = PSXRecompV4::mod_runtime_netplay_view_features(plan);
    check(keep.size() == 1 && keep[0] == "a.pkg/f",
          "a feature with a game write is not own-view");

    check(PSXRecompV4::mod_runtime_clear_for_netplay(&error) &&
              !PSXRecompV4::mod_runtime_netplay_view_active(),
          "a clear ends the own-view plan");

    fs::remove_all(root);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "netplay_view_mods_test: ok\n";
    return 0;
}
