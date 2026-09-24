// cue_sheet.h — the single cue-sheet parser for the runtime.
//
// A .cue is the authoritative table of contents for a PS1 disc dump: it names
// the backing BINARY file(s) and the TRACK/INDEX layout (data track, CD-DA
// audio tracks, pregaps). Everything that needs to understand a cue goes
// through here so the mount path (iso_reader.cpp), the identity/verify path
// (disc_identity.cpp) and the picked-path resolver (disc_path.cpp) can never
// disagree about what a given cue describes.
//
// Parsing is deliberately permissive about surface syntax (case, quoting,
// leading whitespace) and strict about semantics: a FILE whose type is not
// BINARY is reported rather than guessed at, because WAVE/MP3 payloads have no
// fixed sector geometry and mis-mapping the TOC is worse than refusing the
// image.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PSXRecompV4 {

// One FILE line, resolved against the cue's own directory.
struct CueFileRef {
    std::string           name;            // exactly as written in the cue
    std::filesystem::path path;            // resolved (absolute where possible)
    bool                  is_binary = false;  // FILE type token was BINARY
    bool                  exists    = false;  // resolved path is a real file
};

// One TRACK, with its INDEX times still expressed relative to the OWNING FILE
// (the cue's own convention). Converting to disc-relative LBAs needs each
// file's sector count, which only the mounting code knows.
struct CueTrackRef {
    int      number      = 0;
    bool     is_audio    = false;
    size_t   file_index  = 0;      // index into CueSheet::files
    uint32_t index01     = 0;      // track start, file-relative LBA
    uint32_t index00     = 0;      // pregap start, file-relative LBA
    bool     has_index00 = false;
    // PREGAP / POSTGAP lengths in sectors. Unlike INDEX 00, these gaps are
    // NOT stored in the file: the mount must insert them as silent sectors,
    // shifting every later disc LBA (and the TOC) accordingly.
    uint32_t pregap      = 0;
    uint32_t postgap     = 0;
};

struct CueSheet {
    bool opened = false;   // the .cue itself was readable

    std::vector<CueFileRef>  files;   // in cue order
    std::vector<CueTrackRef> tracks;  // in cue order

    // True when every FILE line named a BINARY payload that exists on disk —
    // i.e. this cue can actually be mounted as written.
    bool usable() const;

    // True when at least one FILE line named a non-BINARY payload.
    bool has_non_binary_file() const;

    // Index of the file backing the first non-audio track, else 0. This is the
    // file that carries SYSTEM.CNF / the ISO9660 header, so it is the one
    // identity and CRC checks must read.
    size_t data_file_index() const;
};

// Case-insensitive extension test. `dotted_ext` includes the dot, e.g. ".cue".
bool path_has_extension_ci(const std::filesystem::path& p, const char* dotted_ext);

// Resolve `name` (as written in a cue) against `dir`. Falls back to a
// case-insensitive directory scan when the literal name does not exist, so a
// redump cue written on Windows still mounts on a case-sensitive filesystem.
// Returns the literal resolution when nothing matches.
std::filesystem::path resolve_cue_relative_file(const std::filesystem::path& dir,
                                                const std::string& name);

// Parse `cue_path`. A sheet whose `opened` is false could not be read at all.
CueSheet parse_cue_sheet(const std::filesystem::path& cue_path);

}  // namespace PSXRecompV4
