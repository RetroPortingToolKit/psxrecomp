/* select_shortcuts_config_test — game.toml [controller] select_shortcuts.
 *
 *   1. absent means the host keeps its Select + button shortcuts;
 *   2. false turns them off, true spells the default;
 *   3. it sits beside direct_shortcut without changing it. */
#include "config_loader.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
    const auto root = fs::temp_directory_path() /
        ("select-shortcuts-config-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto config = root / "game.toml";
    auto load = [&](const std::string& controller) {
        {
            std::ofstream f(config);
            f << "[game]\nname=\"Test\"\nid=\"SLUS-TEST\"\nexe=\"stock.exe\"\n"
                 "load_address=\"0x80010000\"\nentry_pc=\"0x80010000\"\ntext_size=\"0x1000\"\n"
                 "stack_base=\"0x801ffff0\"\n[recompiler]\nseeds=\"seeds.txt\"\n"
              << controller;
        }
        return PSXRecompV4::load_game_config(config);
    };

    check(load("").runtime.select_shortcuts, "absent: Select shortcuts stay on");
    check(load("[controller]\ndefault_mode=\"digital\"\n").runtime.select_shortcuts,
          "a [controller] block without the key: on");
    check(!load("[controller]\nselect_shortcuts=false\n").runtime.select_shortcuts,
          "false turns them off");
    check(load("[controller]\nselect_shortcuts=true\n").runtime.select_shortcuts,
          "true is the default spelled out");
    const auto both = load("[controller]\nselect_shortcuts=false\n"
                           "direct_shortcut=\"fast_forward_toggle\"\n"
                           "direct_shortcut_button=\"leftstick\"\n");
    check(!both.runtime.select_shortcuts &&
          both.runtime.direct_shortcut == "fast_forward_toggle" &&
          both.runtime.direct_shortcut_button == "leftstick",
          "direct_shortcut is kept beside it");

    fs::remove(config);
    fs::remove(root);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("select_shortcuts_config_test: ok");
    return 0;
}
