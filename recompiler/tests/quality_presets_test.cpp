/* quality_presets_test -- graphics presets (docs/QUALITY_PRESETS.md).
 *
 *   1. [quality.*] tables parse as [video] laid over [video], with validation;
 *      an unknown preset name or a bad value is an error;
 *   2. detection rules: Steam Deck, Apple M1 (8 GB), Intel UHD and software
 *      GL are Low; an M4 or a discrete RTX is Ultra; cores and memory cap;
 *   3. the decision: first launch detects and persists; same hardware keeps
 *      the saved preset; new hardware re-detects a preset but never touches
 *      Custom; PSX_QUALITY overrides one run without persisting;
 *   4. a preset in force masks the player's saved values for its keys;
 *   5. settings.toml round-trips the quality_* keys.
 */
#include "config_loader.h"
#include "quality_presets.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static fs::path write_temp(const std::string& name, const std::string& body) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
    return p;
}

static fs::path write_game_toml(const std::string& name, const std::string& tail) {
    return write_temp(name,
        "[game]\n"
        "name = \"probe\"\n"
        "exe = \"probe.exe\"\n"
        "load_address = \"0x80010000\"\n"
        "entry_pc = \"0x80010000\"\n"
        "text_size = \"0x1000\"\n"
        "[recompiler]\n"
        "seeds = \"seeds.json\"\n"
        "[runtime]\n"
        "[video]\n"
        "supersample = 1.5\n"
        "frame_generation = true\n"
        "render_thread = true\n"
        "dynamic_resolution = true\n"
        "dynamic_resolution_min = \"display\"\n"
        "pgxp_depth_buffer = true\n"
        "pgxp_color_correction = true\n"
        "pgxp_seam = \"fine\"\n"
        + tail);
}

static const char* kPresets =
    "[quality.ultra]\n"
    "[quality.medium]\n"
    "supersample = 1.0\n"
    "dynamic_resolution_min = \"720p\"\n"
    "[quality.low]\n"
    "supersample = 1.0\n"
    "frame_generation = false\n"
    "dynamic_resolution_min = \"native\"\n"
    "pgxp_depth_buffer = false\n"
    "pgxp_color_correction = false\n"
    "pgxp_seam = \"off\"\n";

static void test_parse() {
    fs::path p = write_game_toml("psxrecomp_quality_parse.toml", kPresets);
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.quality_presets.size() == 3, "three presets parsed");
    check(gc.quality_presets[0].name == "low" && gc.quality_presets[2].name == "ultra",
          "presets ordered lowest first");
    const auto& low = gc.quality_presets[0];
    check(low.runtime.video_supersample_milli == 1000, "low: supersample 1.0");
    check(!low.runtime.video_frame_generation, "low: frame_generation off");
    check(low.runtime.video_render_thread, "low inherits [video] render_thread");
    check(!low.runtime.video_pgxp_depth_buffer && !low.runtime.video_pgxp_color_correction &&
          low.runtime.video_pgxp_seam == 0, "low: PGXP extras off");
    check(gc.quality_presets[1].runtime.video_pgxp_depth_buffer &&
          gc.quality_presets[1].runtime.video_pgxp_seam == 1,
          "medium inherits the [video] PGXP extras");
    check(gc.quality_presets[2].runtime.video_supersample_milli == 1500,
          "ultra (empty) is the [video] block");
    check(gc.runtime.video_supersample_milli == 1500, "gc.runtime stays the [video] block");
    check(psxq::offered_mask(gc) == 0xBu, "offered mask low|medium|ultra");
    auto keys = psxq::governed_keys(gc);
    check(keys.size() == 6, "governed keys are the union of preset keys");
    fs::remove(p);

    fs::path bad = write_game_toml("psxrecomp_quality_badname.toml", "[quality.epic]\n");
    bool threw = false;
    try { (void)PSXRecompV4::load_game_config(bad); } catch (const std::exception&) { threw = true; }
    check(threw, "unknown preset name is rejected");
    fs::remove(bad);

    fs::path badv = write_game_toml("psxrecomp_quality_badvalue.toml",
                                    "[quality.low]\ndynamic_resolution_min = \"huge\"\n");
    threw = false;
    try { (void)PSXRecompV4::load_game_config(badv); } catch (const std::exception&) { threw = true; }
    check(threw, "a preset value gets [video] validation");
    fs::remove(badv);

    fs::path none = write_game_toml("psxrecomp_quality_none.toml", "");
    auto gn = PSXRecompV4::load_game_config(none);
    check(gn.quality_presets.empty() && psxq::offered_mask(gn) == 0,
          "no [quality] tables: no presets");
    fs::remove(none);
}

