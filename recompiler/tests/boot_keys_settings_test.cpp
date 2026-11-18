/* boot_keys_settings_test
 *
 * settings.toml overrides game.toml. The launcher has no control for
 * `bios_hle` or `fast_boot`, yet it wrote both into settings.toml at every
 * save, with whatever value was in force. From then on the file held them for
 * good: a later change of `bios_hle` in the game's game.toml never reached a
 * player who had once started an older build, and two netplay peers of one
 * build could boot in different BIOS modes. (The same latch as `turbo_loads`.)
 *
 * What this checks, for the file side:
 *
 *   1. a file from before settings_format 2 that holds the two keys: they are
 *      not applied (has_* false), the loader says they were echoes, every
 *      other key of the file is kept;
 *   2. saving that state writes the format line and neither key;
 *   3. a file of format 2 that holds a key: the player wrote it; it is applied
 *      and a save writes it back, with its value;
 *   4. a file of format 2 without the keys: nothing is applied, nothing is
 *      written;
 *   5. a settings_format that is not a number counts as an old file.
 *
 * The runtime side (the launcher marks the keys present only when the file
 * set them) is checked by runtime/tests/test_settings_boot_keys_wiring.py.
 */
#include "config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using PSXRecompV4::UserSettings;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    out << text;
}

static std::string read(const fs::path& path) {
    std::stringstream text;
    text << std::ifstream(path, std::ios::binary).rdbuf();
    return text.str();
}

static bool has_line(const std::string& body, const std::string& start) {
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line))
        if (line.rfind(start, 0) == 0) return true;
    return false;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "psx boot keys settings test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path file = dir / "settings.toml";

    /* 1. What a launcher before format 2 wrote: the game's game.toml said
     * bios_hle = true at that time, and the launcher echoed it. */
    write(file,
          "# psxrecomp user settings - written by the launcher. Safe to hand-edit.\n"
          "\n[video]\n"
          "renderer          = \"opengl\"\n"
          "window_width      = 1600\n"
          "fast_boot         = false\n"
          "bios_hle          = true\n"
          "fullscreen        = 1\n"
          "\n[audio]\n"
          "volume = 70\n");
    UserSettings old_file = PSXRecompV4::load_user_settings(file);
    check(!old_file.parse_error, "the old file parses");
    check(old_file.settings_format == 0, "a file without settings_format is format 0");
    check(!old_file.has_bios_hle && !old_file.has_fast_boot,
          "an old file's bios_hle and fast_boot are not applied");
    check(old_file.boot_keys_were_echoes, "the loader says the old file held the echoed keys");
    check(old_file.has_renderer && old_file.renderer == 1 &&
              old_file.has_window_width && old_file.window_width == 1600 &&
              old_file.has_fullscreen && old_file.fullscreen == 1,
          "every other key of the old file is kept");

    /* 2. The next save drops the keys and stamps the format. */
    check(PSXRecompV4::save_user_settings(file, old_file), "save the old file's state");
    std::string body = read(file);
    check(has_line(body, "settings_format = 2"), "a save writes settings_format = 2");
    check(body.find("settings_format") < body.find("[video]"),
          "settings_format is a top-level key, before the first table");
    check(!has_line(body, "bios_hle") && !has_line(body, "fast_boot"),
          "the echoed keys are gone after the save");
    check(has_line(body, "renderer") && has_line(body, "window_width"),
          "the player's other settings survive the save");
    UserSettings healed = PSXRecompV4::load_user_settings(file);
    check(healed.settings_format == 2 && !healed.has_bios_hle && !healed.has_fast_boot &&
              !healed.boot_keys_were_echoes,
          "the rewritten file is format 2 with no boot key and nothing to report");

    /* 3. The player adds a line to a format 2 file: it is their choice. */
    write(file,
          "settings_format = 2\n"
          "\n[video]\n"
          "renderer          = \"opengl\"\n"
          "bios_hle          = false\n");
    UserSettings chosen = PSXRecompV4::load_user_settings(file);
    check(chosen.settings_format == 2, "format 2 is read");
    check(chosen.has_bios_hle && !chosen.bios_hle, "a bios_hle line in a format 2 file is applied");
    check(!chosen.has_fast_boot, "a key that is not in the file is not applied");
    check(!chosen.boot_keys_were_echoes, "a format 2 file has no echo to report");
    check(PSXRecompV4::save_user_settings(file, chosen), "save the chosen state");
    body = read(file);
    check(has_line(body, "bios_hle          = false"), "a chosen bios_hle is written back with its value");
    check(!has_line(body, "fast_boot"), "fast_boot is not invented by the save");
    chosen = PSXRecompV4::load_user_settings(file);
    check(chosen.has_bios_hle && !chosen.bios_hle, "the chosen value survives a save and a load");

    write(file,
          "settings_format = 2\n"
          "\n[video]\n"
          "fast_boot         = true\n"
          "bios_hle          = true\n");
    UserSettings both = PSXRecompV4::load_user_settings(file);
    check(both.has_fast_boot && both.fast_boot && both.has_bios_hle && both.bios_hle,
          "both keys of a format 2 file are applied");

    /* 4. What the launcher writes for a player who chose nothing. */
    UserSettings plain{};
    plain.renderer = 1; plain.has_renderer = true;
    check(PSXRecompV4::save_user_settings(file, plain), "save without boot keys");
    body = read(file);
    check(has_line(body, "settings_format = 2") && !has_line(body, "bios_hle") &&
              !has_line(body, "fast_boot"),
          "a save without a chosen value writes neither key");
    UserSettings back = PSXRecompV4::load_user_settings(file);
    check(!back.has_bios_hle && !back.has_fast_boot && !back.boot_keys_were_echoes,
          "such a file leaves both values to game.toml");

    /* 5. A format that is not a number is an old file. */
    write(file,
          "settings_format = \"two\"\n"
          "\n[video]\n"
          "bios_hle          = true\n");
    UserSettings odd = PSXRecompV4::load_user_settings(file);
    check(odd.settings_format == 0 && !odd.has_bios_hle && odd.boot_keys_were_echoes,
          "a settings_format that is not a number counts as an old file");

    /* A missing file is not an old file with echoes. */
    UserSettings none = PSXRecompV4::load_user_settings(dir / "absent.toml");
    check(!none.boot_keys_were_echoes && !none.has_bios_hle, "a missing file reports nothing");

    fs::remove_all(dir, ec);
    if (failures) return 1;
    std::printf("boot_keys_settings_test: all checks passed\n");
    return 0;
}
