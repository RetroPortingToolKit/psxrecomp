#ifndef PSXRECOMP_MOD_SESSION_BASELINE_H
#define PSXRECOMP_MOD_SESSION_BASELINE_H

/*
 * First-call capture / later-call restore for the scalar host settings a
 * trusted plugin's psx_mod_* setters can change
 * (vsync, frame interpolation and its rate, automatic FMV skipping, and the
 * frame periods native VBlank pacing overrides). reset_mod_owned_presentation()
 * in main.cpp runs this at every session start; it lives here so a unit test
 * (runtime/tests/test_mod_session_baseline.c) drives the same code.
 *
 * The first call records the live values -- settings.toml, environment
 * overrides such as PSX_VSYNC, and the launcher are resolved by then -- and
 * writes nothing back, so the first session keeps exactly what the player
 * configured. Later calls write that record back over the live values.
 * VSync now has a launcher control: its Apply path updates the stored
 * preference before a subsequent session restore, independently of any
 * temporary mod-forced swap mode.
 */

typedef struct PSXModSessionScalars {
    int video_vsync;
    int frame_interpolation;
    int frame_interpolation_fps;
    int auto_skip_fmv;
    double guest_frame_period_ms;
    double frame_period_ms;
} PSXModSessionScalars;

typedef struct PSXModSessionBaseline {
    int captured;
    PSXModSessionScalars values;
} PSXModSessionBaseline;

/*
 * native_vblank_forced: psx_mod_set_native_vblank_rate() owns the frame
 * periods. Only then are they restored; otherwise the video-standard follower
 * owns them (the guest's GP1(08h) mode), and a restore would undo a PAL/NTSC
 * switch. Returns 1 when this call captured (nothing written), 0 when it
 * restored.
 */
static inline int psx_mod_session_baseline_apply(PSXModSessionBaseline* base,
                                                 PSXModSessionScalars* live,
                                                 int native_vblank_forced) {
    if (!base->captured) {
        base->captured = 1;
        base->values = *live;
        return 1;
    }
    live->video_vsync = base->values.video_vsync;
    live->frame_interpolation = base->values.frame_interpolation;
    live->frame_interpolation_fps = base->values.frame_interpolation_fps;
    live->auto_skip_fmv = base->values.auto_skip_fmv;
    if (native_vblank_forced) {
        live->guest_frame_period_ms = base->values.guest_frame_period_ms;
        live->frame_period_ms = base->values.frame_period_ms;
    }
    return 0;
}

#endif
