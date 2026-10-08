/* video_enhancement_settings_test — [video] geometry_correction /
 * perspective_texturing plumbing.
 *
 * These two knobs are the opt-in for the sub-pixel vertex precision and
 * perspective-correct UV enhancements (psxrecomp issue #92). The underlying
 * GTE/GPU machinery has its own unit coverage in the runtime suite; what this
 * test pins is the part that decides whether it is ever switched on:
 *
 *   1. both default OFF (the faithful floor) when game.toml says nothing;
 *   2. game.toml [video] turns them on;
 *   3. settings.toml (the player's file) parses them;
 *   4. save_user_settings round-trips them — a launcher save must not silently
 *      drop a hand-edited key, which would look like "the setting does nothing".
 */
#include "config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

/* game.toml needs the fields load_game_config() requires; keep this to the
 * documented minimum plus whichever [video] body the case under test needs.
 *
 * [runtime] is required even when empty: parse_runtime_block() early-returns
 * on a config with no [runtime] table, so a game.toml carrying [video] alone
 * would silently ignore every video key. Every shipped game.toml has both. */
static fs::path write_game_toml(const std::string& name,
                                const std::string& video_block) {
    return write_temp(name,
        "[game]\n"
        "name = \"probe\"\n"
        "exe = \"probe.exe\"\n"
        "load_address = \"0x80010000\"\n"
        "entry_pc = \"0x80010000\"\n"
        "text_size = \"0x1000\"\n"
        "[recompiler]\n"
        "seeds = \"seeds.json\"\n"
        "[runtime]\n"
        + video_block);
}

/* BOTH default off — the faithful floor — but for different reasons, and the
 * distinction matters if anyone reconsiders these later. geometry_correction is
 * off because it is BROKEN at the coverage the runtime can reach (it moves
 * vertices and splits shared edges) and has no launcher control at all.
 * perspective_texturing is off because it is a deliberate departure from hardware
 * output validated on only one title and renderer; it is structurally safe and
 * players opt in from the launcher. See docs/ENHANCEMENTS.md G1.8/G1.9. */
static void test_defaults_off() {
    fs::path p = write_game_toml("psxrecomp_pgxp_default.toml", "");
    auto gc = PSXRecompV4::load_game_config(p);
    check(!gc.runtime.video_geometry_correction,
          "geometry_correction defaults OFF (known to crack meshes)");
    check(!gc.runtime.video_perspective_texturing,
          "perspective_texturing defaults OFF (faithful floor; opt-in)");
    fs::remove(p);
}

static void test_game_toml_opt_in() {
    fs::path p = write_game_toml("psxrecomp_pgxp_on.toml",
        "[video]\n"
        "window_width = 1920\n"
        "geometry_correction = true\n"
        "perspective_texturing = true\n");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_window_width == 1920,
          "[video] window_width is honoured");
    check(gc.runtime.video_geometry_correction,
          "[video] geometry_correction = true is honoured");
    check(gc.runtime.video_perspective_texturing,
          "[video] perspective_texturing = true is honoured");
    fs::remove(p);
}

static void test_game_window_width_validation() {
    fs::path p = write_game_toml("psxrecomp_window_too_small.toml",
        "[video]\n"
        "window_width = 639\n");
    bool rejected = false;
    try {
        (void)PSXRecompV4::load_game_config(p);
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "[video] window_width rejects values below 640");
    fs::remove(p);
}

/* The two knobs are independent: a title may want stable geometry without
 * changing texture mapping (or the reverse) — so a single flag would be wrong. */
