// Raw (2352) vs cooked (2048) sector geometry. The cue's TRACK mode decides;
// a bare file is raw when sector 0 starts with the CD sync pattern. File
// length is never the evidence: the Legend of Dragoon disc 1 dump is a raw
// MODE2/2352 image with 384 trailing bytes whose length is a multiple of 2048,
// and a length test mounted it as a cooked ISO (PVD read back as zeros, BIOS
// SystemError B/906).

#include "iso_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static const uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

// Raw MODE2 sectors: sync + header + 8-byte subheader, user data = marker i+1.
static void write_raw(const fs::path& path, uint32_t sectors, size_t trailing) {
    std::ofstream out(path, std::ios::binary);
    for (uint32_t i = 0; i < sectors; ++i) {
        std::vector<uint8_t> s(2352, 0);
        std::memcpy(s.data(), kSync, sizeof(kSync));
        s[15] = 2;
        std::memset(s.data() + 24, (int)(i + 1), 2048);
        out.write(reinterpret_cast<const char*>(s.data()), (std::streamsize)s.size());
    }
    std::vector<uint8_t> tail(trailing, 0xEE);
    out.write(reinterpret_cast<const char*>(tail.data()), (std::streamsize)tail.size());
}

// Cooked sectors; sector 0 is an all-zero system-area sector, others = i+1.
static void write_cooked(const fs::path& path, uint32_t sectors) {
    std::ofstream out(path, std::ios::binary);
    for (uint32_t i = 0; i < sectors; ++i) {
        std::vector<uint8_t> s(2048, i == 0 ? 0 : (uint8_t)(i + 1));
        out.write(reinterpret_cast<const char*>(s.data()), (std::streamsize)s.size());
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

static void check_markers(const std::string& image, uint32_t sectors, bool first_is_zero) {
    PS1::ISOReader r;
    CHECK(r.Open(image));
    CHECK(r.GetSectorCount() == sectors);
    uint8_t buf[2048];
    for (uint32_t i = 0; i < sectors; ++i) {
        CHECK(r.ReadSector(i, buf));
        const uint8_t want = (i == 0 && first_is_zero) ? 0 : (uint8_t)(i + 1);
        if (buf[0] != want || buf[2047] != want) {
            std::printf("  %s sector %u: got %02x want %02x\n", image.c_str(), i, buf[0], want);
            ++g_failures;
        }
    }
    CHECK(!r.ReadSector(sectors, buf));
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "psxrecomp-iso-geometry-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // 20 raw sectors + 64 trailing bytes = 47104 = 23 * 2048: a length test
    // calls this a cooked ISO.
    const fs::path odd = dir / "odd.bin";
    write_raw(odd, 20, 64);
    CHECK(fs::file_size(odd) % 2048 == 0);
    CHECK(fs::file_size(odd) % 2352 != 0);

    // Cue declares MODE2/2352: raw.
    write_text(dir / "odd.cue", "FILE \"odd.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    check_markers((dir / "odd.cue").string(), 20, false);
    bool raw = false;
    CHECK(PS1::ISOReader::FirstFileIsRaw((dir / "odd.cue").string(), &raw) && raw);

    // Bare: sector-0 sync says raw.
    check_markers(odd.string(), 20, false);
    raw = false;
    CHECK(PS1::ISOReader::FirstFileIsRaw(odd.string(), &raw) && raw);

    // A cooked ISO whose length is a multiple of BOTH 2048 and 2352
    // (147 * 2048 = 128 * 2352): no sync at sector 0, so cooked.
    const fs::path both = dir / "both.iso";
    write_cooked(both, 147);
    CHECK(fs::file_size(both) % 2352 == 0);
    check_markers(both.string(), 147, true);
    raw = true;
    CHECK(PS1::ISOReader::FirstFileIsRaw(both.string(), &raw) && !raw);

    // Cue MODE1/2048 over a cooked file: cooked.
    write_text(dir / "both.cue", "FILE \"both.iso\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n");
    check_markers((dir / "both.cue").string(), 147, true);

    // Tracks of one FILE disagreeing on sector size: refuse.
    write_text(dir / "conflict.cue",
               "FILE \"odd.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
               "  TRACK 02 MODE1/2048\n    INDEX 01 00:00:10\n");
    {
        PS1::ISOReader r;
        CHECK(!r.Open((dir / "conflict.cue").string()));
        CHECK(!PS1::ISOReader::FirstFileIsRaw((dir / "conflict.cue").string(), &raw));
    }

    // MODE2/2336 has no sync/header; not mountable here: refuse, never misread.
    write_text(dir / "m2336.cue", "FILE \"odd.bin\" BINARY\n  TRACK 01 MODE2/2336\n    INDEX 01 00:00:00\n");
    {
        PS1::ISOReader r;
        CHECK(!r.Open((dir / "m2336.cue").string()));
    }

    fs::remove_all(dir);
    if (g_failures) {
        std::printf("iso_reader_geometry_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("iso_reader_geometry_test: PASS\n");
    return 0;
}
