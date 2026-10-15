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
#include "host_overlay.h"

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
    check(tier(host("x", "Some Future GPU", 16, 32)) == PSX_QUALITY_MEDIUM, "unknown GPU: Medium (conservative)");
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

/* Real GL_RENDERER strings: Windows drivers, Mesa (Linux / SteamOS) and
 * macOS. 16 threads and 32 GB, so only the GPU class decides. */
static void test_renderer_table() {
    struct Row { const char* renderer; int want; };
    static const Row rows[] = {
        /* Windows (AMD Adrenalin, NVIDIA, Intel) */
        {"AMD Radeon(TM) Graphics", PSX_QUALITY_LOW},
        {"AMD Radeon RX Vega 11 Graphics", PSX_QUALITY_LOW},
        {"AMD Radeon(TM) Vega 8 Graphics", PSX_QUALITY_LOW},
        {"AMD Radeon(TM) R7 Graphics", PSX_QUALITY_LOW},
        {"AMD Radeon 780M Graphics", PSX_QUALITY_LOW},
        {"AMD Radeon(TM) 890M Graphics", PSX_QUALITY_LOW},
        {"AMD Radeon RX 6800 XT", PSX_QUALITY_ULTRA},
        {"AMD Radeon RX 7600M XT", PSX_QUALITY_ULTRA},
        {"AMD Radeon(TM) RX 580 Series", PSX_QUALITY_ULTRA},
        {"Radeon RX Vega 64", PSX_QUALITY_ULTRA},
        {"AMD Radeon Pro W6800", PSX_QUALITY_ULTRA},
        {"AMD Radeon(TM) 8060S Graphics", PSX_QUALITY_ULTRA},
        {"NVIDIA GeForce RTX 4080 SUPER/PCIe/SSE2", PSX_QUALITY_ULTRA},
        {"NVIDIA GeForce GTX 1060 6GB/PCIe/SSE2", PSX_QUALITY_ULTRA},
        {"NVIDIA GeForce MX450/PCIe/SSE2", PSX_QUALITY_LOW},
        {"NVIDIA GeForce GT 1030/PCIe/SSE2", PSX_QUALITY_LOW},
        {"Intel(R) UHD Graphics 620", PSX_QUALITY_LOW},
        {"Intel(R) Iris(R) Xe Graphics", PSX_QUALITY_LOW},
        {"Intel(R) Arc(TM) Graphics", PSX_QUALITY_LOW},
        {"Intel(R) Arc(TM) A770 Graphics", PSX_QUALITY_ULTRA},
        {"GDI Generic", PSX_QUALITY_LOW},
        {"Microsoft Basic Render Driver", PSX_QUALITY_LOW},
        /* Mesa */
        {"AMD Radeon Graphics (radeonsi, renoir, LLVM 15.0.7, DRM 3.54, 6.5.0)", PSX_QUALITY_LOW},
        {"AMD Radeon Vega 8 Graphics (radeonsi, raven, LLVM 15.0.7, DRM 3.42)", PSX_QUALITY_LOW},
        {"AMD Radeon 780M (radeonsi, gfx1103_r1, LLVM 17.0.6, DRM 3.57)", PSX_QUALITY_LOW},
        {"AMD Custom GPU 0405 (radeonsi, vangogh, LLVM 15.0.7, DRM 3.49, 6.1.52-valve16-1-neptune-61)",
         PSX_QUALITY_LOW},
        {"AMD Custom GPU 0932 (radeonsi, vangogh, ACO, DRM 3.57)", PSX_QUALITY_LOW},
        {"AMD Radeon RX 6700 XT (radeonsi, navi22, LLVM 17.0.6, DRM 3.57)", PSX_QUALITY_ULTRA},
        {"Mesa Intel(R) UHD Graphics 620 (KBL GT2)", PSX_QUALITY_LOW},
        {"Mesa Intel(R) Xe Graphics (TGL GT2)", PSX_QUALITY_LOW},
        {"Mesa Intel(R) Arc(tm) Graphics (MTL)", PSX_QUALITY_LOW},
        {"Mesa Intel(R) Arc(tm) A750 Graphics (DG2)", PSX_QUALITY_ULTRA},
        {"NVIDIA GeForce RTX 3060/PCIe/SSE2", PSX_QUALITY_ULTRA},
        {"NV167 (nouveau)", PSX_QUALITY_MEDIUM},   /* unnamed chip: unknown */
        {"Virtio-GPU Venus (Some Future GPU)", PSX_QUALITY_MEDIUM},
        {"llvmpipe (LLVM 17.0.6, 256 bits)", PSX_QUALITY_LOW},
        {"zink Vulkan 1.3(AMD Radeon Graphics (RADV RENOIR) (MESA_RADV))", PSX_QUALITY_LOW},
        /* macOS */
        {"Apple M1", PSX_QUALITY_LOW},
        {"Apple M2", PSX_QUALITY_LOW},
        {"Apple M1 Pro", PSX_QUALITY_ULTRA},
        {"Apple M2 Max", PSX_QUALITY_ULTRA},
        {"Apple M3", PSX_QUALITY_ULTRA},
        {"Apple M4", PSX_QUALITY_ULTRA},
        {"AMD Radeon Pro 5500M OpenGL Engine", PSX_QUALITY_ULTRA},
        {"Intel(R) Iris(TM) Plus Graphics OpenGL Engine", PSX_QUALITY_LOW},
    };
    for (const Row& r : rows) {
        PsxHostInfo h = host("x", r.renderer, 16, 32);
        const int got = psx_quality_classify(&h, nullptr, 0);
        if (got != r.want) {
            std::fprintf(stderr, "  renderer \"%s\": got %s, want %s\n", r.renderer,
                         psx_quality_name(got), psx_quality_name(r.want));
            check(false, "renderer table");
        }
    }
    /* Linux without a GL probe: the PCI id. */
    PsxHostInfo deck = host("x", "", 8, 16);
    std::snprintf(deck.gpu_id, sizeof deck.gpu_id, "0x1002:0x163f");
    check(psx_quality_classify(&deck, nullptr, 0) == PSX_QUALITY_LOW, "PCI id: Steam Deck -> Low");
    PsxHostInfo uhd = host("x", "", 8, 16);
    std::snprintf(uhd.gpu_id, sizeof uhd.gpu_id, "0x8086:0x5917");
    check(psx_quality_classify(&uhd, nullptr, 0) == PSX_QUALITY_LOW, "PCI id: Intel iGPU -> Low");
    PsxHostInfo arc = host("x", "", 8, 16);
    std::snprintf(arc.gpu_id, sizeof arc.gpu_id, "0x8086:0x56a0");
    check(psx_quality_classify(&arc, nullptr, 0) == PSX_QUALITY_ULTRA, "PCI id: Intel Arc -> Ultra");
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
    {
        /* The GL probe fails: classified from the OS name for this run, not
         * saved, so the next launch probes again. */
        psxq::Decision np = psxq::decide(all, fresh, m1, nullptr, false, fake_probe);
        check(np.detected_now && !np.persist && !np.gl_probed && np.preset == PSX_QUALITY_LOW,
              "probe failure: OS-name result used, not persisted");
        probe_calls = 0;
        /* Headless / hidden window: no detection, nothing saved. */
        psxq::Decision hd = psxq::decide(all, fresh, m1, nullptr, false, fake_probe, true);
        check(hd.detection_skipped && !hd.persist && hd.preset == PSX_QUALITY_NONE &&
              probe_calls == 0, "headless: no detection, no probe, nothing saved");
        PSXRecompV4::UserSettings saved_low;
        saved_low.has_quality_preset = true; saved_low.quality_preset = "low";
        psxq::Decision hs = psxq::decide(all, saved_low, m1, nullptr, false, fake_probe, true);
        check(hs.preset == PSX_QUALITY_LOW && !hs.persist,
              "headless with a saved preset: the saved preset applies");
    }
    fake_gl = "Intel(R) UHD Graphics 620";
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

/* The preset/key-ownership transaction (psxq::settle_save), on the two cases
 * from the #591 review, through a real settings.toml round trip. */
static PSXRecompV4::UserSettings save_load(const PSXRecompV4::UserSettings& s) {
    fs::path p = fs::temp_directory_path() / "psxrecomp_quality_txn.toml";
    check(PSXRecompV4::save_user_settings(p, s), "txn: save");
    PSXRecompV4::UserSettings r = PSXRecompV4::load_user_settings(p);
    fs::remove(p);
    return r;
}

/* What the next launch does with the file: decide, then mask while a preset
 * (not Custom) is in force (main.cpp quality_preset_in_force). */
static PSXRecompV4::UserSettings next_launch(const PSXRecompV4::UserSettings& disk,
                                             const PsxHostInfo& h,
                                             const std::vector<std::string>& keys,
                                             psxq::Decision* out) {
    psxq::Decision d = psxq::decide(0xFu, disk, h, nullptr, false, nullptr);
    PSXRecompV4::UserSettings us = disk;
    if (d.preset >= 0 && d.preset < PSX_QUALITY_COUNT) psxq::mask_user_settings(us, keys);
    if (out) *out = d;
    return us;
}

static void test_ownership_txn() {
    const std::vector<std::string> keys = {"texture_filtering", "frame_generation",
                                           "dynamic_resolution_min"};
    PsxHostInfo m4 = host("Apple M4", "Apple M4", 10, 16);
    char fp[17];
    psx_quality_fingerprint(&m4, fp);

    /* Saved Ultra on this machine. */
    PSXRecompV4::UserSettings ultra;
    ultra.has_quality_preset = true;   ultra.quality_preset = "ultra";
    ultra.has_quality_base = true;     ultra.quality_base = "ultra";
    ultra.has_quality_hardware = true; ultra.quality_hardware = fp;
    ultra.has_quality_detected = true; ultra.quality_detected = "ultra";
    ultra.has_scanlines = true;        ultra.scanlines = false;

    /* Review case 1: PSX_QUALITY=low for one run, Settings opened and quit
     * with nothing changed. The boot launcher's seed is built empty and
     * filled with this run's (Low) values. */
    {
        psxq::Decision run = psxq::decide(0xFu, ultra, m4, "low", false, nullptr);
        check(run.env_override && run.preset == PSX_QUALITY_LOW, "txn1: override in force");
        PSXRecompV4::UserSettings seed;                    /* empty boot seed */
        seed.has_texture_filter = true;  seed.texture_filter = 1;     /* Low: bilinear */
        seed.has_frame_generation = true; seed.frame_generation = false;
        seed.has_scanlines = true;       seed.scanlines = false;
        psxq::settle_save(seed, ultra, run, psxq::Edit::NONE, PSX_QUALITY_NONE, keys);
        PSXRecompV4::UserSettings disk = save_load(seed);
        check(disk.quality_preset == "ultra" && disk.quality_base == "ultra",
              "txn1: a one-run override never persists the preset");
        check(!disk.has_texture_filter && !disk.has_frame_generation,
              "txn1: nor the override's governed values (empty seed)");
        psxq::Decision nd;
        next_launch(disk, m4, keys, &nd);
        check(nd.preset == PSX_QUALITY_ULTRA, "txn1: next launch is Ultra again");

        /* Even a governed edit under the override applies this run only. */
        PSXRecompV4::UserSettings edited = seed;
        edited.texture_filter = 0;
        psxq::Decision run2 = psxq::decide(0xFu, ultra, m4, "low", false, nullptr);
        psxq::settle_save(edited, ultra, run2, psxq::Edit::GOVERNED_EDIT, PSX_QUALITY_NONE, keys);
        check(edited.quality_preset == "ultra" && !edited.has_texture_filter,
              "txn1: a governed edit under the override is not saved");
    }
    /* The override keeps a previous Custom profile and its values. */
    {
        PSXRecompV4::UserSettings custom = ultra;
        custom.quality_preset = "custom";
        custom.has_texture_filter = true; custom.texture_filter = 1;
        psxq::Decision run = psxq::decide(0xFu, custom, m4, "low", false, nullptr);
        PSXRecompV4::UserSettings seed;
        seed.has_texture_filter = true; seed.texture_filter = 0;
        psxq::settle_save(seed, custom, run, psxq::Edit::NONE, PSX_QUALITY_NONE, keys);
        PSXRecompV4::UserSettings disk = save_load(seed);
        check(disk.quality_preset == "custom" && disk.quality_base == "ultra" &&
              disk.has_texture_filter && disk.texture_filter == 1,
              "txn1: Custom and its values survive a one-run override");
    }

    /* Review case 2: a live in-game change to a governed value (texture
     * filter) while Ultra is in force becomes Custom and survives restart. */
    {
        psxq::Decision run = psxq::decide(0xFu, ultra, m4, nullptr, false, nullptr);
        PsxHostVideoSettings cur{}, next{};
        cur.texture_filter = 0; next = cur; next.texture_filter = 1;
        const psxq::Edit e = psxq::live_edit(cur, next, keys);
        check(e == psxq::Edit::GOVERNED_EDIT, "txn2: texture filter is a governed live edit");
        PSXRecompV4::UserSettings us = ultra;              /* the live path loads disk */
        us.has_texture_filter = true; us.texture_filter = 1;
        check(psxq::settle_save(us, ultra, run, e, PSX_QUALITY_NONE, keys),
              "txn2: the state changes");
        PSXRecompV4::UserSettings disk = save_load(us);
        check(disk.quality_preset == "custom" && disk.quality_base == "ultra",
              "txn2: a governed live edit becomes Custom from Ultra");
        psxq::Decision nd;
        PSXRecompV4::UserSettings eff = next_launch(disk, m4, keys, &nd);
        check(nd.preset == PSX_QUALITY_CUSTOM && eff.has_texture_filter &&
              eff.texture_filter == 1,
              "txn2: the next launch keeps the manual texture filter");
    }
    /* An unrelated live edit (scanlines) keeps the preset. */
    {
        psxq::Decision run = psxq::decide(0xFu, ultra, m4, nullptr, false, nullptr);
        PsxHostVideoSettings cur{}, next{};
        next = cur; next.scanlines = 1;
        const psxq::Edit e = psxq::live_edit(cur, next, keys);
        check(e == psxq::Edit::NONE, "txn3: scanlines are not governed");
        PSXRecompV4::UserSettings us = ultra;
        us.scanlines = true;
        us.has_texture_filter = true; us.texture_filter = 0;   /* the live path writes every row */
        psxq::settle_save(us, ultra, run, e, PSX_QUALITY_NONE, keys);
        check(us.quality_preset == "ultra" && us.scanlines && !us.has_texture_filter,
              "txn3: unrelated edits keep the preset; its keys stay the preset's");
    }
    /* A launcher pick is recorded; the picked preset owns its keys. */
    {
        psxq::Decision run = psxq::decide(0xFu, ultra, m4, nullptr, false, nullptr);
        PSXRecompV4::UserSettings seed;
        seed.has_texture_filter = true; seed.texture_filter = 1;
        psxq::settle_save(seed, ultra, run, psxq::Edit::PICK, PSX_QUALITY_MEDIUM, keys);
        check(seed.quality_preset == "medium" && seed.quality_base == "medium" &&
              seed.quality_hardware == fp && !seed.has_texture_filter,
              "txn4: a picked preset is saved with the hardware it was picked on");
    }
}

/* The supported quality-key set (docs/QUALITY_PRESETS.md): anything else in
 * a preset is a load error, and every player-editable key is masked and
 * copied (ownership enforced). */
static void test_supported_keys() {
    bool threw = false;
    fs::path bad = write_game_toml("psxrecomp_quality_vsync.toml",
                                   "[quality.ultra]\nvsync = \"on\"\n");
    try { (void)PSXRecompV4::load_game_config(bad); } catch (const std::exception&) { threw = true; }
    check(threw, "vsync in a preset is rejected (not a supported quality key)");
    fs::remove(bad);
    threw = false;
    bad = write_game_toml("psxrecomp_quality_renderer.toml", "[quality.low]\nrenderer = \"opengl\"\n");
    try { (void)PSXRecompV4::load_game_config(bad); } catch (const std::exception&) { threw = true; }
    check(threw, "renderer in a preset is rejected");
    fs::remove(bad);

    /* R4's preset keys (#23) are all supported. */
    for (const char* k : {"supersample", "dynamic_resolution_min", "frame_generation",
                          "pgxp_depth_buffer", "pgxp_color_correction", "pgxp_seam",
                          "texture_lod", "anisotropic_filtering", "fmv_chroma_smoothing",
                          "bloom", "dithering"})
        check(PSXRecompV4::quality_key_supported(k), k);

    /* Every player-editable key: masked while a preset is in force, and a
     * one-run save restores its disk value. */
    const std::vector<std::string> player = {
        "texture_filtering", "fmv_filter", "antialiasing", "dynamic_resolution",
        "dynamic_resolution_min", "frame_generation", "render_thread", "present_thread",
        "geometry_correction", "perspective_texturing", "frame_interpolation",
        "frame_interpolation_fps"};
    PSXRecompV4::UserSettings on;
    on.has_texture_filter = on.has_fmv_filter = on.has_antialiasing = true;
    on.has_dynamic_resolution = on.has_dynamic_resolution_min = on.has_frame_generation = true;
    on.has_render_thread = on.has_present_thread = on.has_geometry_correction = true;
    on.has_perspective_texturing = on.has_frame_interpolation = true;
    on.has_frame_interpolation_fps = true;
    for (const std::string& k : player) {
        check(PSXRecompV4::quality_key_supported(k) && PSXRecompV4::quality_key_player_editable(k),
              ("player-editable: " + k).c_str());
        PSXRecompV4::UserSettings m = on;
        psxq::mask_user_settings(m, {k});
        PSXRecompV4::UserSettings c = on;
        psxq::copy_key(c, PSXRecompV4::UserSettings{}, k);
        const bool masked =
            (k == "texture_filtering" && !m.has_texture_filter && !c.has_texture_filter) ||
            (k == "fmv_filter" && !m.has_fmv_filter && !c.has_fmv_filter) ||
            (k == "antialiasing" && !m.has_antialiasing && !c.has_antialiasing) ||
            (k == "dynamic_resolution" && !m.has_dynamic_resolution && !c.has_dynamic_resolution) ||
            (k == "dynamic_resolution_min" && !m.has_dynamic_resolution_min && !c.has_dynamic_resolution_min) ||
            (k == "frame_generation" && !m.has_frame_generation && !c.has_frame_generation) ||
            (k == "render_thread" && !m.has_render_thread && !c.has_render_thread) ||
            (k == "present_thread" && !m.has_present_thread && !c.has_present_thread) ||
            (k == "geometry_correction" && !m.has_geometry_correction && !c.has_geometry_correction) ||
            (k == "perspective_texturing" && !m.has_perspective_texturing && !c.has_perspective_texturing) ||
            (k == "frame_interpolation" && !m.has_frame_interpolation && !c.has_frame_interpolation) ||
            (k == "frame_interpolation_fps" && !m.has_frame_interpolation_fps && !c.has_frame_interpolation_fps);
        check(masked, ("masked and copied: " + k).c_str());
    }
}

/* Host-side launcher classification: the frontend flags Custom on any
 * tracked row; only rows of keys the title governs count. */
static void test_launcher_classification() {
    const std::vector<std::string> keys = {"texture_filtering", "supersample",
                                           "dynamic_resolution_min", "frame_generation"};
    auto only = [](const char* changed) {
        return [changed](const std::string& k) { return k == changed; };
    };
    int picked = PSX_QUALITY_NONE;
    check(psxq::launcher_custom_edit(keys, PSX_QUALITY_ULTRA, PSX_QUALITY_ULTRA,
                                     only("antialiasing"), &picked) == psxq::Edit::NONE,
          "Antialiasing (not governed) leaves Ultra in force");
    check(psxq::launcher_custom_edit(keys, PSX_QUALITY_ULTRA, PSX_QUALITY_ULTRA,
                                     only("texture_filtering"), &picked) ==
              psxq::Edit::GOVERNED_EDIT,
          "a governed row (texture filter) makes Custom");
    picked = PSX_QUALITY_NONE;
    check(psxq::launcher_custom_edit(keys, PSX_QUALITY_LOW, PSX_QUALITY_ULTRA,
                                     only("antialiasing"), &picked) == psxq::Edit::PICK &&
              picked == PSX_QUALITY_LOW,
          "pick Low then change Antialiasing: a Low pick, not Custom");
    check(psxq::launcher_custom_edit(keys, PSX_QUALITY_ULTRA, PSX_QUALITY_CUSTOM,
                                     only("antialiasing"), &picked) ==
              psxq::Edit::GOVERNED_EDIT,
          "already Custom stays Custom");
}

int main() {
    test_parse();
    test_classify();
    test_renderer_table();
    test_decide();
    test_mask();
    test_settings_round_trip();
    test_ownership_txn();
    test_supported_keys();
    test_launcher_classification();
    if (failures) {
        std::fprintf(stderr, "quality_presets_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("quality_presets_test: ok\n");
    return 0;
}