static void test_knobs_independent() {
    /* Each knob must be settable against the other's default: geometry ON while
     * perspective is explicitly opted OUT proves neither key implies the other. */
    fs::path p = write_game_toml("psxrecomp_pgxp_geom_only.toml",
        "[video]\n"
        "geometry_correction = true\n"
        "perspective_texturing = false\n");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_geometry_correction,
          "geometry_correction turns on independently");
    check(!gc.runtime.video_perspective_texturing,
          "perspective_texturing can be opted OUT while geometry is on");
    fs::remove(p);

    /* And the converse direction: perspective_texturing alone must turn on
     * WITHOUT dragging the broken geometry knob on with it. This is the case that
     * matters most in practice — it is what the launcher checkbox does. */
    fs::path q = write_game_toml("psxrecomp_pgxp_persp_only.toml",
        "[video]\n"
        "perspective_texturing = true\n");
    auto gq = PSXRecompV4::load_game_config(q);
    check(gq.runtime.video_perspective_texturing,
          "perspective_texturing alone turns on");
    check(!gq.runtime.video_geometry_correction,
          "perspective_texturing does NOT drag geometry_correction on with it");
    fs::remove(q);
}

static void test_user_settings_read() {
    fs::path p = write_temp("psxrecomp_pgxp_settings.toml",
        "[video]\n"
        "geometry_correction = true\n"
        "perspective_texturing = false\n"
        "[audio]\n"
        "frequency = 48000\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error, "settings.toml parses");
    check(us.has_geometry_correction && us.geometry_correction,
          "settings.toml geometry_correction = true read");
    check(us.has_perspective_texturing && !us.perspective_texturing,
          "settings.toml perspective_texturing = false read (explicit off)");
    check(us.has_audio_freq && us.audio_freq == 48000,
          "settings.toml audio frequency read");
    fs::remove(p);
}

/* An absent key must stay absent, so layering leaves the game.toml value alone
 * instead of forcing it off. */
static void test_user_settings_absent_key() {
    fs::path p = write_temp("psxrecomp_pgxp_settings_empty.toml",
        "[video]\n"
        "supersampling = 2\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(!us.has_geometry_correction,
          "absent geometry_correction leaves has_* false");
    check(!us.has_perspective_texturing,
          "absent perspective_texturing leaves has_* false");
    fs::remove(p);
}

static void test_user_settings_round_trip() {
    PSXRecompV4::UserSettings out;
    out.geometry_correction = true;   out.has_geometry_correction = true;
    out.perspective_texturing = true; out.has_perspective_texturing = true;
    out.audio_freq = 48000;           out.has_audio_freq = true;

    fs::path p = fs::temp_directory_path() / "psxrecomp_pgxp_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes");

    auto back = PSXRecompV4::load_user_settings(p);
    check(!back.parse_error, "written settings.toml re-parses");
    check(back.has_geometry_correction && back.geometry_correction,
          "geometry_correction survives a save/load round trip");
    check(back.has_perspective_texturing && back.perspective_texturing,
          "perspective_texturing survives a save/load round trip");
    check(back.has_audio_freq && back.audio_freq == 48000,
          "audio frequency survives a save/load round trip");
    fs::remove(p);
}

/* settings.toml [video] fov_scale: int and float TOML values read, absent stays
 * absent, and a launcher save round-trips it (range checking is the runtime's). */
