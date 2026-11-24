/* disc_boot_image.cpp — see disc_boot_image.h. */
#include "disc_boot_image.h"

#include "iso_reader.h"
#include "psx_memory.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kExeHeaderSize = 2048;
constexpr uint32_t kPhysicalAddressMask = 0x1fffffffu;

uint32_t read_le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool in_main_ram_window(uint32_t address, uint32_t size) {
    const uint32_t segment = address & ~kPhysicalAddressMask;
    if (segment != 0 && segment != 0x80000000u && segment != 0xa0000000u) return false;
    const uint32_t start = address & kPhysicalAddressMask;
    return start < PSX_MAIN_RAM_WINDOW_BYTES && size <= PSX_MAIN_RAM_WINDOW_BYTES - start;
}

std::optional<PsxDiscBootHeader> decode_boot_header(const uint8_t *bytes, size_t available) {
    if (std::memcmp(bytes, "PS-X EXE", 8) != 0) return std::nullopt;

    const PsxDiscBootHeader header{
        read_le32(bytes + 0x10), read_le32(bytes + 0x14),
        read_le32(bytes + 0x18), read_le32(bytes + 0x1c),
        read_le32(bytes + 0x28), read_le32(bytes + 0x2c),
        read_le32(bytes + 0x30), read_le32(bytes + 0x34),
    };
    if (header.text_size < sizeof(uint32_t) || header.text_size > available ||
        !in_main_ram_window(header.text_address, header.text_size) ||
        !in_main_ram_window(header.entry_pc, sizeof(uint32_t)) ||
        (header.entry_pc & 3u) != 0) {
        return std::nullopt;
    }

    const uint32_t pc = header.entry_pc & kPhysicalAddressMask;
    const uint32_t start = header.text_address & kPhysicalAddressMask;
    if (pc < start || pc - start > header.text_size - sizeof(uint32_t) ||
        (header.fill_size && !in_main_ram_window(header.fill_address, header.fill_size)) ||
        (header.stack_start && !in_main_ram_window(header.stack_start, header.stack_size))) {
        return std::nullopt;
    }
    return header;
}

/* The path after `cdrom:` in SYSTEM.CNF's BOOT line, as the disc reader takes
 * it: no leading separator, no `;1`, `/` between a directory and the file.
 * `BOOT = cdrom:\MARL\SLUS_010.73;1` gives `MARL/SLUS_010.73`. */
std::string boot_path_from_system_cnf(PS1::ISOReader &iso) {
    uint8_t cnf[2048] = {0};
    size_t n = iso.ReadFile("SYSTEM.CNF", cnf, sizeof(cnf) - 1);
    if (n == 0) return std::string();
    std::string text((const char *)cnf, n);
    std::string lower = text;
    for (char &c : lower) c = (char)std::tolower((unsigned char)c);
    const size_t key = lower.find("cdrom:");
    if (key == std::string::npos) return std::string();
    size_t j = key + 6;
    while (j < text.size() && (text[j] == '\\' || text[j] == '/')) j++;
    std::string tok;
    while (j < text.size()) {
        char c = text[j];
        if (c == ';' || c == '\r' || c == '\n' || c == ' ' ||
            c == '\t' || c == '\0') break;
        tok += (c == '\\') ? '/' : c; j++;
        if (tok.size() > 64) break;
    }
    return tok;
}

std::string last_component(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/* `DIR/FILE` from the end of a host path, or empty when it has no directory. */
std::string last_two_components(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash == 0) return std::string();
    const size_t before = path.find_last_of("/\\", slash - 1);
    const size_t start = (before == std::string::npos) ? 0 : before + 1;
    if (start >= slash) return std::string();
    return path.substr(start, slash - start) + "/" + path.substr(slash + 1);
}

}  // namespace

uint8_t *psx_read_disc_boot_image(const std::string &exe_path,
                                  const std::string &disc_path,
                                  uint32_t *out_len,
                                  std::string *out_name,
                                  std::optional<PsxDiscBootHeader> *out_header) {
    if (out_header) out_header->reset();
    if (disc_path.empty()) return nullptr;
    PS1::ISOReader iso;
    if (!iso.Open(disc_path)) return nullptr;

    /* Where the file can be, first hit wins:
     *   1. the basename of the game.toml exe field at the disc root (it names
     *      the same file the recompiler consumed, which came off this disc);
     *   2. the exe field's last directory and name, for a config that names a
     *      file in a directory of the disc;
     *   3. the SYSTEM.CNF BOOT path with its directory: a boot file outside
     *      the root (Rhapsody MARL\, Policenauts NAUTS\, Wild Arms EXE\), and
     *      a config whose local name differs from the disc name;
     *   4. the BOOT path's basename at the root, as before the directory was
     *      kept.
     * The reader lists one directory level, so a deeper BOOT path finds nothing. */
    std::string boot_name;
    PS1::ISOFileEntry ent;
    auto select = [&](const std::string &candidate) {
        if (candidate.empty() || !iso.FindFile(candidate, ent) || ent.is_directory)
            return false;
        boot_name = candidate;
        return true;
    };
    if (exe_path.empty() ||
        (!select(last_component(exe_path)) && !select(last_two_components(exe_path)))) {
        const std::string boot = boot_path_from_system_cnf(iso);
        if (!select(boot) && boot.find('/') != std::string::npos)
            select(last_component(boot));
    }
    if (boot_name.empty() || ent.size <= kExeHeaderSize) return nullptr;

    uint8_t header[kExeHeaderSize];
    if (iso.ReadFile(ent, 0, header, sizeof(header)) != sizeof(header)) return nullptr;

    const uint32_t size = ent.size - kExeHeaderSize;
    uint8_t *image = static_cast<uint8_t *>(std::malloc(size));
    if (!image) return nullptr;

    const size_t got = iso.ReadFile(ent, kExeHeaderSize, image, size);
    if (got == 0) {
        std::free(image);
        return nullptr;
    }

    if (out_header) *out_header = decode_boot_header(header, got);
    *out_len = static_cast<uint32_t>(got);
    *out_name = boot_name;
    return image;
}
