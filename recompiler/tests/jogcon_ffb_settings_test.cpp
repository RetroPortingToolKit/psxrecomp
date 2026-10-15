/* jogcon_ffb_settings_test — EXPERIMENTAL wheel force feedback is a player
 * setting, off by default (settings.toml [controller] jogcon_force_feedback).
 *
 *   1. a fresh UserSettings and an absent key mean off and "no opinion", so
 *      the title's game.toml default (off) applies;
 *   2. settings.toml turns it on and can spell an explicit off;
 *   3. save_user_settings round-trips it beside the other [controller] keys;
 *   4. the game.toml key is experimental_jogcon_force_feedback, off when
 *      absent. */
#include "config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
static fs::path write_temp(const std::string& name, const std::string& body) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
    return p;
}

int main() {
    PSXRecompV4::UserSettings fresh;
    check(!fresh.jogcon_force_feedback && !fresh.has_jogcon_force_feedback,
          "fresh: off, no opinion");

    fs::path p = write_temp("psxrecomp_ffb_absent.toml", "[controller]\np1_mode = \"analog\"\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error && !us.has_jogcon_force_feedback && !us.jogcon_force_feedback,
          "absent key: off, no opinion");
    fs::remove(p);

    p = write_temp("psxrecomp_ffb_on.toml", "[controller]\njogcon_force_feedback = true\n");
    us = PSXRecompV4::load_user_settings(p);
    check(us.has_jogcon_force_feedback && us.jogcon_force_feedback, "settings.toml turns it on");
    fs::remove(p);

    p = write_temp("psxrecomp_ffb_off.toml", "[controller]\njogcon_force_feedback = false\n");
    us = PSXRecompV4::load_user_settings(p);
    check(us.has_jogcon_force_feedback && !us.jogcon_force_feedback, "explicit off is kept");
    fs::remove(p);

    {
        PSXRecompV4::UserSettings s;
        s.jogcon_force_feedback = true; s.has_jogcon_force_feedback = true;
        s.multitap_analog = false; s.has_multitap_analog = true;
        fs::path out = fs::temp_directory_path() / "psxrecomp_ffb_roundtrip.toml";
        check(PSXRecompV4::save_user_settings(out, s), "save");
        auto r = PSXRecompV4::load_user_settings(out);
        check(r.has_jogcon_force_feedback && r.jogcon_force_feedback,
              "round-trip keeps the setting");
        check(r.has_multitap_analog && !r.multitap_analog,
              "round-trip keeps the other [controller] keys");
        fs::remove(out);
    }

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("jogcon_ffb_settings_test: ok");
    return 0;
}
