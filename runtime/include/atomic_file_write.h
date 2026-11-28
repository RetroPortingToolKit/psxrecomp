#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <recomp_persistence/atomic_file.h>
#include "psx_path_utf8.h"

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
    if (!recomp_atomic_file_store(psx_path::utf8(target).c_str(), data.data(), data.size())) {
        if (error) *error = "atomic file publication failed";
        return false;
    }
    return true;
}

} // namespace psx
