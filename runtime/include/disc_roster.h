#pragma once

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace PSXRecompV4 {
inline std::string disc_roster_fold(std::string value) {
    for (char& c : value) c = (char)std::toupper((unsigned char)c);
    return value;
}

// Keep directories in the identity. Same-directory container alternatives
// retain the existing CUE/BIN behavior; two cache directories never collide.
inline std::string disc_roster_key(std::filesystem::path path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (!ec) path = absolute;
    path = path.lexically_normal();
    path.replace_extension();
    std::string key = path.generic_string();
#ifdef _WIN32
    key = disc_roster_fold(key);
#endif
    return key;
}

inline int disc_roster_index(const std::vector<std::filesystem::path>& roster,
                             const std::filesystem::path& disc) {
    if (disc.empty()) return -1;
    const auto key = disc_roster_key(disc);
    for (size_t i = 0; i < roster.size(); ++i)
        if (disc_roster_key(roster[i]) == key) return (int)i;

    // Relocated copies remain supported only when the name identifies one
    // roster entry. An ambiguous basename provides no disc identity.
    const auto stem = disc_roster_fold(disc.stem().string());
    int match = -1;
    for (size_t i = 0; i < roster.size(); ++i) {
        if (disc_roster_fold(roster[i].stem().string()) != stem) continue;
        if (match != -1) return -1;
        match = (int)i;
    }
    return match;
}

inline std::filesystem::path disc_roster_selected(
    const std::vector<std::filesystem::path>& roster, int selected_1based,
    const std::filesystem::path& persisted) {
    if (roster.size() < 2) return persisted;
    const int idx = selected_1based - 1;
    if (idx < 0 || idx >= (int)roster.size()) return persisted;
    return disc_roster_index(roster, persisted) == idx ? persisted : roster[idx];
}

inline std::string disc_roster_value(
    const std::vector<std::filesystem::path>& roster,
    const std::vector<std::string>& values, const std::filesystem::path& disc,
    const std::string& fallback) {
    const int idx = disc_roster_index(roster, disc);
    return idx >= 0 && (size_t)idx < values.size() && !values[idx].empty()
        ? values[idx] : fallback;
}
} // namespace PSXRecompV4
