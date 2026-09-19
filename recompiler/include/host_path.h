#pragma once

#include <filesystem>
#include <system_error>

namespace PSXRecompV4 {

// Some MinGW libstdc++ builds treat a readable UNC path as drive-relative:
// absolute("\\\\server\\share\\file") becomes "D:\\server\\share\\file".
// Windows already considers UNC and device namespace paths fully qualified.
// Preserve their spelling before asking std::filesystem to anchor a path.
inline bool host_path_is_absolute(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& s = path.native();
    const auto separator = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if (s.size() > 2 && separator(s[0]) && separator(s[1]) && !separator(s[2]))
        return true;
#endif
    return path.is_absolute();
}

inline std::filesystem::path host_absolute(const std::filesystem::path& path,
                                          std::error_code& ec) {
    if (host_path_is_absolute(path)) {
        ec.clear();
        return path;
    }
    return std::filesystem::absolute(path, ec);
}

inline std::filesystem::path host_absolute(const std::filesystem::path& path) {
    if (host_path_is_absolute(path)) return path;
    return std::filesystem::absolute(path);
}

// A fully qualified config value must not be prefixed with its config root.
inline std::filesystem::path host_resolve(const std::filesystem::path& root,
                                         const std::filesystem::path& path) {
    return host_absolute(host_path_is_absolute(path) ? path : root / path);
}

} // namespace PSXRecompV4
