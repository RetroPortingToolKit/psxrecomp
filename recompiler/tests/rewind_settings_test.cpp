/* rewind_settings_test -- local rewind is a player setting.
 *
 * Local rewind keeps whole-machine snapshots (2 MB main RAM + 1 MB VRAM +
 * 512 KB SPU RAM, uncompressed) in a ring and captures one every
 * rewind_interval frames, so it is opt-in. The switch, the buffer depth and
 * the interval are settings.toml [video] player settings (player_settings.h,
 * the launcher's General page), next to the controller hotkey that opens it.
 *
 * What this checks:
 *
 *   1. rewind / rewind_depth / rewind_interval load and survive a save;
 *   2. a depth or interval outside the offered choices is ignored, not an
 *      error, and a save drops it;
 *   3. the [hotkeys] rewind_pad binding still reads and round-trips.
 */
#include "config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

static std::string read_text(const fs::path& p) {
    std::ifstream f(p);   // closed on return: Windows cannot delete an open file
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void test_rewind_settings_round_trip() {
    fs::path p = write_temp("psxrecomp_rewind_settings.toml",
        "[video]\n"
        "window_width = 1600\n"
        "rewind = true\n"
        "rewind_depth = 150\n"
        "rewind_interval = 8\n"
        "[hotkeys]\n"
        "rewind_pad = 1272\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "a settings.toml with the rewind keys parses");
    check(us.has_window_width && us.window_width == 1600,
          "the player's other settings survive");
    check(us.extra_player_settings["video.rewind"] == "true" &&
          us.extra_player_settings["video.rewind_depth"] == "150" &&
          us.extra_player_settings["video.rewind_interval"] == "8",
          "rewind, its buffer and its interval are player settings");
    check(us.has_hotkey_pad_rewind && us.hotkey_pad_rewind == 1272,
          "the Rewind controller hotkey is still a setting");
    fs::remove(p);

    fs::path out = fs::temp_directory_path() / "psxrecomp_rewind_settings_saved.toml";
    check(PSXRecompV4::save_user_settings(out, us), "save_user_settings writes");
    const std::string text = read_text(out);
    check(text.find("\nrewind = true\n") != std::string::npos &&
          text.find("\nrewind_depth = 150\n") != std::string::npos &&
          text.find("\nrewind_interval = 8\n") != std::string::npos,
          "a save keeps the rewind settings");
    check(text.find("rewind_pad = 1272") != std::string::npos,
          "a save keeps the Rewind hotkey");
    auto back = PSXRecompV4::load_user_settings(out);
    check(back.has_hotkey_pad_rewind && back.hotkey_pad_rewind == 1272 &&
          back.extra_player_settings["video.rewind_depth"] == "150",
          "the rewind settings survive a save/load round trip");
    fs::remove(out);

    p = write_temp("psxrecomp_rewind_settings_bad.toml",
        "[video]\n"
        "rewind = true\n"
        "rewind_depth = 75\n"
        "rewind_interval = 5\n");
    us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "out-of-choice rewind values are not an error");
    check(us.extra_player_settings.count("video.rewind") == 1 &&
          us.extra_player_settings.count("video.rewind_depth") == 0 &&
          us.extra_player_settings.count("video.rewind_interval") == 0,
          "a depth or interval outside the choices is ignored");
    fs::remove(p);
}

int main() {
    test_rewind_settings_round_trip();

    if (failures) {
        std::fprintf(stderr, "rewind_settings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: rewind and its hotkey are player settings\n");
    return 0;
}
