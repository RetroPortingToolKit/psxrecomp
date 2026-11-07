#pragma once
#include "mod_plugins.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PSXOpenXRStats {
    uint32_t compiled, enabled, initialized, running, state, tracking, frame_open;
    int32_t result, last_failure_result;
    const char *last_failure;
    uint64_t view_flags;
    const char *stage;
    uint64_t waits, submitted, empty, failures, predicted_time;
    uint64_t session_generation, submitted_session_generation;
    float ipd_m;
    float origin_up_dot;
    uint32_t origin_inversion_streak, origin_ambiguous;
    double units_per_meter;
    uint64_t gl_version, min_gl_version, max_gl_version;
    int64_t swapchain_format; /* GL_SRGB8_ALPHA8 preferred; GL_RGBA8 decoded copy */
    char runtime[128];
    float pose[2][7], fov[2][4];
    PSXModRenderView view[2];
    uint64_t submitted_pair_id, submitted_guest_cycle;
    uint64_t submitted_predicted_time;
    uint32_t submitted_layer; /* 1 projection, 2 head-relative quad */
    uint64_t quad_submitted;
    double quad_distance_m, quad_width_m, quad_height_m; /* last submitted quad */
    uint32_t submitted_source; /* 1 fresh stereo pair, 2 native presentation */
    uint64_t native_submitted, submitted_native_frame;
    uint64_t losses; /* session/instance/swapchain losses torn down and retried */
    uint32_t submitted_surfaces;
    uint64_t surface_uploads, surface_failures;
} PSXOpenXRStats;
int psx_openxr_enable(int enabled);
int psx_openxr_begin(int width,int height,double units);
int psx_openxr_view(uint32_t eye,PSXModRenderView *out);
/* copy callback: blit this pair's eye to runtime GL texture. */
typedef int (*PSXOpenXRCopy)(uint32_t eye,uint32_t texture,int w,int h);
int psx_openxr_end(int keep,PSXOpenXRCopy copy);
int psx_openxr_quad(double distance_m,double width_m,double height_m);
int psx_openxr_surface_valid(const PSXModOpenXRSurface *surface);
int psx_openxr_surface(const PSXModOpenXRSurface *surface);
int psx_openxr_surface_get(uint32_t id,PSXModOpenXRSurface *out);
void psx_openxr_pair_metadata(uint64_t pair_id,uint64_t cycle);
void psx_openxr_native_metadata(uint64_t frame);
/* Pure gate: presentation-side frame interpolation must be OFF while a headset
 * session is running (the compositor reprojects). 0 for every non-XR state. */
int psx_openxr_interp_gate(uint32_t compiled,uint32_t enabled,uint32_t initialized,uint32_t running);
int psx_openxr_session_active(void); /* gate applied to the live state */
void psx_openxr_shutdown(void);
void psx_openxr_recenter(void);
void psx_openxr_stats(PSXOpenXRStats *out);
int psx_openxr_input(PSXModOpenXRInput *out);
void psx_openxr_input_snapshot(PSXModOpenXRInput *out);
int psx_openxr_hands(PSXModOpenXRHands *out);
int psx_openxr_input_override(const PSXModOpenXRInput *input);
int psx_openxr_hands_override(const PSXModOpenXRHands *hands);
#ifdef __cplusplus
}
#endif
