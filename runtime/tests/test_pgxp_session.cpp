/* test_pgxp_session.cpp -- does the psx.enhancement.pgxp mod arm PGXP, and
 * only for the session whose plan holds it? (docs/ENHANCEMENTS.md G1.11/G1.12)
 *
 * The mod's activation runs at session start; main.cpp's renderer setup runs
 * after it and applies the [video] baseline. Before G1.11 the activation
 * armed the corrections directly and the baseline switched them straight back
 * off, so the mod did nothing. Now the activation records a request and the
 * session arming (src/pgxp_session.cpp, the code main.cpp calls) takes it.
 *
 * This test links the real mod runtime and package manager, the real builtin
 * plugin (src/mod_builtin_pgxp.c, registered by its constructor) and the real
 * arming, stages the framework's own builtin manifest (and a title override
 * of it with default_enabled = true, as a title that ships PGXP on by default
 * does), and drives sessions in main.cpp's order:
 *
 *   commit (offline) or mod_runtime_clear_for_netplay (netplay)
 *   reset_mod_owned_presentation()  -> psx_pgxp_session_reset()
 *   mod_runtime_activate_plugins()  -> builtin_pgxp_activate (if planned)
 *   renderer setup                  -> psx_pgxp_session_arm(...)
 *
 * The engine setters the arming calls are stubbed to record what it armed.
 */
#include "mod_runtime.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "pgxp.h"
#include "pgxp_session.h"
#include "psx_lobby_client.h"

