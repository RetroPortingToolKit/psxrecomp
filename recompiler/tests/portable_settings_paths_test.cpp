/* portable_settings_paths_test
 *
 * A portable game folder must keep working after it is moved or copied to
 * another PC. The launcher saves the disc, BIOS and memory-card paths the
 * player used; saved as absolute paths they pinned the folder to its first
 * location. What this pins:
 *
 *   1. a path inside the settings folder is written relative to it;
 *   2. a path outside it (a NAS disc, another drive) stays absolute;
 *   3. the relative form reads back unchanged, so the reader's exe-directory
 *      anchoring resolves it on the new PC.
 */
#include "config_loader.h"
#include "host_path.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using PSXRecompV4::relative_to_folder;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    const fs::path game = fs::temp_directory_path() / "psx portable paths test" / "game";
    std::error_code ec;
    fs::remove_all(game.parent_path(), ec);
    fs::create_directories(game / "inputs" / "disc-1");
    fs::create_directories(game.parent_path() / "elsewhere");

    const fs::path disc = game / "inputs" / "disc-1" / "Game (Europe).chd";
    const fs::path outside = game.parent_path() / "elsewhere" / "bios.bin";

    check(relative_to_folder(disc, game).generic_string() == "inputs/disc-1/Game (Europe).chd",
          "a path inside the folder becomes relative");
    check(relative_to_folder(outside, game) == outside, "a path outside the folder stays absolute");
    check(relative_to_folder(fs::path("inputs/x.bin"), game) == fs::path("inputs/x.bin"),
          "an already relative path is unchanged");
    check(relative_to_folder(fs::path(), game).empty(), "an empty path stays empty");

    PSXRecompV4::UserSettings s{};
    check(!s.has_volume && s.volume == 100, "fresh volume leaves the host default");
    s.has_volume = true; s.volume = 37;
    s.bios_path = game / "inputs" / "[EU] SCPH5552.BIN"; s.has_bios_path = true;
    s.disc_path = disc; s.has_disc_path = true;
    s.memcard_dir = game / "saves"; s.has_memcard_dir = true;
    s.memcard1_path = game / "saves" / "card1.mcd"; s.has_memcard1_path = true;
    s.memcard2_path = outside; s.has_memcard2_path = true;
    const fs::path settings = game / "settings.toml";
    check(PSXRecompV4::save_user_settings(settings, s), "settings save");

    std::stringstream text;
    text << std::ifstream(settings).rdbuf();
    const std::string body = text.str();
    check(body.find("path = \"inputs/[EU] SCPH5552.BIN\"") != std::string::npos, "BIOS saved relative");
    check(body.find("path = \"inputs/disc-1/Game (Europe).chd\"") != std::string::npos, "disc saved relative");
    check(body.find("dir     = \"saves\"") != std::string::npos, "memory card folder saved relative");
    check(body.find("card1   = \"saves/card1.mcd\"") != std::string::npos, "card 1 saved relative");
    check(body.find(outside.generic_string()) != std::string::npos, "outside card stays absolute");

    const PSXRecompV4::UserSettings back = PSXRecompV4::load_user_settings(settings);
    check(back.has_volume && back.volume == 37, "host volume survives the settings round trip");
    check(back.has_disc_path && back.disc_path == fs::path("inputs/disc-1/Game (Europe).chd"),
          "relative disc path reads back unchanged");
    check(back.has_memcard1_path && back.memcard1_path == fs::path("saves/card1.mcd"),
          "relative card path reads back unchanged");

