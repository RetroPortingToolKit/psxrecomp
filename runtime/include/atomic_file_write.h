#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace psx {

/* Publish a complete file by writing a same-directory temporary and replacing
 * the destination atomically. The destination is never observed half-written. */
inline bool atomic_write_file(const std::filesystem::path& target,
                              std::string_view data,
                              std::string* error = nullptr) {
    if (target.empty() || target.filename().empty()) {
        if (error) *error = "destination has no filename";
        return false;
    }
    auto temporary = target;
#ifdef _WIN32
    temporary += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
#else
    temporary += "." + std::to_string(getpid()) + ".tmp";
#endif
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        out.close();
        if (!out) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            if (error) *error = "temporary write failed";
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error) *error = "atomic replace failed";
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error) *error = ec.message();
        return false;
    }
#endif
    return true;
}

} // namespace psx