#include "cpu_state.h"
#include "gpu.h"
#include "gpu_hd_textures.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/* POSIX setenv/unsetenv are missing on MinGW; an empty _putenv_s value unsets. */
static void test_setenv(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
static void test_unsetenv(const char *name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

#ifndef PSX_BUILTIN_PGXP_MANIFEST
#error "PSX_BUILTIN_PGXP_MANIFEST must name mods/builtin/.../manifest.toml"
#endif

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- what the arming armed (gte.cpp / gpu.c setters, stubbed) ------------ */

static int g_geometry = -1;
static int g_texture = -1;
extern "C" int psx_netplay_active(void) { return 0; }
/* No external HD pack is configured by the PGXP session fixture. */
extern "C" void gpu_hd_textures_shutdown(void) {}
extern "C" int gpu_hd_textures_configure(const char*, int, int, char*, size_t) { return 0; }
extern "C" int gpu_hd_textures_reload(char*, size_t) { return 0; }
extern "C" void gpu_hd_textures_set_dump_enabled(int) {}
extern "C" void gpu_hd_textures_get_diag(GpuHdTextureDiag* out) { *out = GpuHdTextureDiag{}; }
extern "C" void gte_geometry_correction_set(int enabled) { g_geometry = enabled; }
extern "C" void gpu_texture_correction_set(int enabled) { g_texture = enabled; }
/* pgxp.cpp's position-cache tier lives in gte.cpp. */
static bool g_cached_hud_vertex = false;
extern "C" int gte_geometry_correction_lookup(uint32_t, int32_t* x, int32_t* y) {
    if (!g_cached_hud_vertex) return 0;
    *x = (160 << 16) + 0x4000;
    *y = (80 << 16) + 0x4000;
    return 1;
}

/* The session harness has no GTE backend; pre-draw probes must link as a no-op. */
extern "C" int gte_geometry_correction_lookup_probe(uint32_t, int32_t*, int32_t*) {
    return 0;
}

/* ---- the runtime surface mod_runtime.cpp links against ------------------- */

static std::array<uint8_t, 2 * 1024 * 1024> ram;
extern "C" uint8_t psx_read_byte(uint32_t a) { return ram[a & 0x1fffffu]; }
extern "C" void psx_write_byte(uint32_t a, uint8_t v) { ram[a & 0x1fffffu] = v; }
extern "C" uint16_t psx_read_half(uint32_t a) {
    const uint32_t o = a & 0x1fffffu;
    return (uint16_t)(ram[o] | ((uint16_t)ram[o + 1] << 8));
}
extern "C" void psx_write_half(uint32_t a, uint16_t v) {
    const uint32_t o = a & 0x1fffffu;
    ram[o] = (uint8_t)v;
    ram[o + 1] = (uint8_t)(v >> 8);
}
extern "C" uint32_t psx_read_word(uint32_t a) {
    const uint32_t o = a & 0x1fffffu;
    return (uint32_t)ram[o] | ((uint32_t)ram[o + 1] << 8) |
           ((uint32_t)ram[o + 2] << 16) | ((uint32_t)ram[o + 3] << 24);
}
extern "C" void psx_write_word(uint32_t a, uint32_t v) {
    const uint32_t o = a & 0x1fffffu;
    for (int i = 0; i < 4; i++) ram[o + i] = (uint8_t)(v >> (8 * i));
}
extern "C" void psx_host_write_byte(uint32_t a, uint8_t v) { psx_write_byte(a, v); }
extern "C" void psx_host_write_half(uint32_t a, uint16_t v) { psx_write_half(a, v); }
extern "C" void psx_host_write_word(uint32_t a, uint32_t v) { psx_write_word(a, v); }
extern "C" uint32_t psx_mod_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" int psx_ws_x_margin(void) { return 0; }
extern "C" void gpu_get_display_info(GpuDisplayInfo* out) { *out = GpuDisplayInfo{}; }
extern "C" void dirty_ram_mark_executable_range(uint32_t, uint32_t) {}
extern "C" int fntrace_is_game_started(void) { return 1; }
extern "C" void gpu_ws_tag_hud_primitive(uint32_t, int) {}
extern "C" void gpu_ws_tag_world_primitive(uint32_t, int) {}
extern "C" void gpu_ws_set_adaptive_backdrop_preload(int) {}
extern "C" void psx_projection_reset_session(void) {}
extern "C" void gpu_ws_set_native_scene_predicate(int (*)(void)) {}
extern "C" int gpu_ws_configured_x_reveal(void) { return 0; }
extern "C" void gpu_ws_tag_hud_prim(uint32_t, int) {}
extern "C" void gpu_ws_tag_screen_mask_quad(uint32_t) {}
extern "C" void gpu_ws_tag_radial_screen_mask_quad(uint32_t, float) {}
/* mod_runtime.cpp's lobby netplay commit reads the negotiated match caps.
 * This test drives netplay through mod_runtime_clear_for_netplay, so no
 * lobby match is ever negotiated. */
static PsxLobbyMatchCaps no_match_caps;
extern "C" const PsxLobbyMatchCaps* psx_lobby_match_caps(void) { return &no_match_caps; }

/* ---- staging ------------------------------------------------------------- */

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string replace_once(std::string s, const std::string& from,
                                const std::string& to) {
    const size_t at = s.find(from);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

/* A mods root holding the builtin manifest as `manifest` (verbatim, or a
 * title's default-on override of it) and an optional state.toml. */
static fs::path stage(const char* name, const std::string& manifest,
                      const std::string& state) {
    const fs::path root = fs::temp_directory_path() /
                          (std::string("psxrecomp-pgxp-session-") + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    write_text(root / "packages/psx.enhancement.pgxp/1.0.0/manifest.toml",
               manifest);
    if (!state.empty()) write_text(root / "state.toml", state);
    std::vector<char> zeros(8 * 2352, 0);
    write_text(root / "disc.bin", std::string(zeros.begin(), zeros.end()));
    write_text(root / "disc.cue",
               "FILE \"disc.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n");
    return root;
}

static bool commit(const fs::path& root) {
    std::string error;
    if (!PSXRecompV4::mod_runtime_initialize(root, "SLUS-PGXPT", 0x80010000u,
                                             {}, &error)) {
        std::fprintf(stderr, "initialize: %s\n", error.c_str());
        return false;
    }
    if (!PSXRecompV4::mod_runtime_commit(root / "disc.cue", &error)) {
        std::fprintf(stderr, "commit: %s\n", error.c_str());
        return false;
    }
    return true;
}

/* ---- one session start, as main.cpp sequences it ------------------------- */

static PSXPgxpSessionConfig g_cfg;

static PSXPgxpSessionArm session(bool do_reset = true) {
    if (do_reset) psx_pgxp_session_reset();   /* reset_mod_owned_presentation */
    mod_runtime_activate_plugins();
    g_geometry = g_texture = -1;
    return psx_pgxp_session_arm(&g_cfg, nullptr);
}

static bool armed(const PSXPgxpSessionArm& a, int g, int t, int c, int cull) {
    return a.geometry == g && a.texture == t && a.cpu_mode == c &&
           a.culling == cull && g_geometry == g && g_texture == t &&
           pgxp_cpu_mode() == c && pgxp_culling() == cull;
}

static void netplay_clear(void) {
    std::string error;
    CHECK(PSXRecompV4::mod_runtime_clear_for_netplay(&error));
}

int main(void) {
    for (const char* v : {"PSX_GEOMETRY_CORRECTION", "PSX_PERSPECTIVE_TEXTURING",
                          "PSX_PGXP_CPU_MODE", "PSX_PGXP_CULLING"})
        test_unsetenv(v);
    g_cfg = PSXPgxpSessionConfig{};
    /* No title tuning: session arming must choose the build's defaults,
     * including resetting values left by a previous session/debug toggle. */
    pgxp_set_tolerance(2.0f);
    pgxp_set_position_fallback(0);
    CHECK(armed(session(), 0, 0, 0, 0));
#if defined(PSX_PGXP) && PSX_PGXP
    CHECK(pgxp_tolerance() < 0.0f && pgxp_position_fallback() == 0);
#else
    CHECK(pgxp_tolerance() == 0.5f && pgxp_position_fallback() == 1);
#endif
    CHECK(pgxp_preserve_projection() == 0);
    /* Run those defaults through the real engine: a proven world vertex
     * retains its 0.75px fraction, while a HUD coordinate sharing its integer
     * position cannot borrow a different projection from the cache. */
    pgxp_set_enabled(1);
    const uint32_t packed = (80u << 16) | 160u;
    const uint32_t addr = 0x80100000u;
    const int32_t precise_x = (160 << 16) + 0xc000;
    pgxp_gte_push_sxy(precise_x, 80 << 16, 100, packed);
    psx_pgxp_cop2(nullptr, (0x3au << 26) | (14u << 16), packed, addr);
    int32_t x, y;
    uint16_t z;
    const int world = pgxp_get_precise_vertex(addr, packed, 160, 80, &x, &y, &z);
#if defined(PSX_PGXP) && PSX_PGXP
    CHECK(world == PGXP_SRC_DATAFLOW && x == precise_x && z == 100);
#else
    CHECK(world == PGXP_SRC_NATIVE && x == (160 << 16) && z == 100);
#endif
    g_cached_hud_vertex = true;
    const int hud = pgxp_get_precise_vertex(0xffffffffu, packed, 160, 80, &x, &y, &z);
#if defined(PSX_PGXP) && PSX_PGXP
    CHECK(hud == PGXP_SRC_NATIVE && x == (160 << 16) && z == 0);
#else
    CHECK(hud == PGXP_SRC_FALLBACK && x == (160 << 16) + 0x4000 && z == 0);
#endif
    g_cached_hud_vertex = false;
    pgxp_set_enabled(0);
    /* Each explicit compatibility override is independent of the other. */
    g_cfg.tolerance = 0.25f;
    g_cfg.tolerance_set = 1;
    CHECK(armed(session(), 0, 0, 0, 0));
    CHECK(pgxp_tolerance() == 0.25f);
#if defined(PSX_PGXP) && PSX_PGXP
    CHECK(pgxp_position_fallback() == 0);
#else
    CHECK(pgxp_position_fallback() == 1);
#endif
    g_cfg.tolerance_set = 0;
    g_cfg.position_fallback = 1;
    g_cfg.position_fallback_set = 1;
    CHECK(armed(session(), 0, 0, 0, 0));
#if defined(PSX_PGXP) && PSX_PGXP
    CHECK(pgxp_tolerance() < 0.0f);
#else
    CHECK(pgxp_tolerance() == 0.5f);
#endif
    CHECK(pgxp_position_fallback() == 1);
    /* Explicit compatibility tuning wins even in the hook flavor. */
    g_cfg.tolerance = 0.5f;
    g_cfg.tolerance_set = 1;
    g_cfg.position_fallback = 1;
    g_cfg.position_fallback_set = 1;
    CHECK(armed(session(), 0, 0, 0, 0));
    CHECK(pgxp_tolerance() == 0.5f && pgxp_position_fallback() == 1);
    g_cfg.tolerance = -1.0f;
    g_cfg.position_fallback = 0;
    g_cfg.preserve_projection = 1;

    const std::string builtin = read_text(PSX_BUILTIN_PGXP_MANIFEST);
    CHECK(builtin.find("id = \"psx.enhancement.pgxp\"") != std::string::npos);
    CHECK(builtin.find("default_enabled = false") != std::string::npos);
    CHECK(builtin.find("id = \"culling\"") != std::string::npos);
    /* A title that ships PGXP on by default overrides the builtin at the
     * same id and version: the feature on, and here precise culling too. */
    std::string title = replace_once(builtin, "default_enabled = false",
                                     "default_enabled = true");
    {
        const size_t opt = title.find("id = \"culling\"");
        const size_t def = title.find("default = \"false\"", opt);
        CHECK(opt != std::string::npos && def != std::string::npos);
        if (def != std::string::npos)
            title.replace(def, std::strlen("default = \"false\""),
                          "default = \"true\"");
    }

    /* The builtin's default: nothing planned, the faithful floor, and the
     * tuning keys are applied regardless (they are inert while off). */
    CHECK(commit(stage("builtin", builtin, "")));
    CHECK(armed(session(), 0, 0, 0, 0));
    CHECK(pgxp_tolerance() < 0.0f && pgxp_position_fallback() == 0 &&
          pgxp_preserve_projection() == 1);

    /* A title's default-on override: the mod arms geometry, texture and its
     * culling option at the first boot (the G1.11 bug left it all off). */
    const fs::path title_root = stage("title", title, "");
    CHECK(commit(title_root));
    CHECK(armed(session(), 1, 1, 0, 1));

    /* Netplay rematch: the plan is cleared, nothing activates, PGXP is off
     * -- default-on packages included. */
    netplay_clear();
    CHECK(armed(session(), 0, 0, 0, 0));
    /* ...even if the session-start reset were skipped: the offline
     * session's arming took its request, so nothing is left to inherit. */
    CHECK(commit(title_root));
    CHECK(armed(session(), 1, 1, 0, 1));
    netplay_clear();
    CHECK(armed(session(false), 0, 0, 0, 0));

    /* Offline rematch after netplay: on again. */
    CHECK(commit(title_root));
    CHECK(armed(session(), 1, 1, 0, 1));

    /* A netplay session whose published plan carries the mod (content
     * negotiation, mod_runtime_commit_for_netplay): geometry and texture as
     * planned, precise culling never -- not even from the env override --
     * because its guest-visible NCLIP reads host-only shadows that a rollback
     * load drops on one peer only. */
    g_cfg.netplay = 1;
    CHECK(armed(session(), 1, 1, 0, 0));
    test_setenv("PSX_PGXP_CULLING", "1");
    CHECK(armed(session(), 1, 1, 0, 0));
    test_unsetenv("PSX_PGXP_CULLING");
    g_cfg.netplay = 0;
    CHECK(armed(session(), 1, 1, 0, 1));

    /* The player's off switch on the Mods page. */
    const std::string disabled =
        "format_version = 2\n"
        "[[package]]\n"
        "id = \"psx.enhancement.pgxp\"\n"
        "version = \"1.0.0\"\n"
        "[[feature]]\n"
        "package_id = \"psx.enhancement.pgxp\"\n"
        "id = \"pgxp\"\n"
        "enabled = false\n";
    CHECK(commit(stage("title-off", title, disabled)));
    CHECK(armed(session(), 0, 0, 0, 0));

    /* Options: culling off, CPU mode on. */
    const std::string options =
        "format_version = 2\n"
        "[[package]]\n"
        "id = \"psx.enhancement.pgxp\"\n"
        "version = \"1.0.0\"\n"
        "[[feature]]\n"
        "package_id = \"psx.enhancement.pgxp\"\n"
        "id = \"pgxp\"\n"
        "enabled = true\n"
        "[feature.values]\n"
        "culling = false\n"
        "cpu_mode = true\n";
    CHECK(commit(stage("title-options", title, options)));
    CHECK(armed(session(), 1, 1, 1, 0));

    /* [video] baseline: a player's keys work without the mod and add to it;
     * culling has no [video] key. */
    CHECK(commit(stage("title-off2", title, disabled)));
    g_cfg.video_texture = 1;
    CHECK(armed(session(), 0, 1, 0, 0));
    g_cfg.video_geometry = 1;
    CHECK(armed(session(), 1, 1, 0, 0));
    /* ...unless the title made the mod the one switch (pgxp_mod_only): then
     * a stale settings.toml value cannot keep half of PGXP on. */
    g_cfg.mod_only = 1;
    CHECK(armed(session(), 0, 0, 0, 0));
    CHECK(commit(title_root));
    CHECK(armed(session(), 1, 1, 0, 1));
    netplay_clear();
    CHECK(armed(session(), 0, 0, 0, 0));
    g_cfg.mod_only = 0;
    g_cfg.video_texture = 0;
    g_cfg.video_geometry = 0;

    /* Validation env overrides win over both. Culling needs geometry. */
    CHECK(commit(title_root));
    test_setenv("PSX_GEOMETRY_CORRECTION", "0");
    test_setenv("PSX_PERSPECTIVE_TEXTURING", "0");
    CHECK(armed(session(), 0, 0, 0, 0));
    test_setenv("PSX_PERSPECTIVE_TEXTURING", "1");
    CHECK(armed(session(), 0, 1, 0, 0));
    test_unsetenv("PSX_GEOMETRY_CORRECTION");
    test_unsetenv("PSX_PERSPECTIVE_TEXTURING");
    test_setenv("PSX_PGXP_CULLING", "0");
    CHECK(armed(session(), 1, 1, 0, 0));
    test_unsetenv("PSX_PGXP_CULLING");
    CHECK(commit(stage("builtin2", builtin, "")));
    test_setenv("PSX_GEOMETRY_CORRECTION", "1");
    test_setenv("PSX_PGXP_CULLING", "1");
    CHECK(armed(session(), 1, 0, 0, 1));
    test_setenv("PSX_PERSPECTIVE_TEXTURING", "");   /* set but empty = off (Windows: unset, same result) */
    CHECK(armed(session(), 1, 0, 0, 1));
    test_unsetenv("PSX_GEOMETRY_CORRECTION");
    test_unsetenv("PSX_PGXP_CULLING");
    test_unsetenv("PSX_PERSPECTIVE_TEXTURING");

    CHECK(psx_pgxp_session_env_flag(nullptr) == -1);
    CHECK(psx_pgxp_session_env_flag("0") == 0);
    CHECK(psx_pgxp_session_env_flag("") == 0);
    CHECK(psx_pgxp_session_env_flag("1") == 1);
    CHECK(psx_pgxp_session_env_flag("yes") == 1);

    if (g_failures) {
        std::fprintf(stderr, "test_pgxp_session: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("test_pgxp_session: all checks passed\n");
    return 0;
}

/* render_pass.c: no sandboxed local view runs in this test. */
extern "C" int psx_mod_local_view_scope(void) { return 0; }