#ifdef _WIN32
    // A NAS selection must survive a launcher save and reopen. No network
    // asset is opened: only the task-local settings file is written/read.
    const fs::path expected("//server/share/Game files/disc.chd");
    for (const char* spelling : {
            "//server/share/Game files/disc.chd",
            R"(\\server\share\Game files\disc.chd)"}) {
        PSXRecompV4::UserSettings network{};
        network.disc_path = spelling; network.has_disc_path = true;
        network.bios_path = spelling; network.has_bios_path = true;
        network.memcard1_path = spelling; network.has_memcard1_path = true;
        check(PSXRecompV4::save_user_settings(settings, network), "UNC settings save");
        const auto restored = PSXRecompV4::load_user_settings(settings);
        check(restored.has_disc_path &&
              restored.disc_path.native() == expected.native(),
              "saved UNC disc keeps both leading separators");
        check(restored.has_bios_path &&
              restored.bios_path.native() == expected.native(),
              "saved UNC BIOS keeps both leading separators");
        check(restored.has_memcard1_path &&
              restored.memcard1_path.native() == expected.native(),
              "saved UNC memory card keeps both leading separators");
        check(PSXRecompV4::host_resolve(game, restored.disc_path).native() == expected.native(),
              "reopened UNC disc never becomes a path on the current drive");
    }
