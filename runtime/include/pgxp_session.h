#ifndef PSXRECOMP_PGXP_SESSION_H
#define PSXRECOMP_PGXP_SESSION_H

/*
 * Which PGXP corrections a session arms (docs/ENHANCEMENTS.md G1.11/G1.12).
 *
 * main.cpp drives the whole lifecycle through the two functions at the end of
 * this header, compiled from src/pgxp_session.cpp, which
 * runtime/tests/test_pgxp_session.cpp links and drives the same way:
 *
 *   reset_mod_owned_presentation()  -> psx_pgxp_session_reset()
 *   mod_runtime_activate_plugins()  -> psx.enhancement.pgxp's activation
 *                                      records pgxp_mod_request(...)
 *   renderer setup (session_reboot) -> psx_pgxp_session_arm(&config, ...)
 *
 * The arm TAKES the mod request (reads and clears it), so a request lives only
 * from one session's activation to that session's arming. A later session
 * with an empty plan -- netplay clears it, or the player disabled the mod --
 * cannot inherit it, even if the reset were skipped.
 *
 * Inputs, in order of precedence:
 *   1. env_*: the validation overrides PSX_GEOMETRY_CORRECTION,
 *      PSX_PERSPECTIVE_TEXTURING, PSX_PGXP_CPU_MODE and PSX_PGXP_CULLING (-1
 *      when unset, else 0 or 1). They win over everything, so an A/B run can
 *      switch PGXP off under an enabled mod.
 *   2. video_* OR mod_*: the [video] baseline (game.toml, then settings.toml
 *      and the launcher) and the psx.enhancement.pgxp request this session's
 *      activation recorded. Either one arms a correction. With mod_only
 *      ([video] pgxp_mod_only, a title that ships PGXP through the mod) the
 *      baseline is ignored and the mod is the one switch.
 *
 * The mod arms both geometry and texture correction; its CPU-mode option adds
 * tier-2 propagation and its culling option precise culling. Precise culling
 * changes guest-visible NCLIP results, so it has no [video] key (netplay does
 * not clear [video]); it needs geometry correction. A netplay session never
 * arms it, whatever the mod, the env override or the plan say: a published
 * netplay plan can carry the mod (content negotiation, MOD_PACKAGES.md), and
 * the shadows it reads are host-only state that a rollback load drops on one
 * peer and not the other, so the guest's NCLIP results would fork.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PSXPgxpSessionInputs {
    int video_geometry;
    int video_texture;
    int video_cpu_mode;
    int env_geometry;
    int env_texture;
    int env_cpu_mode;
    int env_culling;
    int mod_enabled;
    int mod_cpu_mode;
    int mod_culling;
    int mod_only;
    int netplay;
} PSXPgxpSessionInputs;

typedef struct PSXPgxpSessionArm {
    int geometry;
    int texture;
    int cpu_mode;
    int culling;
} PSXPgxpSessionArm;

static inline int psx_pgxp_session_pick(int env, int baseline, int mod) {
    if (env >= 0) return env ? 1 : 0;
    return (baseline || mod) ? 1 : 0;
}

static inline PSXPgxpSessionArm psx_pgxp_session_resolve(
        const PSXPgxpSessionInputs* in) {
    PSXPgxpSessionArm arm;
    const int use_video = in->mod_only ? 0 : 1;
    arm.geometry = psx_pgxp_session_pick(in->env_geometry,
                                         use_video && in->video_geometry,
                                         in->mod_enabled);
    arm.texture = psx_pgxp_session_pick(in->env_texture,
                                        use_video && in->video_texture,
                                        in->mod_enabled);
    arm.cpu_mode = psx_pgxp_session_pick(in->env_cpu_mode,
                                         use_video && in->video_cpu_mode,
                                         in->mod_enabled && in->mod_cpu_mode);
    arm.culling = !in->netplay && arm.geometry &&
                  psx_pgxp_session_pick(in->env_culling, 0,
                                        in->mod_enabled && in->mod_culling);
    return arm;
}

/* Parse one of the env overrides: unset -> -1, "0" or empty -> 0, else 1
 * (the rule the runtime has always used for them). */
static inline int psx_pgxp_session_env_flag(const char* value) {
    if (!value) return -1;
    return (*value && *value != '0') ? 1 : 0;
}

/* The [video] PGXP settings main.cpp hands the arming: the player's baseline
 * (game.toml, then settings.toml and the launcher) and the title's tuning
 * keys. */
typedef struct PSXPgxpSessionConfig {
    int video_geometry;
    int video_texture;
    int video_cpu_mode;
    float tolerance;
    int position_fallback;
    /* Set only for explicit [video] values. Otherwise the runtime chooses
     * no clamp / dataflow-only in hook builds, 0.5 / cache in base builds. */
    int tolerance_set;
    int position_fallback_set;
    int preserve_projection;
    int mod_only;
    int netplay;   /* this session is a netplay match: precise culling off */
} PSXPgxpSessionConfig;

/* Session start, before plugin activation: forget any mod request. */
void psx_pgxp_session_reset(void);

/* The renderer setup's arming, once per session after activation: reads the
 * env overrides, takes the mod request, resolves, and applies the result to
 * the engine (gte_geometry_correction_set, gpu_texture_correction_set,
 * pgxp_set_cpu_mode / _culling) along with the tuning keys. `used` (may be
 * NULL) receives the inputs it resolved from. */
PSXPgxpSessionArm psx_pgxp_session_arm(const PSXPgxpSessionConfig* config,
                                       PSXPgxpSessionInputs* used);

#ifdef __cplusplus
}
#endif

#endif
