/* disc_boot_image.cpp — see disc_boot_image.h. */
#include "disc_boot_image.h"

#include "iso_reader.h"
#include "psx_memory.h"

#include <cctype>
#include <charconv>
#include <string_view>
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
std::string read_system_cnf(PS1::ISOReader &iso) {
    uint8_t cnf[2048] = {0};
    const size_t n = iso.ReadFile("SYSTEM.CNF", cnf, sizeof(cnf) - 1);
    return std::string(reinterpret_cast<const char *>(cnf), n);
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::optional<PsxDiscSystemConfig> parse_system_config(std::string_view text) {
    PsxDiscSystemConfig config;
    bool seen_threads = false, seen_events = false, seen_stack = false;
    while (!text.empty()) {
        const size_t end = text.find_first_of("\r\n\0", 0, 3);
        const auto line = text.substr(0, end);
        if (end == std::string_view::npos) text = {};
        else text.remove_prefix(end + 1);

        const size_t equal = line.find('=');
        if (equal == std::string_view::npos) continue;
        std::string key(trim(line.substr(0, equal)));
        for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        uint32_t *field = nullptr;
        bool *seen = nullptr;
        if (key == "TCB") { field = &config.threads; seen = &seen_threads; }
        else if (key == "EVENT") { field = &config.events; seen = &seen_events; }
        else if (key == "STACK") { field = &config.stack; seen = &seen_stack; }
        if (!field || *seen) continue;

        auto value = trim(line.substr(equal + 1));
        value = value.substr(0, value.find_first_of("; \t"));
        if (value.substr(0, 2) == "0x" || value.substr(0, 2) == "0X")
            value.remove_prefix(2);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), *field, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            return std::nullopt;
        *seen = true;
    }
    return config;
}

std::string boot_path_from_system_cnf(const std::string &text) {
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
                                  std::optional<PsxDiscBootHeader> *out_header,
                                  std::optional<PsxDiscSystemConfig> *out_config) {
    if (out_header) out_header->reset();
    if (out_config) out_config->reset();
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
    std::string cnf;
    if (out_config) {
        cnf = read_system_cnf(iso);
        *out_config = parse_system_config(cnf);
    }
    if (exe_path.empty() ||
        (!select(last_component(exe_path)) && !select(last_two_components(exe_path)))) {
        if (!out_config) cnf = read_system_cnf(iso);
        const std::string boot = boot_path_from_system_cnf(cnf);
        if (!select(boot) && boot.find('/') != std::string::npos)
            select(last_component(boot));
        if (out_config && boot.empty()) select("PSX.EXE");
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
