#pragma once
#include <filesystem>
#include <string>
#include <sstream>
#include <cwchar>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif
namespace PSXRecompV4 {
// A failed identity is never a cache hit; callers must verify content instead.
inline bool host_file_identity(const std::filesystem::path& path, std::string& identity) {
    identity.clear();
#if defined(_WIN32)
    // NTFS change/write timestamps can collide across immediate rewrites,
    // including same-size writes with a restored mtime. A file USN alone also
    // cannot qualify persistent receipts: it may be zero or reset with its
    // journal, and the query is unavailable over SMB3. Until a journal-bound
    // change identity is qualified, retain the portable full-verification path.
    (void)path;
    return false;
#else
    std::ostringstream out;
    struct stat info{};
    if (stat(path.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_ino == 0) return false;
    out << "posix1:" << info.st_dev << ':' << info.st_ino << ':' << info.st_size;
#if defined(__APPLE__)
    out << ':' << info.st_mtimespec.tv_sec << ':' << info.st_mtimespec.tv_nsec
        << ':' << info.st_ctimespec.tv_sec << ':' << info.st_ctimespec.tv_nsec;
#else
    out << ':' << info.st_mtim.tv_sec << ':' << info.st_mtim.tv_nsec
        << ':' << info.st_ctim.tv_sec << ':' << info.st_ctim.tv_nsec;
#endif
    identity = out.str();
    return true;
#endif
}
}
