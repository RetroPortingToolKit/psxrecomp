#include "projection_scale_config.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <string>

int main() {
    const auto file = std::filesystem::temp_directory_path() /
        ("psx-projection-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{file};
    auto load = [&](const char* text) {
        { std::ofstream out(file); out << text; }
        return psx_projection_scale_load_config(file);
    };
    if (load("[game]\nname='fixture'\n") != 1.0 ||
        load("[video]\naspect='4:3'\n") != 1.0 ||
        load("[video]\nfov_scale=1.0\n") != 1.0 ||
        load("[video]\nfov_scale=2.0\n") != 2.0 ||
        load("[video]\nfov_scale=8.0\n") != 8.0) return 1;
    for (const char* value : {"0.0", "-1.0", "9.0", "nan", "inf", "'wide'"}) {
        bool rejected = false;
        const std::string text = std::string("[video]\nfov_scale=") + value + "\n";
        try { load(text.c_str()); }
        catch (const std::exception&) { rejected = true; }
        if (!rejected) { std::fprintf(stderr, "accepted invalid scale %s\n", value); return 1; }
    }
    return 0;
}
