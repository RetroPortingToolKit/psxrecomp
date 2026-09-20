#include "host_path.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using namespace PSXRecompV4;
static int checks = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    for (const char* spelling : {
            R"(\\server\share\BIOS files\SCPH5500.BIN)",
            "//server/share/Game (Japan)/game.cue",
            R"(\\?\UNC\server\share\game.cue)",
            R"(\\?\C:\Game files\game.cue)",
            R"(Z:\Game files\game.cue)"}) {
        const fs::path path(spelling);
        check(host_path_is_absolute(path), "fully qualified Windows path");
        std::error_code ec = std::make_error_code(std::errc::invalid_argument);
        check(host_absolute(path, ec).native() == path.native() && !ec,
              "nonthrowing absolute preserves exact Windows path and clears error");
        check(host_absolute(path).native() == path.native(),
              "throwing absolute preserves exact Windows path");
        check(host_resolve(R"(D:\unrelated\config)", path).native() == path.native(),
              "config root never replaces an absolute Windows path");
    }
    check(!host_path_is_absolute(R"(\current-drive\file)"), "drive-rooted path needs current drive");
    check(!host_path_is_absolute(R"(C:drive-relative.bin)"), "drive-relative path needs drive directory");
#else
    check(host_absolute("/tmp/PSX files/game.cue") == fs::path("/tmp/PSX files/game.cue"),
          "POSIX absolute path unchanged");
    check(!host_path_is_absolute(R"(\\server\share\file)"), "backslashes remain ordinary on POSIX");
#endif
    const fs::path relative("relative directory/game.cue");
    check(!host_path_is_absolute(relative), "ordinary relative path");
    check(host_absolute(relative) == fs::absolute(relative), "relative path anchors to cwd");
    const auto root = fs::current_path() / "config directory";
    check(host_resolve(root, relative) == root / relative, "relative config value anchors to root");
    // Optional private integration input: inspect only, never change the asset.
    if (argc == 2) {
        const fs::path path(argv[1]);
        check(bool(std::ifstream(path, std::ios::binary)), "original private path readable");
        check(bool(std::ifstream(host_absolute(path), std::ios::binary)), "resolved private path readable");
        check(host_absolute(path).native() == path.native(), "private path remains exact");
    }
    std::printf("host_path: %d checks passed\n", checks);
}
