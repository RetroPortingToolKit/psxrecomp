#pragma once
#include <filesystem>

/* Host-only [video] extension. Throws on malformed or out-of-range values. */
double psx_projection_scale_load_config(const std::filesystem::path& path);
