#pragma once

#include <filesystem>
#include <string>

namespace psx_path {
// C++20 returns char8_t here; the runtime's narrow interfaces still use UTF-8
// bytes. Do not use path::string(), which may select a Windows locale encoding.
inline std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
}
