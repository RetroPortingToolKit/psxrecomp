/* Resident disc resources (mod_resident.h): build from the effective disc,
 * warm verification, corruption and plan-change invalidation, stock policy,
 * derived blobs, relocation, pruning and the guest-range guard. */
#include "mod_resident.h"
#include "mod_runtime.h"
#include "mod_plugins.h"
#include "psx_sha256.h"
#include "psx_lobby_client.h"
#include "gpu.h"
#include "gpu_hd_textures.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::array<uint8_t, 2 * 1024 * 1024> ram;
static int failures;

extern "C" uint8_t psx_read_byte(uint32_t a) { return ram[a & 0x1fffffu]; }
extern "C" void psx_write_byte(uint32_t a, uint8_t v) { ram[a & 0x1fffffu] = v; }
extern "C" uint16_t psx_read_half(uint32_t a) {
    return (uint16_t)(ram[a & 0x1fffffu] | ram[(a + 1) & 0x1fffffu] << 8);
}
extern "C" void psx_write_half(uint32_t a, uint16_t v) {
    psx_write_byte(a, (uint8_t)v); psx_write_byte(a + 1, (uint8_t)(v >> 8));
}
extern "C" uint32_t psx_read_word(uint32_t a) {
    return (uint32_t)psx_read_half(a) | (uint32_t)psx_read_half(a + 2) << 16;
}
extern "C" void psx_write_word(uint32_t a, uint32_t v) {
    psx_write_half(a, (uint16_t)v); psx_write_half(a + 2, (uint16_t)(v >> 16));
}
extern "C" void psx_host_write_byte(uint32_t a, uint8_t v) { psx_write_byte(a, v); }
extern "C" void psx_host_write_half(uint32_t a, uint16_t v) { psx_write_half(a, v); }
extern "C" void psx_host_write_word(uint32_t a, uint32_t v) { psx_write_word(a, v); }
extern "C" uint32_t psx_mod_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t, uint32_t) { return 0; }
extern "C" void psx_ram_reset_size_request(void) {}
extern "C" void gpu_hd_textures_shutdown(void) {}
extern "C" int gpu_hd_textures_configure(const char*, int, int, char*, size_t) { return 1; }
extern "C" void gpu_hd_textures_set_dump_enabled(int) {}
extern "C" int gpu_hd_textures_active(void) { return 1; }
extern "C" void gpu_hd_textures_get_diag(GpuHdTextureDiag* out) { *out = {}; }
extern "C" int gpu_hd_textures_reload(char*, size_t) { return 1; }
extern "C" void psx_projection_reset_session(void) {}
extern "C" void gpu_ws_set_native_scene_predicate(int (*)(void)) {}
extern "C" int psx_ws_x_margin(void) { return 0; }
extern "C" void gpu_get_display_info(GpuDisplayInfo* out) { *out = GpuDisplayInfo{}; }
extern "C" void dirty_ram_mark_executable_range(uint32_t, uint32_t) {}
extern "C" int fntrace_is_game_started(void) { return 1; }
extern "C" void gpu_ws_tag_hud_primitive(uint32_t, int) {}
extern "C" void gpu_ws_tag_world_primitive(uint32_t, int) {}
extern "C" void gpu_ws_set_adaptive_backdrop_preload(int) {}
extern "C" int gpu_ws_configured_x_reveal(void) { return 0; }
extern "C" void gpu_ws_tag_hud_prim(uint32_t, int) {}
extern "C" void gpu_ws_tag_screen_mask_quad(uint32_t) {}
extern "C" { uint64_t s_frame_count = 0; }
static uint64_t cycles;
extern "C" uint64_t psx_get_cycle_count(void) { return cycles; }
extern "C" void gpu_ws_tag_radial_screen_mask_quad(uint32_t, float) {}
/* mod_runtime.cpp's lobby netplay commit reads the negotiated match caps.
 * No lobby match is negotiated in this test. */
static PsxLobbyMatchCaps no_match_caps;
extern "C" const PsxLobbyMatchCaps* psx_lobby_match_caps(void) { return &no_match_caps; }

static void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; failures++; }
}

