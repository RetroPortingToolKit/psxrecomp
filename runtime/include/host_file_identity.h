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
// A failed identity is never a cache hit. Change time protects restored mtimes.
inline bool host_file_identity(const std::filesystem::path& path, std::string& identity) {
    identity.clear();
    std::ostringstream out;
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct HandleScope { HANDLE value; ~HandleScope() { CloseHandle(value); } } handle{file};
    WCHAR filesystem_name[32]{};
    // NTFS/ReFS expose change time and native file identity. For a remote
    // handle also require SMB3+, rather than assuming every NAS is unsupported.
    FILE_REMOTE_PROTOCOL_INFO remote{};
    const bool is_remote = GetFileInformationByHandleEx(file, FileRemoteProtocolInfo,
        &remote, sizeof remote) != FALSE;
    const DWORD protocol_error = is_remote ? ERROR_SUCCESS : GetLastError();
    bool local_handle = false;
    if (!is_remote) {
        // A failed protocol query proves nothing about locality. Resolve the
        // handle itself: mapped drives become UNC paths here and cannot take
        // the local fallback when a remote provider omits protocol metadata.
        WCHAR final_path[32768]{};
        const DWORD length = GetFinalPathNameByHandleW(file, final_path, 32768, FILE_NAME_NORMALIZED);
        if (length >= 7 && length < 32768 && final_path[0] == L'\\' &&
            final_path[1] == L'\\' && final_path[2] == L'?' && final_path[3] == L'\\' &&
            final_path[5] == L':' && final_path[6] == L'\\') {
            WCHAR drive_root[] = {final_path[4], L':', L'\\', L'\0'};
            const UINT drive_type = GetDriveTypeW(drive_root);
            local_handle = drive_type == DRIVE_FIXED || drive_type == DRIVE_REMOVABLE;
        }
    }
    const bool reliable_protocol = is_remote
        ? remote.Protocol == 0x00020000 && remote.ProtocolMajorVersion >= 3 // WNNC_NET_LANMAN
        : local_handle && (protocol_error == ERROR_INVALID_PARAMETER || protocol_error == ERROR_NOT_SUPPORTED);
    const bool reliable_fs = reliable_protocol &&
        GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, nullptr,
                                     filesystem_name, 32) &&
        (wcscmp(filesystem_name, L"NTFS") == 0 || wcscmp(filesystem_name, L"ReFS") == 0);
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    const bool ok = reliable_fs && GetFileInformationByHandle(file, &info) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof basic) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        (info.nFileIndexHigh != 0 || info.nFileIndexLow != 0) &&
        info.dwVolumeSerialNumber != 0 && basic.ChangeTime.QuadPart != 0 &&
        basic.LastWriteTime.QuadPart != 0;
    if (!ok) return false;
    out << "win1:" << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':'
        << info.nFileIndexLow << ':' << info.nFileSizeHigh << ':' << info.nFileSizeLow
        << ':' << basic.LastWriteTime.QuadPart << ':' << basic.ChangeTime.QuadPart;
#else
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
#endif
    identity = out.str();
    return true;
}
}
