/* disc_boot_image.cpp — see disc_boot_image.h. */
#include "disc_boot_image.h"

#include "iso_reader.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

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
                                  std::string *out_name) {
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
    std::vector<std::string> candidates;
    if (!exe_path.empty()) {
        candidates.push_back(last_component(exe_path));
        const std::string nested = last_two_components(exe_path);
        if (!nested.empty()) candidates.push_back(nested);
    }
    const std::string boot = boot_path_from_system_cnf(iso);
    if (!boot.empty()) {
        candidates.push_back(boot);
        if (boot.find('/') != std::string::npos)
            candidates.push_back(last_component(boot));
    }

    std::string boot_name;
    PS1::ISOFileEntry ent;
    for (const std::string &candidate : candidates) {
        if (!candidate.empty() && iso.FindFile(candidate, ent) && !ent.is_directory) {
            boot_name = candidate;
            break;
        }
    }
    if (boot_name.empty() || ent.size <= 2048) return nullptr;

    uint8_t *file = (uint8_t *)std::malloc(ent.size);
    if (!file) return nullptr;
    size_t got = iso.ReadFile(boot_name, file, ent.size);
    if (got > 2048) {
        uint32_t img_len = (uint32_t)(got - 2048);
        uint8_t *img = (uint8_t *)std::malloc(img_len);
        if (img) {
            memcpy(img, file + 2048, img_len);
            std::free(file);
            *out_len = img_len;
            *out_name = boot_name;
            return img;
        }
    }
    std::free(file);
    return nullptr;
}
