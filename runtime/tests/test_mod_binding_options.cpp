#include "mod_packages.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace PSXRecompV4;
namespace fs = std::filesystem;
static int failures;
static void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
}

int main() {
    const fs::path root = fs::current_path() / "mod-binding-fixture";
    const fs::path path = root / "bundled/input.test/1.0.0/manifest.toml";
    fs::create_directories(path.parent_path());
    const std::string manifest = R"(format_version = 12
id = "input.test"
version = "1.0.0"
name = "Input test"
resolver = "declarative"
[[target]]
game_id = "SLUS-TEST"
[[feature]]
id = "input"
name = "Input"
[[option]]
feature = "input"
id = "fire"
label = "Fire"
type = "binding"
default = "mouse:1,key:40"
[[option]]
feature = "input"
id = "cycle"
label = "Cycle"
type = "binding"
allow_wheel = true
default = "wheel:down"
[[option]]
feature = "input"
id = "sensitivity"
label = "Sensitivity"
type = "integer"
slider = true
min = 1
max = 10
default = 5
)";
    auto write = [&](const std::string& text) { std::ofstream(path) << text; };
    write(manifest);
    std::string error;
    ModPackageManager manager(root);
    check(manager.scan(&error), error.c_str());
    check(manager.load_state(&error), error.c_str());
    check(manager.set_feature_enabled("input.test", "input", true, &error), error.c_str());
    const auto* package = manager.selected_package("input.test");
    check(package && package->options.size() == 3, "format-12 options are loaded");
    if (package && package->options.size() == 3) {
        check(package->options[0].type == ModOptionType::Binding, "binding type preserved");
        check(package->options[1].allow_wheel, "wheel permission preserved");
        check(package->options[2].slider, "slider presentation preserved");
    }
    auto set = [&](const char* id, const char* value) {
        return manager.set_feature_option("input.test", "input", id, value, &error);
    };
    check(set("fire", "key:9,mouse:5,none"), "accept three alternatives");
    check(!set("fire", "wheel:up"), "reject wheel on held action");
    check(!set("fire", "key:512"), "reject invalid scancode");
    check(!set("fire", "none,"), "reject empty alternative");
    check(!set("fire", "none,none,none,none"), "reject fourth alternative");
    check(set("cycle", "wheel:up"), "accept wheel rebinding");
    check(!set("sensitivity", "11"), "enforce slider range");
    check(set("sensitivity", "7"), "accept slider selection");
    check(manager.save_state(&error), error.c_str());
    ModPackageManager restored(root);
    check(restored.scan(&error) && restored.load_state(&error), error.c_str());
    check(restored.feature_option_value("input.test", "input", "fire") == "key:9,mouse:5,none",
          "binding alternatives survive restart");
    check(restored.feature_option_value("input.test", "input", "cycle") == "wheel:up",
          "wheel direction survives restart");
    check(restored.feature_option_value("input.test", "input", "sensitivity") == "7",
          "slider value survives restart");
    auto rejects = [&](std::string text) {
        write(text);
        ModPackageManager invalid(root);
        invalid.scan(&error);
        check(invalid.packages().empty(), "invalid manifest must not enter catalog");
    };
    std::string old = manifest;
    old.replace(old.find("12"), 2, "11");
    rejects(old);
    std::string bad = manifest;
    bad.replace(bad.find("mouse:1,key:40"), 14, "wheel:up");
    rejects(bad);
    std::string decimal = manifest;
    decimal.replace(decimal.find("12"), 2, "13");
    const auto bounds = decimal.find("min = 1\nmax = 10\ndefault = 5");
    decimal.replace(bounds, std::string("min = 1\nmax = 10\ndefault = 5").size(),
                    "min = 100\nmax = 250\ndefault = 125\ndisplay_divisor = 100");
    write(decimal);
    ModPackageManager scaled(root);
    check(scaled.scan(&error), error.c_str());
    check(scaled.set_feature_enabled("input.test", "input", true, &error), error.c_str());
    const auto* scaled_package = scaled.selected_package("input.test");
    check(scaled_package && scaled_package->options[2].display_divisor == 100,
          "decimal display scale reaches provider metadata");
    for (const char* value : {"100", "125", "250"})
        check(scaled.set_feature_option("input.test", "input", "sensitivity", value, &error),
              "decimal slider accepts bounds and fractional default");
    check(!scaled.set_feature_option("input.test", "input", "sensitivity", "251", &error),
          "decimal slider rejects above maximum");
    check(scaled.save_state(&error), error.c_str());
    ModPackageManager decimal_reload(root);
    check(decimal_reload.scan(&error) && decimal_reload.load_state(&error), error.c_str());
    check(decimal_reload.feature_option_value("input.test", "input", "sensitivity") == "250",
          "decimal slider selection survives restart");
    std::string older = decimal;
    older.replace(older.find("13"), 2, "12");
    rejects(older);
    for (const char* divisor : {"0", "25", "10000000"}) {
        std::string invalid = decimal;
        const auto pos = invalid.find("display_divisor = 100");
        invalid.replace(pos, std::string("display_divisor = 100").size(),
                        std::string("display_divisor = ") + divisor);
        rejects(invalid);
    }
    fs::remove_all(root);
    if (!failures) std::cout << "mod binding options: PASS\n";
    return failures ? 1 : 0;
}
