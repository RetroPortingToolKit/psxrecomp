/* rewind_settings_test — local rewind is the built-in Rewind mod.
 *
 * Local rewind keeps whole-machine snapshots (2 MB main RAM + 1 MB VRAM +
 * 512 KB SPU RAM, uncompressed) in a ring and captures one every
 * rewind_interval frames, so it is opt-in. The switch, the buffer depth and
 * the interval used to be settings.toml [video] keys; they are now the
 * built-in Rewind mod's feature and options (mods/builtin/packages/
 * psx.feature.rewind, runtime/src/mod_builtin_rewind.c). What stays a player
 * setting is the controller hotkey that opens it.
 *
 * What this pins is the settings.toml side of that move:
 *
 *   1. the old rewind / rewind_depth / rewind_interval keys are ignored, not
 *      errors -- an old file keeps every other setting;
 *   2. a save drops them, so they cannot come back as a hidden override of
 *      the mod;
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

static void test_old_keys_ignored_and_dropped() {
    fs::path p = write_temp("psxrecomp_rewind_retired.toml",
        "[video]\n"
        "window_width = 1600\n"
        "rewind = true\n"
        "rewind_depth = 150\n"
        "rewind_interval = 8\n"
        "[hotkeys]\n"
        "rewind_pad = 1272\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "a settings.toml with the old rewind keys still parses");
    check(us.has_window_width && us.window_width == 1600,
          "the player's other settings survive the retired keys");
    check(us.has_hotkey_pad_rewind && us.hotkey_pad_rewind == 1272,
          "the Rewind controller hotkey is still a setting");
    fs::remove(p);

    fs::path out = fs::temp_directory_path() / "psxrecomp_rewind_retired_saved.toml";
    check(PSXRecompV4::save_user_settings(out, us), "save_user_settings writes");
    const std::string text = read_text(out);
    check(text.find("rewind =") == std::string::npos &&
          text.find("rewind_depth") == std::string::npos &&
          text.find("rewind_interval") == std::string::npos,
          "a save drops the retired rewind keys");
    check(text.find("rewind_pad = 1272") != std::string::npos,
          "a save keeps the Rewind hotkey");
    auto back = PSXRecompV4::load_user_settings(out);
    check(back.has_hotkey_pad_rewind && back.hotkey_pad_rewind == 1272,
          "the Rewind hotkey survives a save/load round trip");
    fs::remove(out);
}

int main() {
    test_old_keys_ignored_and_dropped();

    if (failures) {
        std::fprintf(stderr, "rewind_settings_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS: rewind is the Rewind mod; settings.toml keeps only its hotkey\n");
    return 0;
}
