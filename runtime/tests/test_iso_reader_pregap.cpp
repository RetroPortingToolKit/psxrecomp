// Cue PREGAP / POSTGAP are gaps the dump does NOT store. The mount must insert
// them as silent sectors so the disc layout (and the TOC games read through
// GetTD) matches the physical disc. The oracle here is layout equivalence: a
// cue that stores its pregap in the file (INDEX 00) and one that declares it
// with PREGAP describe the same disc, so every track LBA, the sector count and
// every sector's contents must agree.

#include "iso_reader.h"

#include <cstdint>
#include <cstdio>
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

// Sector i of a file is filled with byte `markers[i]` (0 = a silent sector).
static void write_bin(const fs::path& path, const std::vector<uint8_t>& markers) {
    std::ofstream out(path, std::ios::binary);
    for (uint8_t m : markers) {
        std::vector<uint8_t> sector(2352, m);
        out.write(reinterpret_cast<const char*>(sector.data()), sector.size());
    }
}

static void write_text(const fs::path& path, const char* text) {
    std::ofstream out(path);
    out << text;
}

static std::vector<uint8_t> run(uint8_t first, int count) {
    std::vector<uint8_t> v;
    for (int i = 0; i < count; ++i) v.push_back((uint8_t)(first + i));
    return v;
}