static PsxHostInfo host(const char* cpu, const char* gpu, int cores, unsigned gb, int deck = 0) {
    PsxHostInfo h;
    std::memset(&h, 0, sizeof h);
    std::snprintf(h.cpu, sizeof h.cpu, "%s", cpu);
    std::snprintf(h.gpu, sizeof h.gpu, "%s", gpu);
    h.logical_cores = cores;
    h.ram_mb = (uint64_t)gb * 1024u;
    h.steam_deck = deck;
    return h;
}

static int tier(const PsxHostInfo& h) { return psx_quality_classify(&h, nullptr, 0); }

static void test_classify() {
    check(tier(host("Apple M1", "Apple M1", 8, 8)) == PSX_QUALITY_LOW, "M1: Low");
    check(tier(host("Apple M4", "Apple M4", 10, 16)) == PSX_QUALITY_ULTRA, "M4: Ultra");
    check(tier(host("Apple M1 Max", "Apple M1 Max", 10, 32)) == PSX_QUALITY_ULTRA, "M1 Max: Ultra");
    check(tier(host("Apple M2", "Apple M2", 8, 16)) == PSX_QUALITY_LOW, "M2 base: Low");
    check(tier(host("Apple M3", "Apple M3", 8, 16)) == PSX_QUALITY_ULTRA, "M3: Ultra");
    check(tier(host("AMD Custom APU 0405", "", 8, 16)) == PSX_QUALITY_LOW, "Deck by CPU: Low");
    check(tier(host("AMD Custom APU 0932",
                    "AMD Custom GPU 0932 (radeonsi, vangogh, LLVM 15)", 8, 16)) == PSX_QUALITY_LOW,
          "Deck OLED: Low");
    check(tier(host("x", "", 8, 16, 1)) == PSX_QUALITY_LOW, "Deck by DMI: Low");
    check(tier(host("Intel(R) Core(TM) i5-8250U", "Intel(R) UHD Graphics 620", 8, 16)) ==
              PSX_QUALITY_LOW, "Intel UHD: Low");
    check(tier(host("i7", "Intel(R) Iris(R) Xe Graphics", 8, 16)) == PSX_QUALITY_LOW,
          "Iris Xe: Low");
    check(tier(host("AMD Ryzen 9 7950X", "NVIDIA GeForce RTX 4080 SUPER/PCIe/SSE2", 32, 64)) ==
              PSX_QUALITY_ULTRA, "RTX 4080: Ultra");
    check(tier(host("x", "llvmpipe (LLVM 17.0.6, 256 bits)", 16, 32)) == PSX_QUALITY_LOW,
          "software GL: Low");
    check(tier(host("x", "Some Future GPU", 16, 32)) == PSX_QUALITY_ULTRA, "unknown GPU: Ultra");
    check(tier(host("AMD Ryzen 7 9800X3D 8-Core Processor", "AMD Radeon(TM) Graphics", 16, 48)) ==
              PSX_QUALITY_LOW, "2-CU Radeon iGPU: Low");
    check(tier(host("x", "NVIDIA GeForce RTX 3060", 4, 16)) == PSX_QUALITY_LOW,
          "4 threads: Low");
    check(tier(host("x", "NVIDIA GeForce RTX 3060", 2, 16)) == PSX_QUALITY_LOW,
          "2 threads cap at Low");
    check(tier(host("x", "NVIDIA GeForce RTX 3060", 12, 8)) == PSX_QUALITY_ULTRA,
          "8 GB does not cap");
    check(tier(host("x", "NVIDIA GeForce RTX 3060", 12, 4)) == PSX_QUALITY_LOW,
          "4 GB caps at Low");
    check(psx_quality_pick_offered(PSX_QUALITY_HIGH, 0xBu) == PSX_QUALITY_MEDIUM,
          "High not offered: next lower");
    check(psx_quality_pick_offered(PSX_QUALITY_LOW, 0xCu) == PSX_QUALITY_HIGH,
          "nothing lower: next higher");
    check(psx_quality_from_name("Ultra") == PSX_QUALITY_ULTRA, "names are case-insensitive");
    check(psx_quality_from_name("custom") == PSX_QUALITY_CUSTOM, "custom name");
}

