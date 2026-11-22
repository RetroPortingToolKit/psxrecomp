/* test_pgxp_session.cpp -- which PGXP corrections does a session arm?
 * (docs/ENHANCEMENTS.md G1.11/G1.12)
 *
 * PGXP is a player setting (settings.toml: video.geometry_correction,
 * video.perspective_texturing, video.pgxp_cpu_mode, video.pgxp_culling). A
 * title mod may still ask for it at activation (pgxp_mod_request); the session
 * arming (src/pgxp_session.cpp, the code main.cpp calls) takes that request
 * and combines it with the [video] baseline and the player's settings, in
 * main.cpp's order:
 *
 *   reset_mod_owned_presentation()  -> psx_pgxp_session_reset()
 *   mod_runtime_activate_plugins()  -> a title mod may call pgxp_mod_request
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
/* Side-effect-free peek (memory.c); host-output observation reads. */
extern "C" int psx_peek_word(uint32_t, uint32_t* out) { *out = 0; return 0; }
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
/* This fixture installs presentation policy; it never dispatches guest code,
 * credits guest cycles, or installs an audio callback. Fail on accidental use. */
extern "C" uint32_t psx_peek_word_untraced(uint32_t a) { return psx_read_word(a); }
extern "C" {
uint64_t psx_cycle_count=0, psx_next_service_cycle=0;
int psx_in_device_service=0, g_event_step_conservative=0, g_ls_replay_active=0;
uint32_t g_psx_cyc_batch=0, g_psx_cyc_batch_limit=0;
void psx_devices_service_to_now(void) { std::abort(); }
void psx_advance_cycles_slow(uint32_t) { std::abort(); }
}
extern "C" void psx_dispatch_call(CPUState*, uint32_t, uint32_t) { std::abort(); }
extern "C" void spu_set_pcm_overlay(PSXModPCMCallback callback) { if (callback) std::abort(); }
extern "C" uint32_t psx_mod_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" int psx_ws_x_margin(void) { return 0; }
extern "C" void gpu_get_display_info(GpuDisplayInfo* out) { *out = GpuDisplayInfo{}; }
extern "C" void dirty_ram_mark_executable_range(uint32_t, uint32_t) {}
extern "C" int fntrace_is_game_started(void) { return 1; }
extern "C" void gpu_ws_tag_hud_primitive(uint32_t, int) {}
extern "C" void gpu_ws_tag_hud_prim_projected(uint32_t, int) {}
extern "C" void gpu_ws_set_gte_sxy_tracking(int) {}
extern "C" int gpu_ws_gte_sxy_recent(int32_t, int32_t) { return 0; }
extern "C" void gpu_ws_tag_world_primitive(uint32_t, int) {}
extern "C" void gpu_ws_set_adaptive_backdrop_preload(int) {}
extern "C" void psx_projection_reset_session(void) {}
extern "C" void gpu_ws_set_native_scene_predicate(int (*)(void)) {}
extern "C" int gpu_ws_configured_x_reveal(void) { return 0; }
extern "C" void gpu_ws_set_manual_view_anchor(int, int) {}
extern "C" void gpu_ws_tag_hud_prim(uint32_t, int) {}
extern "C" void gpu_ws_tag_screen_mask_quad(uint32_t) {}
extern "C" void gpu_ws_tag_reveal_edge_quad(uint32_t, int) {}
extern "C" void gpu_ws_tag_radial_screen_mask_quad(uint32_t, float) {}
extern "C" void gpu_ws_tag_feedback_rect(uint32_t) {}
/* mod_runtime.cpp's lobby netplay commit reads the negotiated match caps.
 * This test drives netplay through mod_runtime_clear_for_netplay, so no
 * lobby match is ever negotiated. */
static PsxLobbyMatchCaps no_match_caps;
extern "C" const PsxLobbyMatchCaps* psx_lobby_match_caps(void) { return &no_match_caps; }

/* ---- one session start, as main.cpp sequences it ------------------------- */

static PSXPgxpSessionConfig g_cfg;

static PSXPgxpSessionArm session(bool do_reset = true, bool title_mod = false,
                                 int mod_cpu = 0, int mod_culling = 0) {
    if (do_reset) psx_pgxp_session_reset();   /* reset_mod_owned_presentation */
    if (title_mod) pgxp_mod_request(1, mod_cpu, mod_culling);   /* its activation */
    g_geometry = g_texture = -1;
    return psx_pgxp_session_arm(&g_cfg, nullptr);
}

static bool armed(const PSXPgxpSessionArm& a, int g, int t, int c, int cull) {
    return a.geometry == g && a.texture == t && a.cpu_mode == c &&
           a.culling == cull && g_geometry == g && g_texture == t &&
           pgxp_cpu_mode() == c && pgxp_culling() == cull;
}

