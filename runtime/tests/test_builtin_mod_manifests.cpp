/* Every framework-builtin mod package must parse under the current manifest
 * schema. A builtin that fails to parse is silently dropped from the catalog
 * at runtime ("mod manifest ignored"), which for an activation package such as
 * psx.enhancement.8mb-ram makes the feature impossible to enable while every
 * other test stays green.
 *
 * Usage: builtin_mod_manifests_test <mods/builtin/packages dir> */
#include "mod_packages.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static int failures;

static void check(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: builtin_mod_manifests_test <packages dir>\n";
        return 2;
    }
    const fs::path root(argv[1]);
    int parsed = 0;
    bool saw_8mb = false;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().filename() != "manifest.toml")
            continue;
        ModPackage package;
        std::string error;
        const bool ok = ModPackageManager::read_manifest(entry.path(), package, &error);
        check(ok, entry.path().string() + ": " + error);
        if (!ok) continue;
        parsed++;
        const fs::path version_dir = entry.path().parent_path();
        check(package.version == version_dir.filename().string(),
              entry.path().string() + ": version does not match its directory");
        check(package.id == version_dir.parent_path().filename().string(),
              entry.path().string() + ": id does not match its directory");
        if (package.id == "psx.enhancement.8mb-ram") {
            saw_8mb = true;
            check(package.features.size() == 1 && package.features[0].id == "8mb-ram",
                  "8 MB package exposes exactly the 8mb-ram feature");
            check(!package.features.empty() && !package.features[0].default_enabled,
                  "8 MB RAM is opt-in (default disabled)");
            check(!package.features.empty() && package.features[0].hidden,
                  "8 MB RAM is hidden by default");
            check(package.plugins.size() == 1 && package.plugins[0].id == "psx.8mb-ram" &&
                      package.plugins[0].feature_id == "8mb-ram",
                  "8 MB feature activates the psx.8mb-ram plugin");
            check(package.targets.size() == 1 && package.targets[0].game_id == "*",
                  "8 MB package applies to every game");
        }
    }
    check(parsed > 0, "no builtin manifests found under " + root.string());
    check(saw_8mb, "psx.enhancement.8mb-ram builtin package missing");
    if (failures == 0)
        std::cout << "PASS builtin mod manifests (" << parsed << " packages)\n";
    return failures ? 1 : 0;
}
