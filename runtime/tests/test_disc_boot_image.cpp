// The boot executable is found on the disc where SYSTEM.CNF says it is, also
// in a subdirectory. Rhapsody (MARL/), Policenauts (NAUTS/) and
// Wild Arms (EXE/) keep it there; a root-only lookup left the text image guard
// without its reference image.
#include "disc_boot_image.h"
#include "native_bios_boot.h"
#include "native_bios_services.h"
#include "cpu_state.h"
#include "psx_memory.h"
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

uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1;
static std::vector<uint8_t> guest_ram(PSX_MAIN_RAM_RETAIL_BYTES, 0x5a);
static size_t guest_writes;

extern "C" void psx_host_write_byte(uint32_t address, uint8_t value) {
    guest_ram.at(psx_ram_canonical_offset(address)) = value;
    ++guest_writes;
}

extern "C" void psx_host_write_word(uint32_t address, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        psx_host_write_byte(address + i, static_cast<uint8_t>(value >> (i * 8)));
}

static uint32_t ram_word(uint32_t address) {
    const uint32_t base = psx_ram_canonical_offset(address);
    uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i)
        word |= uint32_t(guest_ram.at(base + i)) << (i * 8);
    return word;
}

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

    // The last sector of the EXE has only three file bytes.
    const fs::path launch = root / "launch.iso";
    auto launch_image = exe(7, 4099);
    auto word = [&](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            launch_image[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    word(0x10, 0xa0010020);  // PC uses a different alias of the text.
    word(0x14, 0x80011004);
    word(0x18, 0x80010000);
    word(0x1c, 4096);
    word(0x28, 0x80012000);
    word(0x2c, 1024);
    word(0x30, 0x801fff00);
    word(0x34, 32);
    write_iso(launch, {{"", "GAME.EXE", launch_image}});
    expect("partial last sector", launch, "GAME.EXE", "GAME.EXE", &launch_image);
    {
        PS1::ISOReader reader;
        PS1::ISOFileEntry entry;
        if (reader.Open(launch.string()) && reader.FindFile("GAME.EXE", entry)) {
            uint8_t bytes[10] = {};
            const uint32_t offset = 2048 * 3 - 2;
            const size_t got = reader.ReadFile(entry, offset, bytes, sizeof(bytes));
            check(got == 5 && std::memcmp(bytes, launch_image.data() + offset, got) == 0,
                  "resolved range spans sectors and stops at file end");
            check(reader.ReadFile(entry, entry.size, bytes, sizeof(bytes)) == 0,
                  "resolved range starting at EOF is empty");
        } else {
            check(false, "launch fixture mounts and resolves");
        }
    }
    std::optional<PsxDiscBootHeader> header;
    uint32_t len = 0;
    std::string name;
    auto read_header = [&](const fs::path &disc) {
        uint8_t *payload = psx_read_disc_boot_image("GAME.EXE", disc.string(), &len, &name, &header);
        const bool readable = payload && len == launch_image.size() - 2048 &&
                              std::memcmp(payload, launch_image.data() + 2048, len) == 0;
        std::free(payload);
        return readable;
    };
    check(read_header(launch) && header && header->entry_pc == 0xa0010020 &&
          header->global_pointer == 0x80011004 && header->text_address == 0x80010000 &&
          header->text_size == 4096 && header->fill_address == 0x80012000 &&
          header->fill_size == 1024 && header->stack_start == 0x801fff00 &&
          header->stack_size == 32, "reads the launch fields separately from trailing payload bytes");

    struct InvalidHeader {
        size_t field;
        uint32_t invalid;
        uint32_t restore;
        const char *message;
    };
    for (const auto &invalid : {
        InvalidHeader{0x10, 0x80011000, 0xa0010020, "entry lies beyond the declared text"},
        InvalidHeader{0x1c, 8192, 4096, "declared text exceeds the readable payload"},
        InvalidHeader{0x28, 0x807ffff0, 0x80012000, "fill crosses the RAM window"},
        InvalidHeader{0x30, 0xfffffff0, 0x801fff00, "stack does not map to RAM"},
    }) {
        word(invalid.field, invalid.invalid);
        const fs::path bad_launch = root / "bad-launch.iso";
        write_iso(bad_launch, {{"", "GAME.EXE", launch_image}});
        check(read_header(launch) && header, "has a previous successful launch header");
        check(read_header(bad_launch) && !header, invalid.message);
        word(invalid.field, invalid.restore);
    }

    const fs::path native = root / "native.iso";
    write_iso(native, {{"", "SYSTEM.CNF", text(
        "BOOT=cdrom:GAME.EXE;1\r\n"
        "tcb = 5\r\nEVENT=0x12 ; eighteen events\r\n"
        "STACK = 801FEF80\r\nTCB=FF\r\n")},
        {"", "GAME.EXE", launch_image}});
    CPUState launch_cpu{};
    std::string boot_error;
    check(psx_prepare_native_bios_boot(launch_cpu, native.string(), boot_error),
          "native boot prepares the original executable and kernel tables");
    check(launch_cpu.pc == 0xa0010020 && launch_cpu.gpr[28] == 0x80011004 &&
          launch_cpu.gpr[29] == 0x801fef80 && launch_cpu.gpr[30] == 0x801fef80 &&
          launch_cpu.gpr[4] == 1 && launch_cpu.gpr[5] == 0 &&
          launch_cpu.gpr[31] == PSX_NATIVE_BOOT_RETURN,
          "native entry registers follow header and SYSTEM.CNF");
    check(ram_word(PSX_NATIVE_C0_TABLE + 6 * 4) == (0x80000000u | PSX_NATIVE_EXCEPTION_ENTRY) &&
          ram_word(PSX_NATIVE_A0_TABLE + 0x2a * 4) == psx_native_bios_stub(&psx_native_bios_tables[0], 0x2a) &&
          ram_word(PSX_NATIVE_EXCEPTION_ENTRY) == PSX_NATIVE_EXCEPTION_MARKER,
          "native launch publishes real guest vector tables and their dispatch entries");
    check(((ram_word(PSX_NATIVE_EXCEPTION_ENTRY + 0x70) & 0xffffu) << 16) +
          (ram_word(PSX_NATIVE_EXCEPTION_ENTRY + 0x74) & 0xffffu) ==
          PSX_NATIVE_CARD_FASTTRACK_SCRATCH &&
          (PSX_NATIVE_EXCEPTION_STACK & 0x1fffffffu) < 0xdf80,
          "native card patch anchors resolve to writable storage outside the exception stack");
    const uint32_t threads = ram_word(0x110);
    const uint32_t events = ram_word(0x120);
    check(ram_word(0x104) == 4 * 8 && ram_word(0x10c) == 4 &&
          ram_word(0x114) == 5 * 0xc0 && ram_word(0x124) == 18 * 0x1c &&
          ram_word(ram_word(0x108)) == threads &&
          ram_word(threads) == 0x4000 && ram_word(threads + 0xc0) == 0x1000 &&
          ram_word(events) == 0xf0000003u && ram_word(events + 4) == 0x2000 &&
          ram_word(events + 8) == 0x10 && ram_word(events + 5 * 0x1c + 4) == 0,
          "native tables expose counts, active thread, five boot CD events, and free slots");
    check(std::memcmp(guest_ram.data() + 0x10000, launch_image.data() + 2048, 4096) == 0 &&
          guest_ram[0x11000] == 0x5a && ram_word(0x12000) == 0 &&
          ram_word(0x123fc) == 0 && guest_ram[0x12400] == 0x5a,
          "native boot writes exactly the declared text and BSS spans");
    const fs::path invalid_native = root / "invalid-native.iso";
    write_iso(invalid_native, {{"", "SYSTEM.CNF", text("BOOT=cdrom:GAME.EXE;1\nSTACK=80000004")},
                              {"", "GAME.EXE", launch_image}});
    const size_t writes_before = guest_writes;
    check(!psx_prepare_native_bios_boot(launch_cpu, invalid_native.string(), boot_error) &&
          guest_writes == writes_before && launch_cpu.pc == 0xa0010020,
          "invalid native stack is rejected before any guest mutation");

    const fs::path default_boot = root / "default-boot.iso";
    write_iso(default_boot, {{"", "PSX.EXE", launch_image}});
    check(psx_prepare_native_bios_boot(launch_cpu, default_boot.string(), boot_error) &&
          launch_cpu.gpr[29] == 0x801fff00 && ram_word(0x114) == 4 * 0xc0 &&
          ram_word(0x124) == 16 * 0x1c,
          "default boot executable and kernel configuration work without SYSTEM.CNF");

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
