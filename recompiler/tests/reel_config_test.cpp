/* reel_config_test - [controller] reel and [widescreen] adaptive_view.
 *
 * The right-stick reel is a per-title gesture: titles that use it select
 * "ab" (pulse Cross/Circle), "cross" or "circle" (hold one button) or "off".
 * This pins the parser surface the runtime reads, including the default that
 * keeps a title without the keys on the historical A/B gesture.
 *
 * [widescreen] adaptive_view is the title's opt-in for the resize-driven
 * aspect. It defaults off so a game.toml that says nothing keeps the 4:3
 * presentation clamp.
 */
#include "config_loader.h"

#include <cstdio>
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

static fs::path write_game_toml(const std::string& name,
                                const std::string& body) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << "[game]\n"
         "name = \"probe\"\n"
         "exe = \"probe.exe\"\n"
         "load_address = \"0x80010000\"\n"
         "entry_pc = \"0x80010000\"\n"
         "text_size = \"0x1000\"\n"
         "[recompiler]\n"
         "seeds = \"seeds.json\"\n"
         "[runtime]\n"
      << body;
    return p;
}

static bool loads(const std::string& name, const std::string& body) {
    try {
        (void)PSXRecompV4::load_game_config(write_game_toml(name, body));
        return true;
    } catch (...) {
        return false;
    }
}

static void test_defaults() {
    auto gc = PSXRecompV4::load_game_config(
        write_game_toml("psxrecomp_reel_default.toml", ""));
    check(!gc.runtime.has_reel, "reel key absent by default");
    check(gc.runtime.reel == "ab", "reel defaults to ab");
    check(gc.runtime.reel_hold_ms == 300, "reel_hold_ms defaults to 300");
    check(gc.runtime.reel_pulse_ms == 50, "reel_pulse_ms defaults to 50");
    check(!gc.ws_adaptive_view, "adaptive_view defaults off");
}

static void test_reel_modes() {
    for (const char* mode : {"ab", "cross", "circle", "off"}) {
        auto gc = PSXRecompV4::load_game_config(write_game_toml(
            std::string("psxrecomp_reel_") + mode + ".toml",
            std::string("[controller]\nreel = \"") + mode + "\"\n"));
        check(gc.runtime.has_reel, "reel key recorded");
        check(gc.runtime.reel == mode, "reel mode preserved");
    }
}

static void test_reel_timings() {
    auto gc = PSXRecompV4::load_game_config(write_game_toml(
        "psxrecomp_reel_timings.toml",
        "[controller]\n"
        "reel = \"cross\"\n"
        "reel_hold_ms = 500\n"
        "reel_pulse_ms = 25\n"));
    check(gc.runtime.reel_hold_ms == 500, "reel_hold_ms parsed");
    check(gc.runtime.reel_pulse_ms == 25, "reel_pulse_ms parsed");
}

static void test_adaptive_view() {
    auto gc = PSXRecompV4::load_game_config(write_game_toml(
        "psxrecomp_adaptive_view.toml",
        "[widescreen]\nadaptive_view = true\n"));
    check(gc.ws_adaptive_view, "adaptive_view parsed");
}

static void test_rejects() {
    check(!loads("psxrecomp_reel_bad_mode.toml",
                 "[controller]\nreel = \"bogus\"\n"),
          "unknown reel mode is rejected");
    check(!loads("psxrecomp_reel_bad_hold.toml",
                 "[controller]\nreel_hold_ms = 10\n"),
          "reel_hold_ms below range is rejected");
    check(!loads("psxrecomp_reel_bad_pulse.toml",
                 "[controller]\nreel_pulse_ms = 5000\n"),
          "reel_pulse_ms above range is rejected");
}

int main() {
    test_defaults();
    test_reel_modes();
    test_reel_timings();
    test_adaptive_view();
    test_rejects();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("reel_config_test: PASS\n");
    return 0;
}
