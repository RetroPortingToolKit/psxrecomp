/* While a replay records or plays, the snapshots kept before a DMA do not
 * replace the latest capture file.
 *
 * The overlay compiler reads the latest file (--captures). A compile is
 * requested after an autocapture commit, but the writer thread commits the
 * DMA snapshots made after it (overlay_capture_before_dma) whenever it gets to
 * them, and each newer one replaced the latest file. The compiler then read
 * whichever was there when it opened the file. An MDK (SLES-00599) recording
 * compiled 000F7000_4F69E5B0 and its playback, with the same guest bytes,
 * compiled 000F7000_570A0CE6, and the playback stopped at frame 2060.
 *
 * This test commits one capture, then makes a series of DMAs:
 *   A  no replay: the latest file is the last DMA snapshot (unchanged rule);
 *   B  a replay plays a recording whose code log says its captures compiled
 *      the autocapture (overlay_autocapture_replay_set PACED_AUTOCAPTURE):
 *      the latest file is still the capture, and the DMA snapshots are in
 *      the history;
 *   C  a replay records (overlay_autocapture_watch): the same as B;
 *   D  a replay plays an older recording, made when the compiler read the
 *      latest file (PACED_LATEST): as A, so its units are compiled from the
 *      same input as when it was recorded.
 */
#include "code_provider.h"
#include "dirty_ram_interp.h"
#include "overlay_capture.h"

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

const unsigned kDmas = 12;
uint8_t g_ram[2u * 1024u * 1024u]{};
uint32_t g_dirty_pages[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
int g_failures = 0;

void check(bool ok, const std::string &what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
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

std::string read_all(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

unsigned history_files(const fs::path &capture) {
    unsigned n = 0;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(capture.string() + ".d", ec))
        n += entry.is_regular_file();
    return n;
}

void fresh_guest() {
    std::memset(g_ram, 0, sizeof g_ram);
    std::memset(g_dirty_ram_exec_pc_bitmap, 0, sizeof g_dirty_ram_exec_pc_bitmap);
    std::memset(g_dirty_ram_dispatch_pc_bitmap, 0, sizeof g_dirty_ram_dispatch_pc_bitmap);
    std::memset(g_dirty_ram_dispatch_seg_bitmap, 0, sizeof g_dirty_ram_dispatch_seg_bitmap);
    std::memset(g_dirty_ram_exec_page_bitmap, 0, sizeof g_dirty_ram_exec_page_bitmap);
    std::memset(g_dirty_pages, 0, sizeof g_dirty_pages);
}

/* The same guest history every run: code runs in a page that a disc read then
 * overwrites, as MDK does at 0x800F7000. */
void make_dmas() {
    for (unsigned i = 0; i < kDmas; ++i) {
        const uint32_t page = 0x30000u + (i % 2u) * 0x1000u;
        for (uint32_t j = 0; j < 64u; ++j)
            g_ram[page + j] = static_cast<uint8_t>(i * 13u + j);
        set_exec(page + 4u * (i % 4u));
        overlay_capture_before_dma(0x80000000u | page, 2048u);
        std::memset(&g_ram[page], static_cast<int>(0x50u ^ i), 2048u);
    }
}

enum class Mode { none, playing, playing_latest, recording };

void no_observer(void) {}

/* Returns the latest file after the DMAs, and the one the capture wrote. */
void run(const fs::path &root, const char *name, Mode mode, std::string *capture_text,
         std::string *latest_after, unsigned *history) {
    const fs::path folder = root / name;
    fs::create_directories(folder);
    const fs::path capture = folder / "overlay_captures.json";
    fresh_guest();
    overlay_capture_set_path(capture.string().c_str());
    overlay_capture_set_enabled(1);
    overlay_capture_on_dma(0x10000u, 4u, &g_ram[0x10000]);   /* the store wakes */
    if (mode == Mode::playing) overlay_autocapture_replay_set(OVERLAY_REPLAY_PACED_AUTOCAPTURE);
    if (mode == Mode::playing_latest) overlay_autocapture_replay_set(OVERLAY_REPLAY_PACED_LATEST);
    if (mode == Mode::recording) overlay_autocapture_watch(no_observer);
    /* The capture the compiler is asked to read: code in a third page. */
    for (uint32_t j = 0; j < 64u; ++j) g_ram[0x40000u + j] = static_cast<uint8_t>(0x11u + j);
    set_exec(0x40000u);
    overlay_capture_write_json();
    *capture_text = read_all(capture);
    make_dmas();
    overlay_capture_wait_pending();          /* every DMA snapshot is committed */
    *latest_after = read_all(capture);
    *history = history_files(capture);
    overlay_autocapture_replay_set(0);
    overlay_autocapture_watch(nullptr);
}

}  // namespace

extern "C" uint8_t *memory_get_ram_ptr(void) { return g_ram; }
extern "C" uint32_t dirty_ram_get_bitmap_word_count(void) {
    return DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS;
}
extern "C" uint32_t dirty_ram_get_bitmap_word(uint32_t index) {
    return index < DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS ? g_dirty_pages[index] : 0u;
}
extern "C" int cdrom_load_in_progress(void) { return 0; }
extern "C" int fntrace_is_game_started(void) { return 1; }
extern "C" void overlay_loader_check_cache(uint32_t, uint32_t, const uint8_t *) {}
extern "C" int overlay_loader_registered_count(void) { return 0; }
extern "C" const CodeProvider *code_provider_active(void) { return nullptr; }
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
        ("psxrecomp-capture-replay-latest-" + std::to_string(
            static_cast<unsigned long long>(SDL_GetPerformanceCounter())));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    std::string capture, latest;
    unsigned history = 0;

    run(root, "a-no-replay", Mode::none, &capture, &latest, &history);
    check(!capture.empty() && history > 2u,
          "A: expected a capture and DMA snapshots in the history, got " + std::to_string(history));
    check(latest != capture, "A: without a replay the last DMA snapshot is the latest file");

    run(root, "b-playing", Mode::playing, &capture, &latest, &history);
    check(history > 2u, "B: the DMA snapshots are kept in the history, got " + std::to_string(history));
    check(latest == capture, "B: while a replay plays, a DMA snapshot replaced the latest file");

    run(root, "c-recording", Mode::recording, &capture, &latest, &history);
    check(history > 2u, "C: the DMA snapshots are kept in the history, got " + std::to_string(history));
    check(latest == capture, "C: while a replay records, a DMA snapshot replaced the latest file");

    run(root, "d-playing-latest", Mode::playing_latest, &capture, &latest, &history);
    check(history > 2u, "D: the DMA snapshots are kept in the history, got " + std::to_string(history));
    check(latest != capture,
          "D: an older recording plays with the last DMA snapshot as the latest file, as it was recorded");

    fs::remove_all(root, ignored);
    SDL_Quit();
    if (g_failures) {
        std::fprintf(stderr, "overlay capture replay latest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: while a replay records or plays a recording made so, the compiler's capture "
              "file is the capture it was asked to read; DMA snapshots stay in the history; an "
              "older recording plays with the latest file, as recorded");
    return 0;
}
