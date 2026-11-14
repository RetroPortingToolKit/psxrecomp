// The boot executable is found on the disc where SYSTEM.CNF says it is, also
// in a subdirectory. Rhapsody (MARL/), Policenauts (NAUTS/) and
// Wild Arms (EXE/) keep it there; a root-only lookup left the text image guard
// without its reference image.
#include "disc_boot_image.h"
#include "iso_reader.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures;

static void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    } else {
        std::cout << "ok: " << message << '\n';
    }
}

struct DiscFile {
    std::string dir;   // "" for the root
    std::string name;  // without the ";1"
    std::vector<uint8_t> bytes;
};

// A PS-X EXE of header + `body` bytes; `seed` makes each file's body its own.
static std::vector<uint8_t> exe(uint8_t seed, size_t body = 4096) {
    std::vector<uint8_t> out(2048 + body);
    std::memcpy(out.data(), "PS-X EXE", 8);
    for (size_t i = 0; i < body; i++) out[2048 + i] = (uint8_t)(seed + i * 7);
    return out;
}

static std::vector<uint8_t> text(const std::string &s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// A 2048-byte-per-sector ISO9660 image: root directory at LBA 20, one sector
// per subdirectory from LBA 21, file data from LBA 40.
static void write_iso(const fs::path &path, const std::vector<DiscFile> &files) {
    std::vector<uint8_t> iso(200 * 2048);
    auto le32 = [&](size_t at, uint32_t n) {
        for (unsigned i = 0; i < 4; ++i) iso[at + i] = (uint8_t)(n >> (i * 8));
    };
    auto record = [&](size_t at, uint32_t lba, uint32_t bytes, bool directory,
                      const std::string &name) -> size_t {
        const size_t len = (33 + name.size() + 1) & ~size_t(1);
        iso[at] = (uint8_t)len;
        le32(at + 2, lba); le32(at + 10, bytes);
        iso[at + 25] = directory ? 2 : 0;
        iso[at + 28] = 1; iso[at + 31] = 1; iso[at + 32] = (uint8_t)name.size();
        std::copy(name.begin(), name.end(), iso.begin() + at + 33);
        return len;
    };
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    iso[16 * 2048 + 6] = 1;
    record(16 * 2048 + 156, 20, 2048, true, std::string(1, '\0'));

    std::vector<std::string> dirs;
    for (const DiscFile &f : files) {
        bool known = f.dir.empty();
        for (const std::string &d : dirs) known = known || d == f.dir;
        if (!known) dirs.push_back(f.dir);
    }
    size_t root_at = 20 * 2048;
    std::vector<size_t> dir_at;
    for (size_t i = 0; i < dirs.size(); i++) {
        root_at += record(root_at, (uint32_t)(21 + i), 2048, true, dirs[i]);
        dir_at.push_back((21 + i) * 2048);
    }
    uint32_t lba = 40;
    for (const DiscFile &f : files) {
        size_t *at = &root_at;
        for (size_t i = 0; i < dirs.size(); i++)
            if (dirs[i] == f.dir) at = &dir_at[i];
        *at += record(*at, lba, (uint32_t)f.bytes.size(), false, f.name + ";1");
        std::copy(f.bytes.begin(), f.bytes.end(), iso.begin() + (size_t)lba * 2048);
        lba += (uint32_t)((f.bytes.size() + 2047) / 2048);
    }
    std::ofstream out(path, std::ios::binary);
    out.write((const char *)iso.data(), (std::streamsize)iso.size());
}

// One lookup: the name the function reports and whether the image is `want`'s body.
static void expect(const std::string &what, const fs::path &iso,
                   const std::string &exe_path, const std::string &want_name,
                   const std::vector<uint8_t> *want) {
    uint32_t len = 0;
    std::string name;
    uint8_t *img = psx_read_disc_boot_image(exe_path, iso.string(), &len, &name);
    if (!want) {
        check(img == nullptr, what + ": no image");
        std::free(img);
        return;
    }
    const bool same = img && len == want->size() - 2048 &&
                      std::memcmp(img, want->data() + 2048, len) == 0;
    check(same, what + ": the image is the boot file's body");
    check(img && name == want_name,
          what + ": found as " + want_name + (img ? " (got " + name + ")" : ""));
    std::free(img);
}

int main(int argc, char **argv) {
    const fs::path root = (argc > 1 ? fs::path(argv[1]) : fs::current_path()) /
                          "disc_boot_image_scratch";
    fs::remove_all(root);
    fs::create_directories(root);

    // --- boot file at the disc root: as before ------------------------------
    const auto root_exe = exe(1);
    const fs::path at_root = root / "root.iso";
    write_iso(at_root, {{"", "SYSTEM.CNF", text("BOOT = cdrom:\\SCUS_944.23;1\r\nTCB = 4\r\n")},
                        {"", "SCUS_944.23", root_exe}});
    expect("root, exe names the file", at_root, "disc/SCUS_944.23", "SCUS_944.23", &root_exe);
    expect("root, local name differs", at_root, "work\\GAME.EXE", "SCUS_944.23", &root_exe);
    expect("root, no exe field", at_root, "", "SCUS_944.23", &root_exe);

    // --- boot file in a subdirectory (Rhapsody: MARL\SLUS_010.73) -----------
    const auto marl_exe = exe(2);
    const fs::path marl = root / "marl.iso";
    write_iso(marl, {{"", "SYSTEM.CNF", text("BOOT = cdrom:\\MARL\\SLUS_010.73;1\r\nTCB = 4\r\n")},
                     {"MARL", "SLUS_010.73", marl_exe},
                     {"MARL", "DATA.BIN", text("not the boot file")}});
    {
        // The fixture has the fault's shape: nothing with that name at the root.
        PS1::ISOReader reader;
        PS1::ISOFileEntry entry;
        check(reader.Open(marl.string()), "subdirectory fixture mounts");
        check(!reader.FindFile("SLUS_010.73", entry), "the boot file is not at the root");
        check(reader.FindFile("MARL/SLUS_010.73", entry) && entry.size == marl_exe.size(),
              "the boot file is in MARL");
    }
    expect("subdirectory, exe names the file", marl, "disc/SLUS_010.73", "MARL/SLUS_010.73", &marl_exe);
    expect("subdirectory, no exe field", marl, "", "MARL/SLUS_010.73", &marl_exe);

    // --- the BOOT line's other spellings ------------------------------------
    const auto nauts_exe = exe(3);
    const fs::path nauts = root / "nauts.iso";
    write_iso(nauts, {{"", "SYSTEM.CNF", text("BOOT=cdrom:NAUTS\\SLPS_002.16;1\r\n")},
                      {"NAUTS", "SLPS_002.16", nauts_exe}});
    // A continuation disc: the build's exe is disc 1's name, this disc has its own.
    expect("subdirectory, no leading separator, other disc's name", nauts,
           "disc/SLPS_002.15", "NAUTS/SLPS_002.16", &nauts_exe);

    const auto lower_exe = exe(4);
    const fs::path lower = root / "lower.iso";
    write_iso(lower, {{"", "SYSTEM.CNF", text("boot = CDROM:/exe/scus_946.08;1\n")},
                      {"EXE", "SCUS_946.08", lower_exe}});
    expect("subdirectory, forward slashes and lower case", lower,
           "disc/SCUS_946.08", "exe/scus_946.08", &lower_exe);

    // --- game.toml exe names a disc path (a program of a set, not the BOOT file)
    const auto launch_exe = exe(5), prog_exe = exe(6);
    const fs::path set = root / "set.iso";
    write_iso(set, {{"", "SYSTEM.CNF", text("BOOT = cdrom:\\LAUNCH.EXE;1\r\n")},
                    {"", "LAUNCH.EXE", launch_exe},
                    {"BIN", "PROG2.EXE", prog_exe}});
    expect("exe names a disc path", set, "work/BIN/PROG2.EXE", "BIN/PROG2.EXE", &prog_exe);
    expect("exe names a root file of a set", set, "work/LAUNCH.EXE", "LAUNCH.EXE", &launch_exe);

    // --- no image -------------------------------------------------------------
    const fs::path deep = root / "deep.iso";
    write_iso(deep, {{"", "SYSTEM.CNF", text("BOOT = cdrom:\\A\\B\\GAME.EXE;1\r\n")},
                     {"A", "OTHER.BIN", text("the reader lists one directory level")}});
    expect("two directory levels are not read", deep, "disc/GAME.EXE", "", nullptr);

    const fs::path small = root / "small.iso";
    write_iso(small, {{"", "SYSTEM.CNF", text("BOOT = cdrom:\\DIR\\TINY.EXE;1\r\n")},
                      {"DIR", "TINY.EXE", std::vector<uint8_t>(2048, 0)}});
    expect("a file of only a header", small, "disc/TINY.EXE", "", nullptr);

    const fs::path bare = root / "bare.iso";
    write_iso(bare, {{"", "README.TXT", text("no SYSTEM.CNF")}});
    expect("no SYSTEM.CNF and no file", bare, "disc/SLUS_010.73", "", nullptr);
    expect("no such image", root / "missing.iso", "disc/SLUS_010.73", "", nullptr);
    expect("no disc path", fs::path(), "disc/SLUS_010.73", "", nullptr);

    fs::remove_all(root);
    std::cout << (failures ? "FAILED: " : "passed, failures: ") << failures << '\n';
    return failures ? 1 : 0;
}