static std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Every observable of the two mounts must agree.
static void check_same_disc(const fs::path& a_cue, const fs::path& b_cue) {
    PS1::ISOReader a, b;
    CHECK(a.Open(a_cue.string()));
    CHECK(b.Open(b_cue.string()));
    CHECK(a.TrackCount() == b.TrackCount());
    CHECK(a.GetSectorCount() == b.GetSectorCount());
    for (int t = 1; t <= a.TrackCount(); ++t) {
        CHECK(a.TrackStartLBA(t) == b.TrackStartLBA(t));
        CHECK(a.TrackPregapLBA(t) == b.TrackPregapLBA(t));
        CHECK(a.TrackIsAudio(t) == b.TrackIsAudio(t));
    }
    uint8_t ra[2352], rb[2352];
    for (uint32_t lba = 0; lba < a.GetSectorCount(); ++lba) {
        const bool oa = a.ReadRawSector(lba, ra);
        const bool ob = b.ReadRawSector(lba, rb);
        CHECK(oa && ob);
        if (oa && ob && ra[0] != rb[0]) {
            std::printf("  sector %u differs: %02x vs %02x\n", lba, ra[0], rb[0]);
            ++g_failures;
        }
    }
    CHECK(!a.ReadRawSector(a.GetSectorCount(), ra));
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "psxrecomp-cue-pregap-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Single-file dump shaped like Xi (issue #388): data track, then the first
    // audio track with a 3-sector PREGAP that is not in the file, then an audio
    // track whose pregap IS in the file (INDEX 00).
    const std::vector<uint8_t> data  = run(0x10, 10);  // file sectors 0..9
    const std::vector<uint8_t> aud2  = run(0x40, 3);   // file sectors 10..12
    const std::vector<uint8_t> aud3  = run(0x60, 3);   // file sectors 13..15
    write_bin(dir / "gap.bin", cat(cat(data, aud2), aud3));
    write_text(dir / "gap.cue",
               "FILE \"gap.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "  TRACK 02 AUDIO\n"
               "    PREGAP 00:00:03\n"
               "    INDEX 01 00:00:10\n"
               "  TRACK 03 AUDIO\n"
               "    INDEX 00 00:00:13\n"
               "    INDEX 01 00:00:14\n");

    {
        PS1::ISOReader r;
        CHECK(r.Open((dir / "gap.cue").string()));
        CHECK(r.TrackCount() == 3);
        CHECK(r.TrackStartLBA(1) == 0);
        CHECK(r.TrackPregapLBA(2) == 10);   // the virtual gap starts here
        CHECK(r.TrackStartLBA(2) == 13);    // INDEX 01 moved by the gap
        CHECK(r.TrackPregapLBA(3) == 16);
        CHECK(r.TrackStartLBA(3) == 17);
        CHECK(r.GetSectorCount() == 19);
        uint8_t raw[2352];
        CHECK(r.ReadRawSector(9, raw) && raw[0] == 0x19);
        CHECK(r.ReadRawSector(10, raw) && raw[0] == 0x00 && raw[2351] == 0x00);
        CHECK(r.ReadRawSector(12, raw) && raw[0] == 0x00);
        CHECK(r.ReadRawSector(13, raw) && raw[0] == 0x40);
        CHECK(r.ReadRawSector(18, raw) && raw[0] == 0x62);
        uint8_t user[2048];
        CHECK(r.ReadSector(11, user) && user[0] == 0x00);
    }

    // The same physical disc, with the pregap stored in the file as INDEX 00.
    write_bin(dir / "stored.bin",
              cat(cat(cat(data, std::vector<uint8_t>(3, 0)), aud2), aud3));
    write_text(dir / "stored.cue",
               "FILE \"stored.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 00 00:00:10\n"
               "    INDEX 01 00:00:13\n"
               "  TRACK 03 AUDIO\n"
               "    INDEX 00 00:00:16\n"
               "    INDEX 01 00:00:17\n");
    check_same_disc(dir / "gap.cue", dir / "stored.cue");

    // Multi-file (redump layout) with every audio pregap stripped from its
    // file and declared as PREGAP, versus the redump original, plus a POSTGAP
    // on the data track.
    write_bin(dir / "t1.bin", run(0x10, 4));
    write_bin(dir / "t2_stored.bin", cat(std::vector<uint8_t>(2, 0), run(0x40, 3)));
    write_bin(dir / "t3_stored.bin", cat(std::vector<uint8_t>(2, 0), run(0x60, 2)));
    write_bin(dir / "t2_gap.bin", run(0x40, 3));
    write_bin(dir / "t3_gap.bin", run(0x60, 2));
    write_text(dir / "multi_stored.cue",
               "FILE \"t1.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "FILE \"t2_stored.bin\" BINARY\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 00 00:00:00\n"
               "    INDEX 01 00:00:02\n"
               "FILE \"t3_stored.bin\" BINARY\n"
               "  TRACK 03 AUDIO\n"
               "    INDEX 00 00:00:00\n"
               "    INDEX 01 00:00:02\n");
    write_text(dir / "multi_gap.cue",
               "FILE \"t1.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "FILE \"t2_gap.bin\" BINARY\n"
               "  TRACK 02 AUDIO\n"
               "    PREGAP 00:00:02\n"
               "    INDEX 01 00:00:00\n"
               "FILE \"t3_gap.bin\" BINARY\n"
               "  TRACK 03 AUDIO\n"
               "    PREGAP 00:00:02\n"
               "    INDEX 01 00:00:00\n");
    check_same_disc(dir / "multi_gap.cue", dir / "multi_stored.cue");

    // POSTGAP follows the track's stored data and precedes the next track.
    write_text(dir / "postgap.cue",
               "FILE \"t1.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "    POSTGAP 00:00:02\n"
               "FILE \"t2_gap.bin\" BINARY\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 01 00:00:00\n");
    {
        PS1::ISOReader r;
        CHECK(r.Open((dir / "postgap.cue").string()));
        CHECK(r.TrackPregapLBA(2) == 6);
        CHECK(r.TrackStartLBA(2) == 6);
        CHECK(r.GetSectorCount() == 9);
        uint8_t raw[2352];
        CHECK(r.ReadRawSector(3, raw) && raw[0] == 0x13);
        CHECK(r.ReadRawSector(4, raw) && raw[0] == 0x00);
        CHECK(r.ReadRawSector(5, raw) && raw[0] == 0x00);
        CHECK(r.ReadRawSector(6, raw) && raw[0] == 0x40);
    }

    // No gap lines: the layout is the plain concatenation, as before.
    {
        PS1::ISOReader r;
        CHECK(r.Open((dir / "multi_stored.cue").string()));
        CHECK(r.TrackPregapLBA(2) == 4);
        CHECK(r.TrackStartLBA(2) == 6);
        CHECK(r.TrackPregapLBA(3) == 9);
        CHECK(r.TrackStartLBA(3) == 11);
        CHECK(r.GetSectorCount() == 13);
    }

    fs::remove_all(dir);
    if (g_failures) {
        std::printf("iso_reader_pregap_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("iso_reader_pregap_test: OK\n");
    return 0;
}
