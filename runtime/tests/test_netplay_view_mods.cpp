#include "mod_runtime.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "sio.h"
#include <cstring>
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
/* sio.c's environment (as in test_sio_jogcon_protocol.c). */
extern "C" {
uint32_t i_stat = 0, i_mask = 0;
uint32_t g_debug_current_func_addr = 0, g_debug_last_store_pc = 0;
int psx_get_in_exception(void) { return 0; }
uint64_t psx_get_cycle_count(void) { static uint64_t t; return t += 64; }
uint32_t memory_get_sr(void) { return 0; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t, uint32_t, uint8_t) {}
void starvation_ring_record(uint8_t, uint8_t, uint8_t, uint16_t, uint16_t,
                            uint8_t, uint8_t, uint16_t, uint8_t, uint16_t,
                            uint8_t, uint32_t) {}
void card_read_summary_record(uint8_t, uint8_t, uint16_t, uint8_t, uint8_t,
                              uint8_t, uint8_t, uint32_t) {}
void card_data_writes_arm(uint8_t, uint16_t, uint8_t, uint8_t) {}
void event_ring_record_aux(uint16_t, uint8_t, uint32_t) {}
void psx_irq_raise(uint32_t bit, uint32_t) { i_stat |= 1u << bit; }
int memcard_is_present(int) { return 0; }
int memcard_read_sector(int, int, uint8_t* buf) { std::memset(buf, 0, 128); return -1; }
int memcard_write_sector(int, int, const uint8_t*) { return -1; }
void memcard_flush(int) {}
}

/* The guest bus: main RAM, plus the real SIO (sio.c) at 0x1F801040..5F, so
 * a bus read of SIO_DATA / SIO_STAT has its hardware side effects here too.
 * Every MMIO bus access is counted. */
static int mmio_bus_accesses;
static bool is_sio(uint32_t a) {
    a &= 0x1fffffffu;
    return a >= 0x1F801040u && a < 0x1F801060u;
}
extern "C" uint8_t psx_read_byte(uint32_t address) {
    if (is_sio(address)) { ++mmio_bus_accesses; return (uint8_t)sio_read(address); }
    return ram[address & 0x1fffffu];
}

extern "C" void psx_write_byte(uint32_t address, uint8_t value) {
    if (is_sio(address)) { ++mmio_bus_accesses; sio_write(address, value); return; }
    ram[address & 0x1fffffu] = value;
}

/* Side-effect-free peek (memory.c): RAM only in this fixture. */
extern "C" int psx_peek_word(uint32_t addr, uint32_t* out) {
    if (is_sio(addr) || (addr & 0x1fffffffu) >= 0x200000u) return 0;
    const uint32_t o = addr & 0x1ffffcu;
    *out = (uint32_t)ram[o] | ((uint32_t)ram[o + 1] << 8) |
           ((uint32_t)ram[o + 2] << 16) | ((uint32_t)ram[o + 3] << 24);
    return 1;
}

extern "C" uint16_t psx_read_half(uint32_t address) {
    if (is_sio(address)) { ++mmio_bus_accesses; return (uint16_t)sio_read(address); }
    const uint32_t offset = address & 0x1fffffu;
    return (uint16_t)(ram[offset] | ((uint16_t)ram[offset + 1] << 8));
}

extern "C" void psx_write_half(uint32_t address, uint16_t value) {
    if (is_sio(address)) { ++mmio_bus_accesses; sio_write(address, value); return; }
    const uint32_t offset = address & 0x1fffffu;
    ram[offset] = (uint8_t)value;
    ram[offset + 1] = (uint8_t)(value >> 8);
}

extern "C" uint32_t psx_read_word(uint32_t address) {
    if (is_sio(address)) { ++mmio_bus_accesses; return sio_read(address); }
    const uint32_t offset = address & 0x1fffffu;
    return (uint32_t)ram[offset] |
           ((uint32_t)ram[offset + 1] << 8) |
           ((uint32_t)ram[offset + 2] << 16) |
           ((uint32_t)ram[offset + 3] << 24);
}

