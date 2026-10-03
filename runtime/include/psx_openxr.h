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
    float ipd_m;
    double units_per_meter;
    uint64_t gl_version, min_gl_version, max_gl_version;
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
} PSXOpenXRStats;
int psx_openxr_enable(int enabled);
int psx_openxr_begin(int width,int height,double units);
int psx_openxr_view(uint32_t eye,PSXModRenderView *out);
/* copy callback: blit this pair's eye to runtime GL texture. */
typedef int (*PSXOpenXRCopy)(uint32_t eye,uint32_t texture,int w,int h);
int psx_openxr_end(int keep,PSXOpenXRCopy copy);
int psx_openxr_quad(double distance_m,double width_m,double height_m);
void psx_openxr_pair_metadata(uint64_t pair_id,uint64_t cycle);
void psx_openxr_native_metadata(uint64_t frame);
void psx_openxr_shutdown(void);
void psx_openxr_recenter(void);
void psx_openxr_stats(PSXOpenXRStats *out);
int psx_openxr_input(PSXModOpenXRInput *out);
void psx_openxr_input_snapshot(PSXModOpenXRInput *out);
int psx_openxr_hands(PSXModOpenXRHands *out);
#ifndef PSX_NO_DEBUG_TOOLS
int psx_openxr_input_override(const PSXModOpenXRInput *input);
int psx_openxr_hands_override(const PSXModOpenXRHands *hands);
#endif
#ifdef __cplusplus
}
#endif