static std::string sha_hex(const uint8_t* p, size_t n) {
    uint8_t d[32];
    psx_sha256_compute(p, n, d);
    static const char* h = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; i++) { s[2 * i] = h[d[i] >> 4]; s[2 * i + 1] = h[d[i] & 15]; }
    return s;
}

static void write_bytes(const fs::path& p, const std::vector<uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write((const char*)b.data(), (std::streamsize)b.size());
}
static void write_text(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << t;
}

/* derive: one reversed copy of each file, plus a copy of file 0's bytes from
 * file 1 (identical bytes must share one blob). */
static int derive_calls;
static int derive(PSXResidentSink* sink, uint32_t file, const uint8_t* data, uint32_t size,
                  int stock, void* user) {
    derive_calls++;
    std::vector<uint8_t> rev(data, data + size);
    std::reverse(rev.begin(), rev.end());
    const uint32_t meta[4] = {size, (uint32_t)stock, 0xabcdu, file};
    if (!psx_resident_emit(sink, file, 7, meta, rev.data(), size)) return 0;
    if (file == 1) {
        const auto* first = static_cast<const std::vector<uint8_t>*>(user);
        if (!psx_resident_emit(sink, file, 9, nullptr, first->data(), (uint32_t)first->size())) return 0;
    }
    return 1;
}

static std::string events(uint64_t lo, uint64_t hi, uint32_t max) {
    std::vector<char> b(1 << 20);
    return psx_resident_events_json(b.data(), (uint32_t)b.size(), lo, hi, max) ? std::string(b.data()) : "";
}

static std::string status() {
    std::vector<char> b(8192);
    return psx_resident_status_json(b.data(), (uint32_t)b.size()) ? std::string(b.data()) : "";
}

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "psx_mod_resident_test";
    fs::remove_all(root, ec);
#ifdef _WIN32
    _putenv_s("PSX_RESIDENT_CACHE", (root / "cache").string().c_str());
#else
    setenv("PSX_RESIDENT_CACHE", (root / "cache").string().c_str(), 1);