static void test_user_settings_fov_scale() {
    fs::path p = write_temp("psxrecomp_fov_settings.toml", "[video]\nfov_scale = 2\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(us.has_fov_scale && us.fov_scale == 2.0, "settings.toml integer fov_scale read");
    fs::remove(p);
    p = write_temp("psxrecomp_fov_settings_f.toml", "[video]\nfov_scale = 1.5\n");
    us = PSXRecompV4::load_user_settings(p);
    check(us.has_fov_scale && us.fov_scale == 1.5, "settings.toml float fov_scale read");
    fs::remove(p);
    p = write_temp("psxrecomp_fov_settings_s.toml", "[video]\nfov_scale = \"wide\"\n");
    us = PSXRecompV4::load_user_settings(p);
    check(!us.has_fov_scale, "non-numeric fov_scale leaves has_fov_scale false");
    fs::remove(p);
    PSXRecompV4::UserSettings out;
    out.fov_scale = 1.25; out.has_fov_scale = true;
    p = fs::temp_directory_path() / "psxrecomp_fov_roundtrip.toml";
    check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes fov_scale");
    auto back = PSXRecompV4::load_user_settings(p);
    check(back.has_fov_scale && back.fov_scale == 1.25, "fov_scale survives a save/load round trip");
    fs::remove(p);
}

/* Internal resolution (Settings -> Display): game.toml shipped default,
 * settings.toml precedence over the legacy factor, stable-id round trip,
 * and the legacy supersampling range widened to the runtime's 1..32. */
static void test_internal_resolution_game_toml() {
    fs::path p = write_game_toml("psxrecomp_ir_default.toml", "");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_internal_resolution == 0,
          "internal_resolution defaults unset (supersampling stands)");
    check(gc.runtime.video_resolution_reference_lines == 240,
          "resolution_reference_lines defaults to 240");
    fs::remove(p);

    p = write_game_toml("psxrecomp_ir_4k.toml",
        "[video]\n"
        "internal_resolution = \"4K\"\n"
        "resolution_reference_lines = 240\n"
        "supersampling = 12\n");
    gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_internal_resolution == 2160, "game.toml \"4K\" parses (case-insensitive)");
    check(gc.runtime.video_supersampling == 12, "supersampling accepts 12 (1..32)");
    fs::remove(p);

    p = write_game_toml("psxrecomp_ir_lines.toml",
        "[video]\n"
        "internal_resolution = 1600\n");
    gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_internal_resolution == 1600, "game.toml integer lines parse");
    fs::remove(p);

    for (const char* bad : { "internal_resolution = \"9k\"\n",
                             "internal_resolution = 1\n",
                             "resolution_reference_lines = 50\n",
                             "supersampling = 33\n" }) {
        p = write_game_toml("psxrecomp_ir_bad.toml", std::string("[video]\n") + bad);
        bool rejected = false;
        try { (void)PSXRecompV4::load_game_config(p); } catch (const std::exception&) { rejected = true; }
        check(rejected, bad);
        fs::remove(p);
    }
}

