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
#elif defined(__APPLE__)
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
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
#elif defined(__APPLE__)
    // macOS lets a running image be renamed, or unlinked and replaced. Ask
    // the kernel which vnode backs dyld's main executable header, open the
    // path it reports and accept the descriptor only if it is that vnode. The
    // bytes at the mapped file offset must also be the loaded header and load
    // commands (LC_UUID included). A universal file is hashed whole, as the
    // other platforms hash the whole image file.
    const auto* image=reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    if (!image||image->magic!=MH_MAGIC_64||image->filetype!=MH_EXECUTE) return false;
    const uint64_t address=reinterpret_cast<uintptr_t>(image);
    proc_regionwithpathinfo region{};
    if (proc_pidinfo(getpid(),PROC_PIDREGIONPATHINFO,address,&region,sizeof region)!=sizeof region)
        return false;
    const proc_regioninfo& mapping=region.prp_prinfo;
    const uint64_t into=address-mapping.pri_address;
    const size_t header=sizeof *image+image->sizeofcmds;
    if (address<mapping.pri_address||into>=mapping.pri_size||header>mapping.pri_size-into)
        return false;
    region.prp_vip.vip_path[sizeof region.prp_vip.vip_path-1]=0;
    const int fd=open(region.prp_vip.vip_path,O_RDONLY|O_CLOEXEC);
    if (fd<0) return false;
    struct Descriptor { int fd; ~Descriptor() { close(fd); } } held{fd};
    struct stat file{};
    const vinfo_stat& mapped=region.prp_vip.vip_vi.vi_stat;
    if (fstat(fd,&file)||!S_ISREG(file.st_mode)||
        static_cast<uint32_t>(file.st_dev)!=mapped.vst_dev||
        static_cast<uint64_t>(file.st_ino)!=mapped.vst_ino) return false;
    std::vector<uint8_t> loaded(header);
    const ssize_t got=pread(fd,loaded.data(),header,static_cast<off_t>(mapping.pri_offset+into));
    if (got!=static_cast<ssize_t>(header)||std::memcmp(loaded.data(),image,header)) return false;
    // Opening /dev/fd/N duplicates this verified descriptor (fd(4)); it does
    // not look the executable's pathname up again.
    return psx_sha256_file(std::filesystem::path("/dev/fd/"+std::to_string(fd)),out)==PsxFileHashResult::Ok;
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