#endif

    /* ISO: root dir @20 -> DATA dir @21 -> A.BIN (3000 bytes @22..23),
     * B.BIN (5000 bytes @24..26). Non-zero bytes after each end of file. */
    std::vector<uint8_t> iso(30 * 2048);
    auto le32 = [&](size_t at, uint32_t n) { for (unsigned i = 0; i < 4; ++i) iso[at + i] = (uint8_t)(n >> (i * 8)); };
    auto record = [&](size_t at, uint32_t lba, uint32_t bytes, bool dir, const std::string& name) {
        iso[at] = (uint8_t)((33 + name.size() + 1) & ~size_t(1));
        le32(at + 2, lba); le32(at + 10, bytes); iso[at + 25] = dir ? 2 : 0;
        iso[at + 28] = 1; iso[at + 31] = 1; iso[at + 32] = (uint8_t)name.size();
        std::copy(name.begin(), name.end(), iso.begin() + at + 33);
        return (size_t)iso[at];
    };
    iso[16 * 2048] = 1; std::copy_n("CD001", 5, iso.begin() + 16 * 2048 + 1); iso[16 * 2048 + 6] = 1;
    record(16 * 2048 + 156, 20, 2048, true, std::string(1, '\0'));
    record(20 * 2048, 21, 2048, true, "DATA");
    const size_t a_rec = 21 * 2048;
    const size_t b_rec = a_rec + record(a_rec, 22, 3000, false, "A.BIN;1");
    record(b_rec, 24, 5000, false, "B.BIN;1");
    for (unsigned i = 0; i < 2 * 2048; ++i) iso[22 * 2048 + i] = i < 3000 ? (uint8_t)(i * 7) : 0xEE;
    for (unsigned i = 0; i < 3 * 2048; ++i) iso[24 * 2048 + i] = i < 5000 ? (uint8_t)(i * 13 + 1) : 0xDD;
    const std::vector<uint8_t> a_padded(iso.begin() + 22 * 2048, iso.begin() + 24 * 2048);
    const std::vector<uint8_t> b_padded(iso.begin() + 24 * 2048, iso.begin() + 27 * 2048);
    const std::string a_sha = sha_hex(a_padded.data(), a_padded.size());
    const std::string b_sha = sha_hex(b_padded.data(), b_padded.size());

    const fs::path disc_root = root / "disc";
    const fs::path iso_path = disc_root / "game.iso";
    write_bytes(iso_path, iso);
    std::string error;
    check(PSXRecompV4::mod_runtime_initialize(disc_root, "RESIDENT", 0, {}, &error), "initialize");
    check(PSXRecompV4::mod_runtime_commit(iso_path, &error), "commit");
    check(!PSXRecompV4::mod_runtime_fingerprint().empty(), "a committed plan has a fingerprint");

    uint32_t lba = 0, size = 0;
    check(psx_mod_disc_file_extent("DATA/B.BIN", &lba, &size) && lba == 24 && size == 5000,
          "extent query reports the effective record");
    check(!psx_mod_disc_file_extent("DATA", &lba, &size) && !lba && !size, "extent rejects directories");

    PSXResidentFile files[2] = {{"DATA/A.BIN", 3000, a_sha.c_str()},
                                {"data/b.bin", 5000, b_sha.c_str()}};
    PSXResidentSpec spec{};
    spec.struct_size = sizeof spec;
    spec.title = "ResidentTest";
    spec.format = "rt-v1";
    spec.files = files;
    spec.file_count = 2;
    spec.policy = PSX_RESIDENT_REQUIRE_STOCK;
    spec.derive = derive;
    spec.derive_user = (void*)&a_padded;
    spec.keep_packs = 2;

    const PSXResidentPack* pack = psx_resident_prepare(&spec);
    check(pack != nullptr, "cold prepare");
    check(derive_calls == 2, "derive runs once per file on a cold build");
    check(status().find("\"state\":\"prepared\"") != std::string::npos, "status reports a prepared pack");
    uint32_t padded = 0;
    const uint8_t* a = psx_resident_file(pack, 0, &size, &padded);
    check(a && size == 3000 && padded == 4096 && std::equal(a_padded.begin(), a_padded.end(), a),
          "file bytes include the true sector tail");
    check(psx_resident_file_lba(pack, 1) == 24 && psx_resident_file_stock(pack, 0) &&
          psx_resident_file_stock(pack, 1) && !psx_resident_modified_files(pack), "lba and stock flags");
    uint32_t which = 99;
    const uint8_t* at = psx_resident_find_lba(pack, 25, 4096, &which);
    check(at && which == 1 && std::equal(b_padded.begin() + 2048, b_padded.end(), at), "find by lba inside a file");
    check(!psx_resident_find_lba(pack, 25, 4097, nullptr), "find rejects spans past the padded end");
    check(!psx_resident_find_lba(pack, 21, 16, nullptr), "find rejects lbas before every file");
    check(psx_resident_derived_count(pack) == 3, "derived blobs kept in order");
    uint32_t file = 0, tag = 0, meta[4] = {};
    const uint8_t* d = psx_resident_derived(pack, 1, &file, &tag, meta, &size);
    check(d && file == 1 && tag == 7 && size == 6144 && meta[0] == 6144 && meta[1] == 1 && meta[2] == 0xabcd &&
          d[0] == b_padded.back(), "derived blob metadata and bytes");
    const uint8_t* dup = psx_resident_derived(pack, 2, &file, &tag, nullptr, &size);
    check(dup == a && tag == 9, "identical bytes share one blob");

    /* Warm: verified from disk, derive not called. */
    pack = psx_resident_prepare(&spec);
    check(pack && derive_calls == 2, "warm prepare reuses the cache");
    check(status().find("\"state\":\"verified\"") != std::string::npos, "status reports verification");
    check(std::equal(a_padded.begin(), a_padded.end(), psx_resident_file(pack, 0, nullptr, nullptr)),
          "warm bytes identical");

    /* A flipped payload byte is caught by its blob hash and rebuilt. */
    fs::path pack_path;
    for (const auto& e : fs::directory_iterator(root / "cache" / "ResidentTest"))
        if (e.path().extension() == ".pack") pack_path = e.path();
    {
        std::fstream f(pack_path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-1, std::ios::end);
        char c = 0x55;
        f.write(&c, 1);
    }
    pack = psx_resident_prepare(&spec);
    check(pack && derive_calls == 4, "corrupt cache rebuilt");

    /* Stock policy. */
    std::string wrong(64, '0');
    files[0].stock_sha256 = wrong.c_str();
    check(!psx_resident_prepare(&spec), "require-stock rejects a differing file");
    check(status().find("differs from the original disc") != std::string::npos, "failure reason kept");
    spec.policy = PSX_RESIDENT_ALLOW_MODIFIED;
    pack = psx_resident_prepare(&spec);
    check(pack && !psx_resident_file_stock(pack, 0) && psx_resident_modified_files(pack) == 1,
          "allow-modified serves and counts the differing file");
    files[0].stock_sha256 = a_sha.c_str();
    spec.policy = PSX_RESIDENT_REQUIRE_STOCK;

    /* Old packs of this format are pruned to keep_packs; others untouched. */
    for (int i = 0; i < 4; i++) write_bytes(root / "cache" / "ResidentTest" / ("rt-v1-old" + std::to_string(i) + ".pack"), {1});
    write_bytes(root / "cache" / "ResidentTest" / "other-v1-x.pack", {1});
    pack = psx_resident_prepare(&spec);
    unsigned ours = 0, other = 0;
    for (const auto& e : fs::directory_iterator(root / "cache" / "ResidentTest")) {
        const auto name = e.path().filename().string();
        ours += name.rfind("rt-v1-", 0) == 0;
        other += name.rfind("other-", 0) == 0;
    }
    check(pack && ours == 3 && other == 1, "prune keeps the active pack plus keep_packs of its format only");

    /* A mod plan that patches B and grows A through its directory record:
     * a new key (new pack), effective bytes and the relocated extent. */
    write_text(disc_root / "packages/rt.patch/1.0.0/manifest.toml",
        "format_version = 5\nid = \"rt.patch\"\nversion = \"1.0.0\"\nname = \"RT Patch\"\n"
        "[[target]]\ngame_id = \"RESIDENT\"\ndisc_sha256 = \"" + sha_hex(iso.data(), iso.size()) + "\"\n"
        "[[feature]]\nid = \"asset\"\nname = \"Asset\"\n"
        "[[patch]]\nfeature = \"asset\"\ntarget = \"disc_user\"\noffset = " + std::to_string(24 * 2048 + 2) + "\n"
        "expected = \"1b\"\nreplace = \"5a\"\n"
        "[[patch]]\nfeature = \"asset\"\ntarget = \"disc_user\"\noffset = " + std::to_string(a_rec + 10) + "\n"
        "expected = \"b80b0000\"\nreplace = \"00100000\"\n");
    write_text(disc_root / "state.toml",
        "format_version = 2\n[[feature]]\npackage_id = \"rt.patch\"\nid = \"asset\"\nenabled = true\n");
    const std::string stock_fingerprint = PSXRecompV4::mod_runtime_fingerprint();
    check(PSXRecompV4::mod_runtime_initialize(disc_root, "RESIDENT", 0, {}, &error) &&
          PSXRecompV4::mod_runtime_commit(iso_path, &error), "patched plan commit");
    check(PSXRecompV4::mod_runtime_fingerprint() != stock_fingerprint, "plan fingerprint changed");
    check(!psx_resident_prepare(&spec), "require-stock refuses the modded plan");
    spec.policy = PSX_RESIDENT_ALLOW_MODIFIED;
    const int before = derive_calls;
    pack = psx_resident_prepare(&spec);
    check(pack && derive_calls == before + 2, "modded plan builds its own pack");
    const uint8_t* b = psx_resident_file(pack, 1, &size, nullptr);
    check(b && b[2] == 0x5a && !psx_resident_file_stock(pack, 1), "effective patched bytes served");
    a = psx_resident_file(pack, 0, &size, &padded);
    check(a && size == 4096 && padded == 4096 && a[3500] == 0xEE && !psx_resident_file_stock(pack, 0),
          "grown file read through its patched directory record");
    check(status().find("\"modified\":2") != std::string::npos, "status counts modified files");

    /* An unwritable cache keeps the pack in memory: the loader path depends
     * on the disc and plan only. A regular file where the title folder
     * belongs makes every cache write fail. */
    const fs::path blocked = root / "blocked";
    write_bytes(blocked / "ResidentTest", {1});