extern "C" void psx_write_word(uint32_t address, uint32_t value) {
    if (is_sio(address)) { ++mmio_bus_accesses; sio_write(address, value); return; }
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
extern "C" void gpu_ws_tag_feedback_rect(uint32_t) {}

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
static int out_hits, out_activations, out_scope;
static uint32_t out_peeks[5];
extern "C" int psx_mod_host_output_scope(void);
static void out_entry(CPUState* cpu, uint32_t) {
    out_hits++;
    out_scope = psx_mod_host_output_scope();
    cpu->gpr[2] = 0x12345678u;                /* review repro: must not stick */
    /* review 2: MMIO reads through the mod API and the CPU's bus pointers,
     * and stores through cpu->write_* (RAM and SIO_CTRL). */
    out_peeks[0] = psx_mod_read_word(0x1F801040u);   /* SIO_DATA: pops RX */
    out_peeks[1] = psx_mod_read_half(0x1F801044u);   /* SIO_STAT: ages ACK */
    out_peeks[2] = psx_mod_read_byte(0x80004000u);
    if (cpu->read_word) out_peeks[3] = cpu->read_word(0x1F801040u);
    if (cpu->read_half) out_peeks[4] = cpu->read_half(0x1F801044u);
    if (cpu->write_word) cpu->write_word(0x80004004u, 0xDEADBEEFu);
    if (cpu->write_half) cpu->write_half(0x1F80104Au, 0x0040u);  /* SIO reset */
    if (cpu->write_byte) cpu->write_byte(0x80004008u, 0x77u);
    cpu->pc = 0x80001234u;
    psx_mod_write_byte(0x80004000u, 0x5Au);   /* must be refused online */
    psx_mod_finish_function(cpu);             /* must be ignored online */
}
static void out_activate(void) { out_activations++; }

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
    write_text(root / "bundled/out.pkg/1.0.0/manifest.toml",
               package("out.pkg", "rumble", "out.rumble", "netplay = \"host_output\"\n"));
    write_text(root / "bundled/bad.pkg/1.0.0/manifest.toml",
               package("bad.pkg", "x", "bad.x", "netplay = \"everywhere\"\n"));
    const std::string state =
        "format_version = 2\n"
        "[[feature]]\npackage_id = \"view.pkg\"\nid = \"wide\"\nenabled = true\n"
        "[[feature]]\npackage_id = \"sim.pkg\"\nid = \"cheat\"\nenabled = true\n"
        "[[feature]]\npackage_id = \"out.pkg\"\nid = \"rumble\"\nenabled = true\n";
    write_text(root / "state.toml", state);

    check(psx_mod_register_function_entry_plugin("view.wide", 0x80003000u, view_entry),
          "register view hook");
    check(psx_mod_register_function_entry_plugin("sim.cheat", 0x80003100u, sim_entry),
          "register sim hook");
    check(psx_mod_register_activation_plugin("view.wide", view_activate) &&
              psx_mod_register_activation_plugin("sim.cheat", sim_activate),
          "register activations");
    check(psx_mod_register_vblank_plugin("view.wide", view_vblank), "register vblank");
    check(psx_mod_register_function_entry_plugin("out.rumble", 0x80003200u, out_entry) &&
              psx_mod_register_activation_plugin("out.rumble", out_activate),
          "register host-output hook");

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
    check(psx_mod_function_entry(&cpu, 0x80003200u) == 1 && out_hits == 1 &&
              psx_read_byte(0x80004000u) == 0x5Au && !out_scope,
          "offline a host-output hook is an ordinary hook");
    cpu = CPUState{};
    psx_write_byte(0x80004000u, 0);

    /* Netplay: only the local-view feature remains, parked outside the scope. */
    view_hits = sim_hits = view_activations = sim_activations = vblanks = 0;
    const std::string saved = read_text(root / "state.toml");
    check(PSXRecompV4::mod_runtime_commit_netplay_view({}, &error), error.c_str());
    check(PSXRecompV4::mod_runtime_netplay_view_active(), "own-view plan active");
    mod_runtime_activate_plugins();
    check(view_activations == 1 && sim_activations == 0,
          "only the local-view plugin activates in a match");
    check(out_activations == 2, "the host-output plugin activates in a match too");
    out_hits = 0;
    cpu = CPUState{};
    cpu.gpr[31] = 0x80005550u;
    cpu.read_word = psx_read_word;   cpu.write_word = psx_write_word;
    cpu.read_half = psx_read_half;   cpu.write_half = psx_write_half;
    cpu.read_byte = psx_read_byte;   cpu.write_byte = psx_write_byte;
    /* A live pad exchange: select port 1, send 0x01, so SIO holds RX data
     * and a pending ACK that a bus read would consume or age. */
    sio_init();
    sio_set_pad_connected(0, 1);
    sio_write(0x1F80104Au, 0x1003u);   /* TX_EN | SELECT | ACK_IRQ_EN, port 1 */
    sio_write(0x1F801040u, 0x01u);     /* pad address byte */
    sio_tick(2000);                    /* exchange done: RX byte + ACK pending */
    check((sio_peek_stat() & 0x0002u) != 0, "fixture: RX_RDY set (pending RX byte)");
    check(sio_peek_ack_visible_reads() > 0, "fixture: ACK counter > 0");
    const int ack0 = sio_peek_ack_visible_reads();
    const uint16_t stat0 = sio_peek_stat(), ctrl0 = sio_peek_ctrl();
    const uint8_t rx0 = sio_peek_rx_data();
    psx_write_word(0x80004004u, 0x11111111u);
    psx_write_byte(0x80004008u, 0x22u);
    mmio_bus_accesses = 0;
    const CPUState before = cpu;
    check(psx_mod_function_entry(&cpu, 0x80003200u) == 0 && out_hits == 1,
          "a host-output hook runs in the shared simulation, cannot finish it");
    check(out_scope == 1 && !psx_mod_host_output_scope(),
          "host-output scope is set only while its hook runs");
    check(cpu.gpr[2] == 0 && std::memcmp(&cpu, &before, sizeof cpu) == 0,
          "a host-output hook's CPU register changes do not reach the shared CPU");
    check(mmio_bus_accesses == 0,
          "a host-output hook makes no MMIO bus access (reads or cpu->write_*)");
    check((sio_peek_stat() & 0x0002u) != 0,
          "SIO RX_RDY still set: the hook did not pop the RX byte");
    check(sio_peek_ack_visible_reads() == ack0,
          "SIO ACK counter unchanged: the hook's SIO_STAT reads did not age it");
    check(sio_peek_stat() == stat0 && sio_peek_ctrl() == ctrl0 &&
              sio_peek_rx_data() == rx0,
          "SIO stat, ctrl and RX data unchanged by the hook");
    check(psx_read_word(0x80004004u) == 0x11111111u &&
              psx_read_byte(0x80004008u) == 0x22u,
          "cpu->write_* from a host-output hook do not reach guest RAM");
    check(out_peeks[0] == 0 && out_peeks[1] == 0 && out_peeks[3] == 0 &&
              out_peeks[4] == 0,
          "MMIO observation reads return 0 (no device access)");
    check(out_peeks[2] == psx_read_byte(0x80004000u),
          "RAM observation reads still see guest RAM");
    check(psx_read_byte(0x80004000u) == 0,
          "a host-output plugin's guest write is refused online");
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