static void test_internal_resolution_settings() {
    fs::path p = write_temp("psxrecomp_ir_settings.toml",
        "[video]\n"
        "supersampling = 2\n"
        "internal_resolution = \"display\"\n"
        "window_width = 7680\n");
    auto us = PSXRecompV4::load_user_settings(p);
    check(us.has_internal_resolution && us.internal_resolution == -1,
          "settings.toml internal_resolution = \"display\" reads -1");
    check(us.has_supersampling && us.supersampling == 2, "legacy supersampling still read");
    check(us.has_window_width && us.window_width == 7680,
          "settings.toml window_width accepts 7680 (was capped at 3840)");
    fs::remove(p);

    p = write_temp("psxrecomp_ir_settings_bad.toml",
        "[video]\n"
        "internal_resolution = \"huge\"\n");
    us = PSXRecompV4::load_user_settings(p);
    check(!us.parse_error && !us.has_internal_resolution,
          "an unknown preset is ignored, not an error");
    fs::remove(p);

    const int values[] = { 1, 720, 1080, 1440, 2160, 2880, 4320, -1, 480 };
    const char* ids[]  = { "native", "720p", "1080p", "1440p", "4k", "5k", "8k", "display", nullptr };
    for (int i = 0; i < 9; i++) {
        PSXRecompV4::UserSettings out;
        out.has_internal_resolution = true; out.internal_resolution = values[i];
        out.has_supersampling = true;       out.supersampling = 4;
        p = fs::temp_directory_path() / "psxrecomp_ir_roundtrip.toml";
        check(PSXRecompV4::save_user_settings(p, out), "save_user_settings writes");
        std::string text;
        {   // closed before fs::remove: Windows cannot delete an open file
            std::ifstream f(p);
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (ids[i])
            check(text.find(std::string("internal_resolution = \"") + ids[i] + "\"") != std::string::npos,
                  "a preset is persisted by its stable id");
        else
            check(text.find("internal_resolution = 480") != std::string::npos,
                  "a custom line count is persisted as an integer");
        auto back = PSXRecompV4::load_user_settings(p);
        check(back.has_internal_resolution && back.internal_resolution == values[i],
              "internal_resolution survives a save/load round trip");
        check(back.has_supersampling && back.supersampling == 4,
              "the legacy supersampling key is still written beside it");
        fs::remove(p);
    }
}

/* [video] texture_window_batching: a game.toml-only OpenGL batching opt-in
 * (the image is unchanged; see gpu_gl_renderer.c s_twin_batching). Off unless
 * the game asks for it. */
static void test_texture_window_batching() {
    fs::path p = write_game_toml("psxrecomp_twin_default.toml", "");
    auto gc = PSXRecompV4::load_game_config(p);
    check(!gc.runtime.video_texture_window_batching,
          "texture_window_batching defaults OFF");
    fs::remove(p);
    fs::path q = write_game_toml("psxrecomp_twin_on.toml",
        "[video]\n"
        "texture_window_batching = true\n");
    auto gq = PSXRecompV4::load_game_config(q);
    check(gq.runtime.video_texture_window_batching,
          "[video] texture_window_batching = true is honoured");
    fs::remove(q);
}

/* docs/ENHANCEMENTS.md G1.11: the PGXP title keys. Defaults keep the
 * historical behaviour (tolerance 0.5, position cache consulted, IR-path
 * shadows); a title built with the hooks sets all three. */
static void test_pgxp_title_keys() {
    fs::path p = write_game_toml("psxrecomp_pgxp_keys_default.toml", "");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_pgxp_tolerance == 0.5,
          "pgxp_tolerance defaults to 0.5");
    check(gc.runtime.video_pgxp_position_fallback,
          "pgxp_position_fallback defaults ON (unchanged behaviour)");
    check(!gc.runtime.video_pgxp_preserve_projection,
          "pgxp_preserve_projection defaults OFF (unchanged behaviour)");
    check(!gc.runtime.video_pgxp_mod_only,
          "pgxp_mod_only defaults OFF (unchanged behaviour)");
    fs::remove(p);

    p = write_game_toml("psxrecomp_pgxp_keys_dataflow.toml",
        "[video]\n"
        "pgxp_tolerance = -1.0\n"
        "pgxp_position_fallback = false\n"
        "pgxp_preserve_projection = true\n"
        "pgxp_mod_only = true\n");
    gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.video_pgxp_mod_only, "pgxp_mod_only = true is honoured");
    check(gc.runtime.video_pgxp_tolerance < 0.0,
          "pgxp_tolerance = -1.0 disables the clamp");
    check(!gc.runtime.video_pgxp_position_fallback,
          "pgxp_position_fallback = false is honoured");
    check(gc.runtime.video_pgxp_preserve_projection,
          "pgxp_preserve_projection = true is honoured");
    check(!gc.runtime.video_geometry_correction &&
          !gc.runtime.video_perspective_texturing,
          "the tuning keys do not turn PGXP on by themselves");
    fs::remove(p);

    p = write_game_toml("psxrecomp_pgxp_keys_bad.toml",
        "[video]\n"
        "pgxp_preserve_projection = 1\n");
    bool rejected = false;
    try { (void)PSXRecompV4::load_game_config(p); } catch (const std::exception&) { rejected = true; }
    check(rejected, "pgxp_preserve_projection must be a boolean");
    fs::remove(p);

    p = write_game_toml("psxrecomp_pgxp_keys_bad_mod_only.toml",
        "[video]\n"
        "pgxp_mod_only = \"yes\"\n");
    rejected = false;
    try { (void)PSXRecompV4::load_game_config(p); } catch (const std::exception&) { rejected = true; }
    check(rejected, "pgxp_mod_only must be a boolean");
    fs::remove(p);
}

