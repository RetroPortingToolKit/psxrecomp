#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace PSXRecompV4 {

/* Canonical bytes are an immutable, engine-owned snapshot. Plugins never
 * reopen the selected file after verification. The PSX domain contains the
 * first data track's 2048-byte sector payloads, excluding CD audio. The
 * psx-mode2 domain instead retains 2336 bytes per raw Mode-2 sector, including
 * XA subheaders and Form-2 audio/video; cooked ISO cannot represent it. */
bool load_mod_media(const std::filesystem::path& path,
                    const std::string& format, uint64_t expected_size,
                    const std::string& expected_sha256,
                    std::shared_ptr<const std::vector<uint8_t>>& bytes,
                    std::string* error = nullptr);

/* Bounded ISO9660 access to an already verified immutable psx-mode2 snapshot.
 * Does not reopen paths or touch the guest drive. Stream files expose their
 * raw extent; read() returns ordinary 2048-byte file data (e.g. ARC/EXE). */
class ModMode2Disc {
public:
    ModMode2Disc(const uint8_t* bytes, uint64_t size):bytes_(bytes),size_(size) {}
    bool find(const std::string& path,uint32_t& lba,uint32_t& size) const;
    bool read(const std::string& path,std::vector<uint8_t>& out) const;
private:
    const uint8_t* bytes_; uint64_t size_;
};

} // namespace PSXRecompV4
