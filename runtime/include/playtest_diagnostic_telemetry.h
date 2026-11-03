#pragma once
#include <array>
#include <cstdint>

namespace psx_playtest_diagnostic {
// Host evidence only. No game-specific addresses or classification assumptions.
using TelemetryRow = std::array<int64_t,36>;
constexpr const char *TelemetryHeader =
    "sequence,host_elapsed_us,host_interval_us,incident,cd_reading,cd_pending,"
    "cd_lba,cd_data_read,cd_xa,render_passes,render_aborted,render_verify_checks,render_verify_mismatch,"
    "gpu_draw_commands,host_presents,host_present_errors,video_snapshot_valid,video_renderer,"
    "video_resolution_preset,video_aspect_num,video_aspect_den,video_widescreen,"
    "composed_renderer,composed_internal_scale,composed_source_width,composed_source_height,"
    "composed_drawable_width,composed_drawable_height,composed_viewport_width,composed_viewport_height,"
    "render_verify_enabled,render_verify_ever_executed,visual_verify_available,visual_verify_checks,"
    "incident_capture_request,host_present_status_available\n";
}
