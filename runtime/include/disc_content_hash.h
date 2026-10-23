#pragma once

#include "disc_path.h"
#include "iso_reader.h"
#include "psx_sha256.h"
#include "psx_sha256_file.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace PSXRecompV4 {

// One byte identity for package targets and mandatory original-disc policy:
// CUE hashes its data-track file; CHD hashes every decoded raw 2352-byte sector.
// Host crypto and its portable fallback are shared through psx_sha256_file.
inline bool disc_content_sha256(const std::filesystem::path& path, std::string& out,
                 std::string* error) {
    out.clear();
    if (path.empty()) return true;
    const DiscPathResolution resolved = resolve_disc_path(path);
    const std::filesystem::path input = resolved.data;
    std::string extension = resolved.mount.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".chd") {
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        PS1::ISOReader disc;
        if (!disc.Open(resolved.mount.string())) {
            if (error) *error =
                "cannot decode image fingerprint: " + resolved.mount.string();
            return false;
        }
        std::array<uint8_t, 2352> sector{};
        for (uint32_t lba = 0; lba < disc.GetSectorCount(); ++lba) {
            if (!disc.ReadRawSector(lba, sector.data())) {
                if (error) *error =
                    "cannot finish decoding image fingerprint: " +
                    resolved.mount.string();
                return false;
            }
            psx_sha256_update(&hash, sector.data(), sector.size());
        }
        uint8_t digest[32];
        psx_sha256_final(&hash, digest);
        std::ostringstream text;
        for (uint8_t byte : digest)
            text << std::hex << std::setw(2) << std::setfill('0')
                 << (unsigned)byte;
        out = text.str();
        return true;
    }

    uint8_t digest[32];
    const auto result = psx_sha256_file(input, digest);
    if (result == PsxFileHashResult::OpenError) {
        if (error) *error = "cannot fingerprint image: " + input.string();
        return false;
    }
    if (result != PsxFileHashResult::Ok) {
        if (error) *error =
            "cannot finish fingerprinting image: " + input.string();
        return false;
    }
    std::ostringstream text;
    for (uint8_t byte : digest)
        text << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    out = text.str();
    return true;
}

// Configured once from the title's [game] required_disc_sha256, before any
// launcher/provider call. This is title policy, independent of mod selections.
inline std::vector<std::string>& required_disc_sha256() {
    static std::vector<std::string> hashes;
    return hashes;
}

inline bool required_disc_content_matches(const std::filesystem::path& path,
        const std::vector<std::string>& expected, std::string* error = nullptr) {
    if (expected.empty()) return true;
    std::string actual, detail;
    if (path.empty() || !disc_content_sha256(path, actual, &detail)) {
        if (error) *error = "Cannot verify the required original disc. " +
            (detail.empty() ? std::string("Select the original supported disc image.") : detail);
        return false;
    }
    if (std::find(expected.begin(), expected.end(), actual) != expected.end()) return true;
    if (error) *error = "Unsupported disc image. Use the verified original disc; "
        "apply changes through Mods. Disc SHA-256: " + actual;
    return false;
}

} // namespace PSXRecompV4
