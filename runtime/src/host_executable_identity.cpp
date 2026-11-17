#include "host_executable_identity.h"
#include "psx_sha256_file.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
bool image_digest(uint8_t out[32]) {
#if defined(_WIN32)
    // Windows keeps an executing image mapped against in-place writes. Ask
    // the loader for that image, independent of shortcuts and caller cwd.
    std::vector<wchar_t> path(512);
    for (;;) {
        const DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if (!n) return false;
        if (n<path.size())
            return psx_sha256_file(std::filesystem::path(std::wstring(path.data(),n)),out)==PsxFileHashResult::Ok;
        if (path.size()>=32768) return false;
        path.resize(path.size()*2);
    }
#elif defined(__linux__)
    // Open this procfs anchor directly: resolving its symlink to a pathname
    // would hash a replacement if the executable was renamed/unlinked.
    return psx_sha256_file(std::filesystem::path("/proc/self/exe"),out)==PsxFileHashResult::Ok;
#else
    // No verified image anchor has been qualified here. Keep the portable
    // build functional, but never substitute a guessed argv/cwd identity.
    (void)out;
    return false;
#endif
}
}

extern "C" int psx_running_executable_sha256(uint8_t out[32]) {
    if (!out) return 0;
    static std::once_flag once;
    static std::array<uint8_t,32> digest{};
    static bool valid=false;
    try {
        std::call_once(once,[] {
            try { valid=image_digest(digest.data()); }
            catch (...) { valid=false; }
        });
    } catch (...) { return 0; }
    if (!valid) return 0;
    std::memcpy(out,digest.data(),digest.size());
    return 1;
}