static int probe_calls = 0;
static const char* fake_gl = nullptr;      /* nullptr: the GL probe fails */
static int fake_probe(char* out, size_t cap) {
    ++probe_calls;
    if (!fake_gl) return 0;
    std::snprintf(out, cap, "%s", fake_gl);
    return 1;
}

static void test_decide() {
    const unsigned all = 0xFu;
    PSXRecompV4::UserSettings fresh;
    PsxHostInfo m1 = host("Intel(R) Core(TM) i5-8250U", "Intel(R) UHD Graphics 620", 8, 8);
    psxq::Decision d = psxq::decide(all, fresh, m1, nullptr, false, fake_probe);
    check(d.detected_now && d.persist, "first launch detects and persists");
    check(d.preset == PSX_QUALITY_LOW && d.base == PSX_QUALITY_LOW, "UHD 620 first launch: Low");
    check(probe_calls == 1, "a due detection asks GL");
    {
        PsxHostInfo igpu = host("AMD Ryzen 7 9800X3D 8-Core Processor", "NVIDIA GeForce RTX 4080 SUPER", 16, 48);
        fake_gl = "AMD Radeon(TM) Graphics";
        psxq::Decision g = psxq::decide(all, fresh, igpu, nullptr, false, fake_probe);
        check(g.preset == PSX_QUALITY_LOW,
              "GL's renderer wins over the OS adapter name (display on the iGPU)");
        fake_gl = nullptr;
        psxq::Decision o = psxq::decide(all, fresh, igpu, nullptr, false, fake_probe);
        check(o.preset == PSX_QUALITY_ULTRA, "probe failure falls back to the OS name");
    }
    probe_calls = 0;

    PSXRecompV4::UserSettings us;
    psxq::record(us, d);
    check(us.quality_preset == "low" && us.quality_hardware == d.fingerprint,
          "record stores preset and fingerprint");

    psxq::Decision again = psxq::decide(all, us, m1, nullptr, false, fake_probe);
    check(!again.detected_now && !again.persist && again.preset == PSX_QUALITY_LOW,
          "same hardware: saved preset, no detection");

    PSXRecompV4::UserSettings chosen = us;
    chosen.quality_preset = "high";
    psxq::Decision kept = psxq::decide(all, chosen, m1, nullptr, false, fake_probe);
    check(kept.preset == PSX_QUALITY_HIGH, "a player's preset pick is kept on the same hardware");

    PsxHostInfo m4 = host("Apple M4", "Apple M4", 10, 16);
    psxq::Decision moved = psxq::decide(all, us, m4, nullptr, false, fake_probe);
    check(moved.detected_now && moved.preset == PSX_QUALITY_ULTRA,
          "new hardware re-detects a preset");

    PSXRecompV4::UserSettings custom = us;
    custom.quality_preset = "custom";
    custom.quality_base = "low";
    psxq::Decision c = psxq::decide(all, custom, m4, nullptr, false, fake_probe);
    check(c.preset == PSX_QUALITY_CUSTOM && c.base == PSX_QUALITY_LOW && !c.persist,
          "Custom is never overridden, even on new hardware");
    check(c.hardware_changed_custom, "new hardware under Custom is reported");

    psxq::Decision re = psxq::decide(all, custom, m4, nullptr, true, fake_probe);
    check(re.detected_now && re.preset == PSX_QUALITY_ULTRA, "Re-detect replaces Custom");

    psxq::Decision env = psxq::decide(all, us, m1, "medium", false, fake_probe);
    check(env.preset == PSX_QUALITY_MEDIUM && env.env_override && !env.persist,
          "PSX_QUALITY overrides one run, not saved");

    fake_gl = "Intel(R) UHD Graphics 620";
    PsxHostInfo linux_box = host("Intel(R) Core(TM) i5-8250U", "", 8, 16);
    std::snprintf(linux_box.gpu_id, sizeof linux_box.gpu_id, "0x8086:0x5917");
    psxq::Decision gl = psxq::decide(all, fresh, linux_box, nullptr, false, fake_probe);
    check(gl.preset == PSX_QUALITY_LOW,
          "unnamed GPU: GL probe names it (UHD 620 -> Low)");
    PSXRecompV4::UserSettings lus;
    psxq::record(lus, gl);
    psxq::Decision gl2 = psxq::decide(all, lus, linux_box, nullptr, false, fake_probe);
    const int calls = probe_calls;
    psxq::Decision gl3 = psxq::decide(all, lus, linux_box, nullptr, false, fake_probe);
    check(!gl2.detected_now && !gl3.detected_now && probe_calls == calls,
          "fingerprint is OS-only: the probe does not make the next launch re-detect");

    psxq::Decision none = psxq::decide(0u, fresh, m1, nullptr, false, fake_probe);
    check(none.preset == PSX_QUALITY_NONE && !none.persist, "no presets: no decision");
}