#endif

    // A packaged game is its own project root even when an ancestor carries
    // a root marker (a global ~/.gitignore): its relative [runtime] paths
    // (memcard_dir = "saves") must stay inside the game folder.
    {
        const fs::path home = game.parent_path() / "home";
        const fs::path pkg = home / "Games" / "R4";
        fs::create_directories(pkg / "licenses");
        std::ofstream(home / ".gitignore") << "*.o\n";
        std::ofstream(pkg / "game.toml") << "\n";
        check(PSXRecompV4::find_project_root(pkg / "game.toml") == home,
              "precondition: a plain folder walks up to the marked ancestor");
        std::ofstream(pkg / "licenses" / "psxrecomp-LICENSE") << "MIT\n";
        check(PSXRecompV4::find_project_root(pkg / "game.toml") == PSXRecompV4::host_absolute(pkg),
              "a packaged game folder is its own root");
    }

    /* A setup install. settings.toml is in
     * <root>/build-release, the memory cards are one level above it, in
     * <root>/saves. They were stored by full path, so a moved or copied
     * install kept using the old folder: an empty card for the player, or the
     * saves of the other copy. */
    {
        const fs::path top = game.parent_path();
        const fs::path install = top / "install";
        const fs::path moved = top / "moved here";
        const fs::path exe_dir = install / "build-release";
        fs::create_directories(exe_dir);
        fs::create_directories(install / "saves");
        fs::create_directories(install / "tools" / "deep");
        const fs::path cards = install / "saves";
        const fs::path nas_disc = top / "elsewhere" / "Game (Disc 1).chd";
        const fs::path own_disc = install / "disc" / "Game (Disc 1).chd";

        check(relative_to_folder(cards, exe_dir) == cards,
              "without an install root, a path above the folder stays absolute");
        check(relative_to_folder(cards, exe_dir, install).generic_string() == "../saves",
              "the cards of a setup install are written with one climb");
        check(relative_to_folder(cards / "card1.mcd", exe_dir, install).generic_string() ==
                  "../saves/card1.mcd",
              "a card file of a setup install is written with one climb");
        check(relative_to_folder(exe_dir / "saves", exe_dir, install).generic_string() == "saves",
              "a path inside the folder stays plain relative with an install root");
        check(relative_to_folder(outside, exe_dir, install) == outside,
              "a path outside the install root stays absolute");
        check(relative_to_folder(cards, install / "tools" / "deep", install) == cards,
              "a folder deeper than one level below the root gets no climb");
        check(relative_to_folder(cards, exe_dir, top) == cards,
              "a root that is not the folder's parent gives no climb");

        PSXRecompV4::set_user_settings_install_root(install);
        PSXRecompV4::UserSettings u{};
        u.disc_path = own_disc; u.has_disc_path = true;
        u.bios_path = nas_disc; u.has_bios_path = true;
        u.memcard_dir = cards; u.has_memcard_dir = true;
        u.memcard1_path = cards / "card1.mcd"; u.has_memcard1_path = true;
        u.memcard2_path = outside; u.has_memcard2_path = true;
        const fs::path install_settings = exe_dir / "settings.toml";
        check(PSXRecompV4::save_user_settings(install_settings, u), "setup install settings save");
        std::stringstream saved;
        saved << std::ifstream(install_settings).rdbuf();
        const std::string install_body = saved.str();
        check(install_body.find("dir     = \"../saves\"") != std::string::npos,
              "setup install: the card folder is saved as ../saves");
        check(install_body.find("card1   = \"../saves/card1.mcd\"") != std::string::npos,
              "setup install: card 1 is saved below ../saves");
        check(install_body.find("card2   = \"" + outside.generic_string() + "\"") != std::string::npos,
              "setup install: a card outside the install stays absolute");
        check(install_body.find("path = \"" + own_disc.generic_string() + "\"") != std::string::npos,
              "setup install: the disc path keeps the plain rule (no climb)");
        check(install_body.find(install.generic_string() + "/saves") == std::string::npos,
              "setup install: the full path of the card folder is not in the file");

        /* The move. The reader anchors a relative value on the exe folder
         * (anchor_card_path in runtime/src/main.cpp); the same expression here. */
        fs::create_directories(moved);
        fs::copy(install, moved, fs::copy_options::recursive, ec);
        check(!ec, "the install can be copied");
        const fs::path moved_exe_dir = moved / "build-release";
        const PSXRecompV4::UserSettings after_move =
            PSXRecompV4::load_user_settings(moved_exe_dir / "settings.toml");
        check(after_move.has_memcard_dir && after_move.memcard_dir == fs::path("../saves"),
              "the relative card folder reads back unchanged");
        check((moved_exe_dir / after_move.memcard_dir).lexically_normal() == moved / "saves",
              "a moved install uses its own saves folder");
        check((moved_exe_dir / after_move.memcard1_path).lexically_normal() ==
                  moved / "saves" / "card1.mcd",
              "a moved install uses its own card file");

        /* An install written by an earlier version holds the full path. After
         * an update the path is read as it stands (same folder, cards in
         * place), and the next save writes the relative form. */
        {
            std::ofstream old_file(install_settings, std::ios::trunc);
            old_file << "[memcard]\n"
                     << "dir     = \"" << cards.generic_string() << "\"\n"
                     << "card1   = \"" << (cards / "card1.mcd").generic_string() << "\"\n";
        }
        const PSXRecompV4::UserSettings old_form = PSXRecompV4::load_user_settings(install_settings);
        check(old_form.has_memcard_dir && old_form.memcard_dir == cards,
              "an existing install's full card path is read as it stands");
        check(PSXRecompV4::save_user_settings(install_settings, old_form), "existing install re-save");
        const PSXRecompV4::UserSettings healed = PSXRecompV4::load_user_settings(install_settings);
        check(healed.memcard_dir == fs::path("../saves") &&
                  (exe_dir / healed.memcard_dir).lexically_normal() == cards,
              "an existing install is rewritten relative and still names the same folder");

        /* An install that was moved while its file still held the full path:
         * the path names the old place, which is outside the new root, so it
         * stays as it is. The runtime cannot tell it from a folder the player
         * chose. */
        PSXRecompV4::set_user_settings_install_root(moved);
        check(PSXRecompV4::save_user_settings(moved_exe_dir / "settings.toml", old_form),
              "moved install with an old full path: save");
        const PSXRecompV4::UserSettings stale =
            PSXRecompV4::load_user_settings(moved_exe_dir / "settings.toml");
        check(stale.memcard_dir == cards,
              "a full path to another install is kept, not guessed at");

        PSXRecompV4::set_user_settings_install_root({});
        check(PSXRecompV4::save_user_settings(install_settings, u), "save without an install root");
        const PSXRecompV4::UserSettings plain = PSXRecompV4::load_user_settings(install_settings);
        check(plain.memcard_dir == cards,
              "without an install root the old rule holds: a path above the folder stays absolute");
    }

    fs::remove_all(game.parent_path(), ec);
    if (failures) return 1;
    std::printf("portable_settings_paths_test: all checks passed\n");
    return 0;
}
