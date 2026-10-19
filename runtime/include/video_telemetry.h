#ifndef PSX_VIDEO_TELEMETRY_H
#define PSX_VIDEO_TELEMETRY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { PSX_VIDEO_NONE = 0, PSX_VIDEO_SOFTWARE = 1,
       PSX_VIDEO_OPENGL = 2, PSX_VIDEO_VULKAN = 3 };
typedef struct PsxVideoTelemetry {
    uint64_t presents, present_errors;
    int valid, renderer, internal_scale, present_status_available;
    int source_width, source_height;
    int drawable_width, drawable_height;
    int viewport_width, viewport_height;
} PsxVideoTelemetry;

/* Render-owner publication, before submission. These describe the latest
 * composed image, which can lead actual swaps when presentation is threaded.
 * Zero dimensions mean unavailable, not a zero-sized successful image.
 * A backend has one composing owner at a time. No readbacks or GL syncs. */
void psx_video_telemetry_compose(int renderer, int scale, int source_w, int source_h,
                               int drawable_w, int drawable_h, int viewport_w, int viewport_h);
/* Call after the backend's swap/present API returns. A success means the host
 * API accepted/completed the call, not that a monitor displayed correct pixels.
 * Counts include repeats, overlays and generated frames. */
/* result: 1 accepted, 0 failed, -1 returned but API supplies no status (SDL2). */
void psx_video_telemetry_present(int result);
/* Bounded passive read: valid=0 if a coherent composition was unavailable.
 * Completion/error counters remain available in that case. */
void psx_video_telemetry_snapshot(PsxVideoTelemetry *out);

/* Emulation-owner settings after runtime mod overrides, sampled every VBlank.
 * Actual allocated scale/dimensions come from the separate backend snapshot. */
typedef struct PsxVideoConfigTelemetry {
    int renderer, resolution_preset, aspect_num, aspect_den, widescreen;
} PsxVideoConfigTelemetry;
void psx_video_config_telemetry(PsxVideoConfigTelemetry *out);
/* Queue one composed-output PNG on the existing presenter capture path.
 * Zero means rejected/unavailable. Acceptance is not proof the file was written.
 * Called only for an explicit owner incident, never for every ordinary frame. */
int psx_video_telemetry_capture(const char *path);

#ifdef __cplusplus
}
#endif
#endif
