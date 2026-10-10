/* pgxp_session.cpp -- the per-session PGXP arming main.cpp runs
 * (docs/ENHANCEMENTS.md G1.11/G1.12). See pgxp_session.h for the sequence.
 * Kept out of main.cpp so runtime/tests/test_pgxp_session.cpp runs exactly
 * this code against the real mod runtime and builtin plugin. */
#include "pgxp_session.h"
#include "pgxp.h"

#include <cstdlib>

extern "C" void gte_geometry_correction_set(int enabled);
extern "C" void gpu_texture_correction_set(int enabled);

extern "C" void psx_pgxp_session_reset(void) {
    pgxp_mod_request(0, 0, 0);
}

extern "C" PSXPgxpSessionArm psx_pgxp_session_arm(
        const PSXPgxpSessionConfig* config, PSXPgxpSessionInputs* used) {
    PSXPgxpSessionInputs in{};
    in.env_geometry =
        psx_pgxp_session_env_flag(std::getenv("PSX_GEOMETRY_CORRECTION"));
    in.env_texture =
        psx_pgxp_session_env_flag(std::getenv("PSX_PERSPECTIVE_TEXTURING"));
    in.env_cpu_mode =
        psx_pgxp_session_env_flag(std::getenv("PSX_PGXP_CPU_MODE"));
    in.env_culling =
        psx_pgxp_session_env_flag(std::getenv("PSX_PGXP_CULLING"));
    in.video_geometry = config->video_geometry ? 1 : 0;
    in.video_texture = config->video_texture ? 1 : 0;
    in.video_cpu_mode = config->video_cpu_mode ? 1 : 0;
    in.mod_only = config->mod_only ? 1 : 0;
    in.netplay = config->netplay ? 1 : 0;
    /* Take, not peek: the request belongs to this session alone. */
    in.mod_enabled = pgxp_mod_request_take(&in.mod_cpu_mode, &in.mod_culling);

    const PSXPgxpSessionArm arm = psx_pgxp_session_resolve(&in);
    /* The hook flavor tracks the full-word dataflow. A coordinate cache can
     * inject unrelated world fractions into HUD vertices, and a tolerance
     * clamp discards valid shadows, mixing precise/native triangle corners.
     * Base builds lack that coverage and retain their validated defaults.
     * Neither default enables the optional PGXP enhancement itself. */
#if defined(PSX_PGXP) && PSX_PGXP
    const float default_tolerance = -1.0f;
    const int default_position_fallback = 0;
#else
    const float default_tolerance = 0.5f;
    const int default_position_fallback = 1;
#endif
    pgxp_set_tolerance(config->tolerance_set ? config->tolerance : default_tolerance);
    pgxp_set_position_fallback(config->position_fallback_set ?
                                config->position_fallback : default_position_fallback);
    pgxp_set_preserve_projection(config->preserve_projection);
    pgxp_set_culling(arm.culling);
    gte_geometry_correction_set(arm.geometry);
    gpu_texture_correction_set(arm.texture);
    pgxp_set_cpu_mode(arm.cpu_mode);
    if (used) *used = in;
    return arm;
}
