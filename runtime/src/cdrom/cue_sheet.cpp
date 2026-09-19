// cue_sheet.cpp — see cue_sheet.h.

#include "cue_sheet.h"
#include "host_path.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace PSXRecompV4 {

namespace {

std::string upper_ascii(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool equal_ci(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i])) return false;
    }
    return true;
}

// Extract the FILE line's payload name and type token. Handles both the quoted
// form (`FILE "game.bin" BINARY`) and the bare form (`FILE game.bin BINARY`)
// that some hand-written cues use.
bool parse_file_line(const std::string& line, size_t file_kw_pos,
                     std::string& out_name, std::string& out_type) {
    const size_t after_kw = file_kw_pos + 4;  // strlen("FILE")
    const size_t q1 = line.find('"', after_kw);
    size_t rest_pos;

    if (q1 != std::string::npos) {
        const size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 <= q1 + 1) return false;
        out_name = line.substr(q1 + 1, q2 - q1 - 1);
        rest_pos = q2 + 1;
    } else {
        std::istringstream iss(line.substr(after_kw));
        if (!(iss >> out_name) || out_name.empty()) return false;
        rest_pos = after_kw;
        // Advance past the name we just consumed so the type token scan below
        // starts after it.
        const size_t name_at = line.find(out_name, after_kw);
        if (name_at != std::string::npos) rest_pos = name_at + out_name.size();
    }

    std::istringstream rest(line.substr(rest_pos));
    out_type.clear();
    rest >> out_type;
    return true;
}

}  // namespace

bool CueSheet::usable() const {
    if (!opened || files.empty()) return false;
    for (const CueFileRef& f : files) {
        if (!f.is_binary || !f.exists) return false;
    }
    return true;
}

bool CueSheet::has_non_binary_file() const {
    for (const CueFileRef& f : files) {
        if (!f.is_binary) return true;
    }
    return false;
}

size_t CueSheet::data_file_index() const {
    for (const CueTrackRef& t : tracks) {
        if (!t.is_audio && t.file_index < files.size()) return t.file_index;
    }
    return 0;
}

bool path_has_extension_ci(const fs::path& p, const char* dotted_ext) {
    return equal_ci(p.extension().string(), std::string(dotted_ext));
}

fs::path resolve_cue_relative_file(const fs::path& dir, const std::string& name) {
    fs::path named(name);
    // Cues written on Windows may use backslashes; std::filesystem on POSIX
    // treats those as ordinary characters, so normalize them first.
    if (name.find('\\') != std::string::npos) {
        std::string slashed = name;
        for (char& c : slashed) if (c == '\\') c = '/';
        named = fs::path(slashed);
    }

    const fs::path literal = PSXRecompV4::host_path_is_absolute(named) ? named : (dir / named);
    std::error_code ec;
    if (fs::exists(literal, ec)) return literal;

    // Case-insensitive fallback: scan the containing directory for a filename
    // that differs only by case.
    const fs::path parent = literal.parent_path();
    const std::string want = literal.filename().string();
    ec.clear();
    if (fs::is_directory(parent, ec)) {
        ec.clear();
        for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
            if (equal_ci(it->path().filename().string(), want)) return it->path();
        }
    }
    return literal;
}

CueSheet parse_cue_sheet(const fs::path& cue_path) {
    CueSheet sheet;

    std::ifstream cue(cue_path);
    if (!cue.is_open()) return sheet;
    sheet.opened = true;

    const fs::path dir = cue_path.parent_path();

    std::string line;
    int      cur_track_num   = -1;
    bool     cur_track_audio = false;
    uint32_t cur_index00     = 0;
    bool     cur_has_index00 = false;

    while (std::getline(cue, line)) {
        // Strip a trailing CR so cues with CRLF endings parse on POSIX.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const std::string upper = upper_ascii(line);

        const size_t file_pos = upper.find("FILE");
        if (file_pos != std::string::npos) {
            std::string name, type;
            if (parse_file_line(line, file_pos, name, type)) {
                CueFileRef ref;
                ref.name      = name;
                ref.is_binary = equal_ci(type, "BINARY");
                ref.path      = resolve_cue_relative_file(dir, name);
                std::error_code ec;
                ref.exists    = fs::exists(ref.path, ec) && !fs::is_directory(ref.path, ec);
                sheet.files.push_back(std::move(ref));
            }
            // A FILE line resets any track state carried from the previous file.
            cur_track_num   = -1;
            cur_has_index00 = false;
            cur_index00     = 0;
            continue;
        }

        int  tn = 0;
        char type_buf[32] = {0};
        if (std::sscanf(upper.c_str(), " TRACK %d %31s", &tn, type_buf) == 2) {
            cur_track_num   = tn;
            cur_track_audio = (std::string(type_buf).find("AUDIO") != std::string::npos);
            cur_index00     = 0;
            cur_has_index00 = false;
            continue;
        }

        int idx = 0, mm = 0, ss = 0, ff = 0;
        if (std::sscanf(upper.c_str(), " INDEX %d %d:%d:%d", &idx, &mm, &ss, &ff) == 4 &&
            cur_track_num >= 1 && !sheet.files.empty()) {
            const uint32_t index_lba = (uint32_t)(((mm * 60 + ss) * 75) + ff);
            if (idx == 0) {
                cur_index00     = index_lba;
                cur_has_index00 = true;
                continue;
            }
            if (idx != 1) continue;
            CueTrackRef t;
            t.number      = cur_track_num;
            t.is_audio    = cur_track_audio;
            t.file_index  = sheet.files.size() - 1;
            t.index01     = index_lba;
            t.index00     = cur_index00;
            t.has_index00 = cur_has_index00;
            sheet.tracks.push_back(t);
            cur_track_num = -1;
        }
    }

    return sheet;
}

}  // namespace PSXRecompV4
