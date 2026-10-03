#include "projection_scale_config.hpp"
#include "projection_scale.hpp"
#include "toml.hpp"
#include <stdexcept>

double psx_projection_scale_load_config(const std::filesystem::path& path) {
    const auto config = toml::parse(path.string());
    if (!config.contains("video")) return 1.0;
    const auto& video = toml::find(config, "video");
    if (!video.contains("fov_scale")) return 1.0;
    const double value = toml::find<double>(video, "fov_scale");
    if (!psx_projection_scale_valid(value))
        throw std::runtime_error("[video] fov_scale must be > 0 and <= 8 (1.0 = faithful)");
    return value;
}