#ifdef _WIN32
    _putenv_s("PSX_RESIDENT_CACHE", blocked.string().c_str());
#else
    setenv("PSX_RESIDENT_CACHE", blocked.string().c_str(), 1);
#endif
    pack = psx_resident_prepare(&spec);
    check(pack && psx_resident_file(pack, 1, nullptr, nullptr), "unwritable cache still prepares");
    check(status().find("prepared (memory only:") != std::string::npos, "memory-only state reported");
    check(fs::is_regular_file(blocked / "ResidentTest"), "no cache written over the blocker");
#ifdef _WIN32
    _putenv_s("PSX_RESIDENT_CACHE", (root / "cache").string().c_str());
#else
    setenv("PSX_RESIDENT_CACHE", (root / "cache").string().c_str(), 1);
#endif

    /* Service ring: frame window, newest-first selection, oldest-first
     * output, eviction past capacity. */
    check(events(0, UINT64_MAX, 10).find("\"total\":0") != std::string::npos, "empty ring");
    for (uint32_t i = 0; i < 20; i++) {
        s_frame_count = 100 + i;
        cycles = 1000 * i;
        psx_resident_record(i & 1 ? "rt.read" : "rt.begin", i & 1 ? UINT32_MAX : i, 22 + i, 2048 * i, i % 3 != 0);
    }
    const std::string window = events(105, 107, 100);
    check(window.find("\"seq\":5,\"frame\":105,\"cycle\":5000,\"op\":\"rt.read\",\"file\":-1,\"lba\":27,\"bytes\":10240,\"served\":1}") != std::string::npos &&
          window.find("\"seq\":7") != std::string::npos && window.find("\"seq\":4,") == std::string::npos &&
          window.find("\"seq\":8,") == std::string::npos, "frame window inclusive");
    check(window.find("\"seq\":5") < window.find("\"seq\":6"), "oldest first");
    const std::string newest = events(0, UINT64_MAX, 2);
    check(newest.find("\"seq\":18") != std::string::npos && newest.find("\"seq\":19") != std::string::npos &&
          newest.find("\"seq\":17") == std::string::npos, "count keeps the newest");
    check(events(106, 106, 10).find("\"served\":0") != std::string::npos, "declined requests kept");
    std::vector<char> tiny(16);
    check(!psx_resident_events_json(tiny.data(), 16, 0, UINT64_MAX, 10), "small buffer refused");
    for (uint32_t i = 0; i < 9000; i++) psx_resident_record("rt.flood", 0, i, 0, 1);
    const std::string all = events(0, UINT64_MAX, 100000);
    check(all.find("\"total\":9020,\"capacity\":8192") != std::string::npos &&
          all.find("\"seq\":828,") != std::string::npos && all.find("\"seq\":827,") == std::string::npos,
          "oldest entries evicted");

    /* Guest-range guard. */
    for (uint32_t i = 0; i < 64; i++) ram[0x1000 + i] = (uint8_t)(i ^ 0x5a);
    std::vector<uint8_t> guarded(ram.begin() + 0x1000, ram.begin() + 0x1040);
    const std::string guard = sha_hex(guarded.data(), guarded.size());
    const PSXResidentRange ranges[2] = {{0x80001000u, 0x80001020u}, {0x80001020u, 0x80001040u}};
    check(psx_resident_guest_ranges_match(ranges, 2, guard.c_str()), "guard matches");
    ram[0x1030] ^= 1;
    check(!psx_resident_guest_ranges_match(ranges, 2, guard.c_str()), "guard detects a changed byte");

    /* Invalid specs are refused before any disc access. */
    PSXResidentSpec bad = spec;
    bad.format = "bad/format";
    check(!psx_resident_prepare(&bad), "format ids are file-name safe");
    bad = spec;
    bad.struct_size = 4;
    check(!psx_resident_prepare(&bad), "struct size checked");

    fs::remove_all(root, ec);
    if (failures) return 1;
    std::cout << "mod resident tests passed\n";
    return 0;
}

/* render_pass.c: no sandboxed local view runs in this test. */
extern "C" int psx_mod_local_view_scope(void) { return 0; }
