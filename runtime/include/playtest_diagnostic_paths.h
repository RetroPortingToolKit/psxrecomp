#pragma once
/* The runtime root is preferred. These paths are a host-only fallback and
 * never alter settings, saves, cards or other persistent runtime locations. */
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

inline std::filesystem::path psx_playtest_diagnostic_executable() {
#ifdef _WIN32
    std::array<wchar_t,32768> path{};
    DWORD n=GetModuleFileNameW(nullptr,path.data(),(DWORD)path.size());
    if(!n || n>=path.size())throw std::runtime_error("cannot locate executable");
    return std::filesystem::path(path.data());
#elif defined(__linux__)
    return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(__APPLE__)
    std::array<char,32768> path{};
    uint32_t size=(uint32_t)path.size();
    if(_NSGetExecutablePath(path.data(),&size))throw std::runtime_error("cannot locate executable");
    return std::filesystem::weakly_canonical(path.data());
#else
    return {};
#endif
}

inline std::filesystem::path psx_playtest_diagnostic_application_directory() {
    const auto executable=psx_playtest_diagnostic_executable();
    return executable.empty()?std::filesystem::current_path():executable.parent_path();
}

inline std::string psx_playtest_diagnostic_utf8(const std::filesystem::path& path) {
    const auto bytes=path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}

inline std::filesystem::path psx_playtest_diagnostic_session_directory(
    const std::filesystem::path& application, const std::string& game,
    const std::string& session) {
    // GAME_ID is validated by CMake; keep direct consumers inside the temp root.
    std::string safe;
    for(char c:game) safe += ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
                             (c>='0'&&c<='9')||c=='-'||c=='_')?c:'_';
    std::error_code error;
    auto folder=application/"saves"/"diagnostics"/session;
    if(std::filesystem::create_directories(folder,error))return folder;
    folder=std::filesystem::temp_directory_path()/"psxrecomp-diagnostics"/safe/session;
    error.clear();
    if(std::filesystem::create_directories(folder,error))return folder;
    throw std::runtime_error("session directory unavailable in application and temporary directories");
}
