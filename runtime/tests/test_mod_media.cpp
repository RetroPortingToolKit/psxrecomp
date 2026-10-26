#include "mod_media.h"
#include "psx_sha256.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace PSXRecompV4;
static int failures;
static void check(bool ok, const char* text) {
    if (!ok) { std::cerr << "FAIL: " << text << '\n'; ++failures; }
}
static void write(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
static std::string hash(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    std::string result;
    for (uint8_t byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
int main(int argc, char** argv) {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::string error;
    if (argc == 5) {
        const bool ok = load_mod_media(argv[1], argv[2], std::stoull(argv[3]), argv[4], bytes, &error);
        if (!ok) { std::cerr << error << '\n'; return 1; }
        std::cout << "verified bytes=" << bytes->size() << " sha256=" << hash(*bytes) << '\n';
        return 0;
    }
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-media-test";
    fs::create_directories(root);
    std::vector<uint8_t> rom(64);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<uint8_t>(i);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    const auto rom_hash = hash(rom);
    const auto path = root / "rom.z64";
    write(path, rom);
    check(load_mod_media(path, "n64-rom", rom.size(), rom_hash, bytes, &error) && *bytes == rom,
          "canonical N64 bytes");
    auto snapshot = bytes;
    auto swapped = rom;
    for (size_t i = 0; i < swapped.size(); i += 2) std::swap(swapped[i], swapped[i + 1]);
    write(path, swapped);
    check(load_mod_media(path, "n64-rom", rom.size(), rom_hash, bytes, &error) && *bytes == rom,
          "halfword swapped N64 identity");
    swapped = rom;
    for (size_t i = 0; i < swapped.size(); i += 4)
        std::reverse(swapped.begin() + i, swapped.begin() + i + 4);
    write(path, swapped);
    check(load_mod_media(path, "n64-rom", rom.size(), rom_hash, bytes, &error) && *bytes == rom,
          "word swapped N64 identity");
    swapped[32] ^= 1;
    write(path, swapped);
    check(!load_mod_media(path, "n64-rom", rom.size(), rom_hash, bytes, &error) && !bytes,
          "same-size wrong media rejected without exposing bytes");
    check(*snapshot == rom, "snapshot survives on-disk changes");
    check(!load_mod_media(path, "n64-rom", rom.size() + 4, rom_hash, bytes, &error), "size guard");
    check(!load_mod_media(path, "n64-rom", rom.size(), std::string(64, 'z'), bytes, &error), "hash syntax");
    check(!load_mod_media(root / "absent", "n64-rom", 64, rom_hash, bytes, &error), "missing file");
    check(!load_mod_media(path, "file", 513ull * 1024 * 1024, rom_hash, bytes, &error), "bounded allocation");

    std::vector<uint8_t> iso(24 * 2048);
    size_t pvd = 16 * 2048;
    iso[pvd] = 1;
    std::copy_n("CD001", 5, iso.begin() + pvd + 1);
    iso[pvd + 6] = 1; iso[pvd + 80] = 24; iso[pvd + 87] = 24;
    iso[pvd + 129] = 8; iso[pvd + 130] = 8;
    const auto iso_hash = hash(iso);
    const auto disc = root / "disc.iso";
    write(disc, iso);
    check(load_mod_media(disc, "psx-disc", iso.size(), iso_hash, bytes, &error) && *bytes == iso,
          "cooked ISO volume");
    for (unsigned mode : {1u, 2u}) {
        std::vector<uint8_t> raw(24 * 2352);
        for (unsigned i = 0; i < 24; ++i) {
            std::fill(raw.begin() + i * 2352 + 1, raw.begin() + i * 2352 + 11, 0xff);
            raw[i * 2352 + 15] = static_cast<uint8_t>(mode);
            std::copy_n(iso.begin() + i * 2048, 2048,
                        raw.begin() + i * 2352 + (mode == 1 ? 16 : 24));
        }
        write(root / "disc.bin", raw);
        check(load_mod_media(root / "disc.bin", "psx-disc", iso.size(), iso_hash, bytes, &error) && *bytes == iso,
              "Mode 1/2 raw BIN canonical identity");
        std::vector<uint8_t> mode2;
        for (unsigned i = 0; i < 24; ++i)
            mode2.insert(mode2.end(), raw.begin()+i*2352+16, raw.begin()+(i+1)*2352);
        check(load_mod_media(root / "disc.bin", "psx-mode2", mode2.size(), hash(mode2), bytes, &error) == (mode == 2),
              "raw XA resource accepts Mode 2 and refuses Mode 1");
        if (mode == 2) {
            // Same ISO user data, different XA channel: these are different media.
            raw[16] = raw[20] = 9;
            write(root / "disc.bin", raw);
            check(!load_mod_media(root / "disc.bin", "psx-mode2", mode2.size(), hash(mode2), bytes, &error),
                  "XA subheader participates in identity");
            raw[16] = raw[20] = 0;
            raw[20] = 1;
            write(root / "disc.bin", raw);
            check(!load_mod_media(root / "disc.bin", "psx-mode2", mode2.size(), hash(mode2), bytes, &error),
                  "duplicated XA subheader validated");
            raw[20] = 0;
            write(root / "disc.bin", raw);
        }
        write(root / "audio.bin", std::vector<uint8_t>(2352, 0x5a));
        const auto cue = root / "disc.cue";
        {
            std::ofstream out(cue);
            out << "FILE \"disc.bin\" BINARY\n TRACK 01 MODE" << mode << "/2352\n INDEX 01 00:00:00\n"
                   "FILE \"audio.bin\" BINARY\n TRACK 02 AUDIO\n INDEX 01 00:00:00\n";
        }
        check(load_mod_media(cue, "psx-disc", iso.size(), iso_hash, bytes, &error) && *bytes == iso,
              "CUE identity excludes audio tracks");
    }
    iso[pvd + 87] = 23;
    write(disc, iso);
    check(!load_mod_media(disc, "psx-disc", iso.size(), hash(iso), bytes, &error), "ISO endian count guard");
    iso[pvd + 87] = 24; iso[pvd + 1] = 'X';
    write(disc, iso);
    check(!load_mod_media(disc, "psx-disc", iso.size(), hash(iso), bytes, &error), "ISO PVD guard");
    iso[pvd + 1] = 'C'; iso[pvd + 80] = 40; iso[pvd + 87] = 40;
    write(disc, iso);
    check(load_mod_media(disc, "psx-disc", iso.size(), hash(iso), bytes, &error) && *bytes == iso,
          "mixed-mode volume size may extend beyond data track");
    // Bounded immutable ISO9660 lookup, including a file crossing sectors.
    std::vector<uint8_t> mode2_files(24*2336);
    auto dual=[&](size_t at,uint32_t value) {
        for(unsigned i=0;i<4;++i) {mode2_files[at+i]=uint8_t(value>>(i*8));mode2_files[at+7-i]=uint8_t(value>>(i*8));}
    };
    const size_t volume=16*2336+8,dir=20*2336+8;
    mode2_files[volume]=1;std::copy_n("CD001",5,mode2_files.begin()+volume+1);mode2_files[volume+6]=1;
    dual(volume+158,20);dual(volume+166,2048);
    mode2_files[dir]=42;dual(dir+2,21);dual(dir+10,3000);mode2_files[dir+32]=8;
    std::copy_n("TEXT.BIN",8,mode2_files.begin()+dir+33);
    for(unsigned i=0;i<3000;++i)mode2_files[(21+i/2048)*2336+8+i%2048]=uint8_t(i*7);
    ModMode2Disc native(mode2_files.data(),mode2_files.size());
    uint32_t found_lba=0,found_size=0;std::vector<uint8_t> found;
    check(native.find("text.bin",found_lba,found_size) && found_lba==21 && found_size==3000 &&
          native.read("TEXT.BIN",found) && found[2049]==uint8_t(2049*7),"Mode-2 ISO file lookup and cross-sector read");
    check(!native.find("../TEXT.BIN",found_lba,found_size),"Mode-2 lookup rejects parent traversal");
    mode2_files[dir]=1;
    check(!native.find("TEXT.BIN",found_lba,found_size),"short directory record rejected before name-length access");
    mode2_files[dir]=42;mode2_files[dir+9]^=1;
    check(!native.find("TEXT.BIN",found_lba,found_size),"directory dual-endian fields must agree");
    mode2_files[dir+9]^=1;dual(dir+2,24);
    check(!native.find("TEXT.BIN",found_lba,found_size),"donor file extent cannot escape snapshot");
    dual(dir+2,21);mode2_files[21*2336+2]=0x20;
    check(!native.read("TEXT.BIN",found) && found.empty(),"ordinary file reads cannot consume Form-2 XA payload");
    for (const char* file : {"rom.z64", "disc.iso", "disc.bin", "disc.cue", "audio.bin"})
        fs::remove(root / file);
    fs::remove(root);
    return failures ? 1 : 0;
}
