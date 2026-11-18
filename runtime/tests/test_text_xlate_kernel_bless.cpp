/* A translation patch writes guest RAM without psx_write_byte, so
 * it must tell the kernel-bless table itself. A patch that lands in the
 * relocated-kernel window would otherwise leave a row marked clean over
 * changed bytes, and the stale compiled body would keep running.
 *
 * Drives the real text_xlate.cpp: one glyph label inside the kernel window and
 * one in game RAM. Every byte the patch writes must be reported, at its
 * physical address, before the dispatch that follows. */
#include "text_xlate.h"
#include "cpu_state.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::array<uint8_t, 2u * 1024u * 1024u> g_ram{};
static std::vector<uint32_t> g_noted;   /* one entry per reported byte */

extern "C" {
uint64_t s_frame_count;

uint8_t* memory_get_ram_ptr(void) { return g_ram.data(); }
uint16_t gr_vram_read(int, int) { return 0; }
void gr_vram_write(int, int, uint16_t) {}
void dirty_ram_text_bless(uint32_t, const uint8_t*, uint32_t) {}
void psx_kernel_bless_note_range(uint32_t phys, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) g_noted.push_back(phys + i);
}
}

static bool noted(uint32_t phys) {
    for (uint32_t p : g_noted) if (p == phys) return true;
    return false;
}

int main(void) {
    const uint32_t kernel_va = 0x80000E10u;   /* exception-handler body */
    const uint32_t game_va   = 0x80012340u;
    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("psxrecomp-text-xlate-kbless-" + std::to_string(nonce));
    const fs::path translations = root / "translations";
    std::error_code ec;

#ifdef _WIN32
    _putenv_s("PSX_LANG", "en");
    _putenv_s("PSX_XLATE_CAPTURE", "0");
#else
    setenv("PSX_LANG", "en", 1);
    setenv("PSX_XLATE_CAPTURE", "0", 1);
#endif
    if (!fs::create_directories(translations, ec)) {
        std::cerr << "failed to prepare test directory\n";
        return 1;
    }
    {
        std::ofstream out(translations / "labels.toml");
        out << "[[glyph_label]]\n"
               "addr = " << kernel_va << "\n"
               "src_hex = \"a082\"\n"
               "width = 2\n"
               "en = \"A\"\n"
               "[[glyph_label]]\n"
               "addr = " << game_va << "\n"
               "src_hex = \"a282\"\n"
               "width = 2\n"
               "en = \"B\"\n";
    }
    g_ram[0x0E10u] = 0xA0u;  g_ram[0x0E11u] = 0x82u;
    g_ram[0x12340u] = 0xA2u; g_ram[0x12341u] = 0x82u;

    CPUState cpu{};
    text_xlate_init(root.string().c_str(), "en");
    text_xlate_on_dispatch(&cpu, 0x80010000u);
    fs::remove_all(root, ec);

    const bool patched = (g_ram[0x0E10u] != 0xA0u || g_ram[0x0E11u] != 0x82u) &&
                         (g_ram[0x12340u] != 0xA2u || g_ram[0x12341u] != 0x82u);
    if (!patched) {
        std::cerr << "FAIL: the glyph labels were not patched; the test proves nothing\n";
        return 1;
    }
    if (!noted(0x0E10u) || !noted(0x0E11u) || !noted(0x12340u) || !noted(0x12341u)) {
        std::cerr << "FAIL: a translation patch wrote RAM without "
                     "psx_kernel_bless_note_range (" << g_noted.size() << " bytes reported)\n";
        return 1;
    }
    std::cout << "PASS: translation patches report their RAM writes ("
              << g_noted.size() << " bytes)\n";
    return 0;
}
