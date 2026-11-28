/* A periodic capture may fire while the drive is reading, between the first
 * and the last page of one CD read, and still binds evidence only to bytes
 * that ran.
 *
 * overlay_autocapture_tick() used to skip every fire while the drive was
 * reading. A title that keeps the drive reading for a whole scene then never
 * captured that scene. The gate added no coherence: overlay_capture_before_dma
 * clears the execution evidence of every page a CD DMA covers before its
 * first word lands, so a snapshot taken mid-read cannot name code that ran on
 * the bytes the read replaces.
 *
 * This test makes a two-page read over pages where interpreted code ran,
 * lands the first page, runs code on it and elsewhere, and ticks the periodic
 * capture before the second page lands:
 *
 *   - overlay_capture_before_dma cleared the evidence of both pages;
 *   - with a turbo load engaged (vblanks faster than real time) no capture
 *     fires;
 *   - with the turbo load off and the read still in flight the capture fires:
 *     it names the code elsewhere and the code that ran on the landed page
 *     after the read began, and no PC that ran on the read's pages before it.
 */
#include "code_provider.h"
#include "dirty_ram_interp.h"
#include "overlay_capture.h"
#include "turbo_loads_gate.h"

#include "psx_sdl.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern "C" {
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_dispatch_seg_bitmap[PSX_CODE_SEGMENT_COUNT]
                                        [DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
uint64_t g_dirty_ram_insns_run = 0;
uint64_t g_dirty_window_dispatches = 0;
uint64_t s_frame_count = 0;
uint32_t g_overlay_region_floor = 0x00010000u;
}

