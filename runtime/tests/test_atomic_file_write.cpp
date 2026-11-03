#include "atomic_file_write.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static bool read_file(const std::filesystem::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return bool(in);
}

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("psx-atomic-write-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const auto target = root / "receipt.json";
    {
        std::ofstream old(target, std::ios::binary);
        old << "old";
    }

    std::string error;
    if (!psx::atomic_write_file(target, "{\"complete\":true}\n", &error)) {
        std::fprintf(stderr, "atomic write failed: %s\n", error.c_str());
        return 1;
    }
    std::string got;
    if (!read_file(target, &got) || got != "{\"complete\":true}\n") {
        std::fprintf(stderr, "destination was not atomically replaced\n");
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path() != target) {
            std::fprintf(stderr, "temporary file leaked\n");
            return 1;
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return ec ? 1 : 0;
}