static void test_mask() {
    PSXRecompV4::UserSettings us;
    us.has_frame_generation = true; us.frame_generation = true;
    us.has_dynamic_resolution_min = true;
    us.has_internal_resolution = true;
    us.has_render_thread = true;
    psxq::mask_user_settings(us, {"frame_generation", "dynamic_resolution_min", "supersample"});
    check(!us.has_frame_generation && !us.has_dynamic_resolution_min,
          "a preset owns its keys");
    check(us.has_render_thread && us.has_internal_resolution,
          "keys the presets do not set stay the player's");
}

static void test_settings_round_trip() {
    PSXRecompV4::UserSettings s;
    s.has_quality_preset = true;   s.quality_preset = "custom";
    s.has_quality_base = true;     s.quality_base = "medium";
    s.has_quality_hardware = true; s.quality_hardware = "0123456789abcdef";
    s.has_quality_detected = true; s.quality_detected = "low";
    fs::path p = fs::temp_directory_path() / "psxrecomp_quality_settings.toml";
    check(PSXRecompV4::save_user_settings(p, s), "save settings");
    auto r = PSXRecompV4::load_user_settings(p);
    check(r.has_quality_preset && r.quality_preset == "custom", "quality_preset round-trips");
    check(r.has_quality_base && r.quality_base == "medium", "quality_base round-trips");
    check(r.has_quality_hardware && r.quality_hardware == "0123456789abcdef",
          "quality_hardware round-trips");
    check(r.has_quality_detected && r.quality_detected == "low", "quality_detected round-trips");
    fs::remove(p);
}

int main() {
    test_parse();
    test_classify();
    test_decide();
    test_mask();
    test_settings_round_trip();
    if (failures) {
        std::fprintf(stderr, "quality_presets_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("quality_presets_test: ok\n");
    return 0;
}