namespace {

namespace fs = std::filesystem;

const uint32_t kPage = 4096u;
const uint32_t kReadBase = 0x00040000u;             /* the two pages the read covers */
const uint32_t kOldPc0 = kReadBase + 0x100u;        /* ran on page one before the read */
const uint32_t kOldPc1 = kReadBase + kPage + 0x100u;/* ran on page two before the read */
const uint32_t kLandedPc = kReadBase + 0x200u;      /* ran on page one after it landed */
const uint32_t kElsewhere = 0x00100000u;
const uint32_t kElsewherePc = kElsewhere + 0x40u;   /* ran outside the read */

uint8_t g_ram[2u * 1024u * 1024u]{};
uint32_t g_dirty_pages[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
int g_failures = 0;

int provider_available() { return 1; }
int provider_request() { return 1; }
int provider_busy() { return 0; }

const CodeProvider kProvider = {
    "capture-mid-dma-test", provider_available, provider_request,
    provider_busy, nullptr,
};

void expect(bool ok, const char *what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

bool bit_is_set(const uint32_t *bitmap, uint32_t phys) {
    const uint32_t word = phys >> 2;
    return ((bitmap[word >> 5] >> (word & 31u)) & 1u) != 0;
}

bool page_is_set(const uint32_t *bitmap, uint32_t phys) {
    const uint32_t page = phys >> 12;
    return ((bitmap[page >> 5] >> (page & 31u)) & 1u) != 0;
}

void set_exec(uint32_t phys) {
    const uint32_t word = phys >> 2;
    g_dirty_ram_exec_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    g_dirty_ram_dispatch_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    g_dirty_ram_dispatch_seg_bitmap[1][word >> 5] |= 1u << (word & 31u);
    const uint32_t page = phys >> 12;
    g_dirty_ram_exec_page_bitmap[page >> 5] |= 1u << (page & 31u);
    g_dirty_pages[page >> 5] |= 1u << (page & 31u);
}

/* Bytes that no other call makes, over one page from `base`. */
void fill_page(uint32_t base, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 0x9E3779B9u;
    for (uint32_t i = 0; i < kPage; ++i) {
        x = x * 1664525u + 1013904223u;
        g_ram[base + i] = static_cast<uint8_t>(x >> 24);
    }
}

std::string read_all(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool names(const std::string &json, uint32_t phys) {
    char pc[16];
    std::snprintf(pc, sizeof pc, "\"0x%08X\"", 0x80000000u | phys);
    return json.find(pc) != std::string::npos;
}

}  // namespace

extern "C" uint8_t *memory_get_ram_ptr(void) { return g_ram; }
extern "C" uint32_t dirty_ram_get_bitmap_word_count(void) {
    return DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS;
}
extern "C" uint32_t dirty_ram_get_bitmap_word(uint32_t index) {
    return index < DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS ? g_dirty_pages[index] : 0u;
}
extern "C" int fntrace_is_game_started(void) { return 1; }
extern "C" void overlay_loader_check_cache(uint32_t, uint32_t, const uint8_t *) {}
extern "C" int overlay_loader_registered_count(void) { return 0; }
extern "C" const CodeProvider *code_provider_active(void) { return &kProvider; }
extern "C" uint32_t crc32_compute(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash ? hash : 1u;
}

int main() {
    if (psx_sdl_init(0) != 0) {
        std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const fs::path root = fs::temp_directory_path() /
        ("psxrecomp-capture-mid-dma-" + std::to_string(
            static_cast<unsigned long long>(SDL_GetPerformanceCounter())));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);
    const fs::path capture = root / "overlay_captures.json";
    overlay_capture_set_path(capture.string().c_str());
    overlay_capture_set_enabled(1);
    overlay_autocapture_set_enabled(1);
    /* The writer activates on the first DMA after the game has started. */
    overlay_capture_on_dma(0x10000u, 4u, &g_ram[0x10000]);

    /* Interpreted code ran on both pages the read is about to replace. */
    fill_page(kReadBase, 1u);
    fill_page(kReadBase + kPage, 2u);
    set_exec(kOldPc0);
    set_exec(kOldPc1);

    /* The read begins: the CD DMA is about to write its first word. */
    overlay_capture_before_dma(0x80000000u | kReadBase, 2u * kPage);
    expect(!bit_is_set(g_dirty_ram_exec_pc_bitmap, kOldPc0) &&
           !page_is_set(g_dirty_ram_exec_page_bitmap, kOldPc0),
           "before_dma cleared the evidence of the read's first page");
    expect(!bit_is_set(g_dirty_ram_exec_pc_bitmap, kOldPc1) &&
           !page_is_set(g_dirty_ram_exec_page_bitmap, kOldPc1),
           "before_dma cleared the evidence of the read's last page");

    /* Page one lands and code runs on it; other code runs elsewhere. Page two
     * still holds its old bytes: the read is in flight. */
    fill_page(kReadBase, 3u);
    set_exec(kLandedPc);
    fill_page(kElsewhere, 4u);
    set_exec(kElsewherePc);

    /* A turbo load runs vblanks faster than real time: no capture fires. */
    turbo_loads_set_engaged(1);
    s_frame_count = 120u;
    g_dirty_window_dispatches = 128u;
    g_dirty_ram_insns_run = 100000u;
    overlay_autocapture_tick();
    expect(bit_is_set(g_dirty_ram_exec_pc_bitmap, kElsewherePc),
           "no periodic capture fires while a turbo load is engaged");

    /* The turbo load is off and the read is still in flight: it fires. A
     * capture that fires takes the executed instructions with it. */
    turbo_loads_set_engaged(0);
    s_frame_count = 240u;
    g_dirty_window_dispatches = 256u;
    g_dirty_ram_insns_run = 200000u;
    overlay_autocapture_tick();
    const bool fired = !bit_is_set(g_dirty_ram_exec_pc_bitmap, kElsewherePc);
    expect(fired, "the periodic capture fires mid-read with the turbo load off");

    /* The latest file is the newest snapshot: the periodic capture's, not the
     * outgoing one before_dma queued. */
    overlay_capture_wait_pending();
    if (fired) {
        const std::string json = read_all(capture);
        expect(names(json, kElsewherePc), "the capture names the code elsewhere");
        expect(names(json, kLandedPc),
               "the capture names the code that ran on the landed page after the read began");
        expect(!names(json, kOldPc0),
               "the capture names no PC that ran on the read's first page before it");
        expect(!names(json, kOldPc1),
               "the capture names no PC that ran on the read's last page before it");
    }

    overlay_autocapture_set_enabled(0);
    fs::remove_all(root, ignored);
    SDL_Quit();
    if (g_failures) {
        std::fprintf(stderr, "overlay capture mid-DMA: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: a periodic capture between the first and last page of a CD read names only code "
              "that ran on bytes still in RAM, and none fires while a turbo load is engaged");
    return 0;
}