int main(void) {
    {
        PSXPgxpSessionInputs in{};
        in.video_geometry = in.video_texture = 1;
        in.env_geometry = in.env_texture = in.env_cpu_mode = in.env_culling = -1;
        in.mod_enabled = in.mod_corrections_set = 1;
        in.mod_texture = 1;
        auto arm = psx_pgxp_session_resolve(&in);
        CHECK(!arm.geometry && arm.texture);
        in.mod_geometry = 1; in.mod_texture = 0;
        arm = psx_pgxp_session_resolve(&in);
        CHECK(arm.geometry && !arm.texture);
    }

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

    /* Nothing asked: the faithful floor, and the tuning keys are applied
     * regardless (they are inert while off). */
    CHECK(armed(session(), 0, 0, 0, 0));
    CHECK(pgxp_tolerance() < 0.0f && pgxp_position_fallback() == 0 &&
          pgxp_preserve_projection() == 1);

    /* A title mod's request arms geometry, texture and its culling option. */
    CHECK(armed(session(true, true, 0, 1), 1, 1, 0, 1));
    CHECK(armed(session(true, true, 1, 0), 1, 1, 1, 0));
    /* The arming took the request: the next session, even with the reset
     * skipped, has nothing to inherit. */
    CHECK(armed(session(false), 0, 0, 0, 0));

    /* Netplay: precise culling never, not even from the env override. */
    g_cfg.netplay = 1;
    CHECK(armed(session(true, true, 0, 1), 1, 1, 0, 0));
    test_setenv("PSX_PGXP_CULLING", "1");
    CHECK(armed(session(true, true, 0, 1), 1, 1, 0, 0));
    test_unsetenv("PSX_PGXP_CULLING");
    g_cfg.netplay = 0;

    /* The player's settings, with no mod at all. */
    g_cfg.player_texture_set = 1;
    g_cfg.video_texture = 1;
    CHECK(armed(session(), 0, 1, 0, 0));
    g_cfg.player_geometry_set = 1;
    g_cfg.video_geometry = 1;
    CHECK(armed(session(), 1, 1, 0, 0));
    g_cfg.player_cpu_mode_set = 1;
    g_cfg.video_cpu_mode = 1;
    CHECK(armed(session(), 1, 1, 1, 0));
    g_cfg.player_culling = 1;
    CHECK(armed(session(), 1, 1, 1, 1));
    /* Culling needs geometry, and a netplay match holds it off. */
    g_cfg.netplay = 1;
    CHECK(armed(session(), 1, 1, 1, 0));
    g_cfg.netplay = 0;
    g_cfg.video_geometry = 0;
    CHECK(armed(session(), 0, 1, 1, 0));
    /* A player's explicit off wins over a title mod's request... */
    CHECK(armed(session(true, true, 0, 0), 0, 1, 1, 0));
    /* ...and an explicit on wins over pgxp_mod_only. */
    g_cfg.mod_only = 1;
    g_cfg.video_geometry = 1;
    CHECK(armed(session(), 1, 1, 1, 1));
    g_cfg.player_geometry_set = g_cfg.player_texture_set = 0;
    g_cfg.player_cpu_mode_set = g_cfg.player_culling = 0;

    /* The [video] baseline without player values: it arms on its own and
     * adds to a mod, unless the title made its mod the one switch. */
    g_cfg.mod_only = 0;
    g_cfg.video_geometry = 0;
    g_cfg.video_texture = 1;
    g_cfg.video_cpu_mode = 0;
    CHECK(armed(session(), 0, 1, 0, 0));
    g_cfg.mod_only = 1;
    CHECK(armed(session(), 0, 0, 0, 0));
    CHECK(armed(session(true, true, 0, 1), 1, 1, 0, 1));
    g_cfg.mod_only = 0;
    g_cfg.video_texture = 0;

    /* Validation env overrides win over everything. Culling needs geometry. */
    test_setenv("PSX_GEOMETRY_CORRECTION", "0");
    test_setenv("PSX_PERSPECTIVE_TEXTURING", "0");
    CHECK(armed(session(true, true, 0, 1), 0, 0, 0, 0));
    test_setenv("PSX_PERSPECTIVE_TEXTURING", "1");
    CHECK(armed(session(true, true, 0, 1), 0, 1, 0, 0));
    test_unsetenv("PSX_GEOMETRY_CORRECTION");
    test_unsetenv("PSX_PERSPECTIVE_TEXTURING");
    test_setenv("PSX_PGXP_CULLING", "0");
    CHECK(armed(session(true, true, 0, 1), 1, 1, 0, 0));
    test_unsetenv("PSX_PGXP_CULLING");
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