/* [timing] guest_cycle_scale and its declarative RAM gate (title constants). */
static void test_timing_gate() {
    fs::path p = write_game_toml("ves_timing_none.toml", "");
    auto gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.guest_cycle_scale == 1, "timing: default scale 1");
    check(gc.runtime.guest_cycle_scale_gate.empty(), "timing: no gate by default");
    check(!gc.runtime.guest_cycle_scale_gated, "timing: mod gate off by default");
    fs::remove(p);

    p = write_game_toml("ves_timing_one.toml",
        "[timing]\nguest_cycle_scale = 64\n"
        "guest_cycle_scale_gate = { addr = 0x800AC794, value = 0x180 }\n");
    gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.guest_cycle_scale == 64, "timing: scale 64");
    check(gc.runtime.guest_cycle_scale_gate.size() == 1 &&
          gc.runtime.guest_cycle_scale_gate[0].addr == 0x800AC794u &&
          gc.runtime.guest_cycle_scale_gate[0].value == 0x180u &&
          gc.runtime.guest_cycle_scale_gate[0].size == 4u &&
          gc.runtime.guest_cycle_scale_gate[0].mask == 0xFFFFFFFFu,
          "timing: single inline-table gate with defaults");
    fs::remove(p);

    p = write_game_toml("ves_timing_arr.toml",
        "[timing]\nguest_cycle_scale = 8\nguest_cycle_scale_gated = true\n"
        "guest_cycle_scale_gate = [ { addr = 0x800AC794, value = 0x180 },\n"
        "  { addr = 0x00010003, size = 1, mask = 0x0F, value = 5 } ]\n");
    gc = PSXRecompV4::load_game_config(p);
    check(gc.runtime.guest_cycle_scale_gate.size() == 2 &&
          gc.runtime.guest_cycle_scale_gate[1].size == 1u &&
          gc.runtime.guest_cycle_scale_gate[1].mask == 0x0Fu &&
          gc.runtime.guest_cycle_scale_gated, "timing: gate array + mod gate");
    fs::remove(p);

    p = write_game_toml("ves_timing_bad.toml",
        "[timing]\nguest_cycle_scale_gate = { addr = 0x1F801070, value = 1 }\n");
    bool rejected = false;
    try { (void)PSXRecompV4::load_game_config(p); } catch (const std::exception&) { rejected = true; }
    check(rejected, "timing: non-RAM gate address rejected");
    fs::remove(p);

    /* Oversized and negative values must be rejected, not wrapped to 32 bits. */
    const struct { const char* body; const char* what; } bad[] = {
        { "guest_cycle_scale_gate = { addr = 0x100010000, value = 1 }", "oversized addr" },
        { "guest_cycle_scale_gate = { addr = -4, value = 1 }", "negative addr" },
        { "guest_cycle_scale_gate = { addr = 0x80010000, value = 0x100000001 }", "oversized value" },
        { "guest_cycle_scale_gate = { addr = 0x80010000, value = -1 }", "negative value" },
        { "guest_cycle_scale_gate = { addr = 0x80010000, value = 1, size = 0x100000004 }", "oversized size" },
        { "guest_cycle_scale_gate = { addr = 0x80010000, value = 1, size = -4 }", "negative size" },
        { "guest_cycle_scale_gate = { addr = 0x80010000, value = 1, mask = 0x1FFFFFFFF }", "oversized mask" },
        { "guest_cycle_scale_gate = { addr = 0x80010000, value = 1, mask = -1 }", "negative mask" },
        { "guest_cycle_scale = 0", "scale 0" },
        { "guest_cycle_scale = 65", "scale 65" },
        { "guest_cycle_scale = -8", "negative scale" },
        { "guest_cycle_scale = 0x100000008", "oversized scale" },
    };
    for (const auto& b : bad) {
        p = write_game_toml("ves_timing_range.toml", std::string("[timing]\n") + b.body + "\n");
        rejected = false;
        try { (void)PSXRecompV4::load_game_config(p); } catch (const std::exception&) { rejected = true; }
        check(rejected, (std::string("timing: rejected ") + b.what).c_str());
        fs::remove(p);
    }
}

int main() {
    test_internal_resolution_game_toml();
    test_internal_resolution_settings();
    test_defaults_off();
    test_game_toml_opt_in();
    test_game_window_width_validation();
    test_knobs_independent();
    test_user_settings_read();
    test_user_settings_absent_key();
    test_user_settings_round_trip();
    test_user_settings_fov_scale();
    test_texture_window_batching();
    test_pgxp_title_keys();
    test_timing_gate();

    if (failures) {
        std::fprintf(stderr, "video_enhancement_settings_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("PASS: [video] geometry_correction / perspective_texturing "
                "plumbing\n");
    return 0;
}
