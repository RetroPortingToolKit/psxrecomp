/* While a playback holds the guest at a recorded boundary, a refused compiler
 * request is retried at once, and the boundary sees that work is left.
 *
 * The provider refuses a request while its retry backoff (milliseconds) runs.
 * The capture code then retried after a number of frames. A playback that
 * waits for a unit at a recorded boundary does not advance frames, so the
 * retry never came: the wait saw no work and the playback stopped with
 * "a native code unit is unavailable after local compilation". An MDK
 * (SLES-00599) playback stopped this way at frame 2894.
 *
 * Here the provider refuses its first request. With the frame held:
 *   - overlay_autocapture_replay_has_work() is nonzero while the request waits;
 *   - the next tick at the same frame asks the provider again, and it accepts.
 */
#include "code_provider.h"
#include "dirty_ram_interp.h"
#include "overlay_capture.h"

#include "psx_sdl.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" int overlay_capture_test_provider_pending(void);
extern "C" unsigned overlay_capture_test_write_attempts(void);

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

uint8_t g_ram[2u * 1024u * 1024u]{};
uint32_t g_dirty_pages[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
int g_requests = 0;
int g_accepts = 0;

int provider_available() { return 1; }
int provider_request() {
    ++g_requests;
    if (g_requests == 1) return 0;   /* the provider's backoff is running */
    ++g_accepts;
    return 1;
}
int provider_busy() { return 0; }

const CodeProvider kProvider = {
    "capture-replay-retry-test", provider_available, provider_request,
    provider_busy, nullptr,
};

void set_exec(uint32_t phys) {
    const uint32_t word = phys >> 2;
    g_dirty_ram_exec_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    g_dirty_ram_dispatch_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    g_dirty_ram_dispatch_seg_bitmap[1][word >> 5] |= 1u << (word & 31u);
    const uint32_t page = phys >> 12;
    g_dirty_ram_exec_page_bitmap[page >> 5] |= 1u << (page & 31u);
    g_dirty_pages[page >> 5] |= 1u << (page & 31u);
}

int fail(const std::string &message, const std::filesystem::path &root) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    overlay_autocapture_replay_set(0);
    overlay_capture_wait_pending();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    SDL_Quit();
    return 1;
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
    const auto root = std::filesystem::temp_directory_path() /
        ("psxrecomp-capture-replay-retry-" + std::to_string(
            static_cast<unsigned long long>(SDL_GetPerformanceCounter())));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    overlay_capture_set_path((root / "overlay_captures.json").string().c_str());
    overlay_capture_set_enabled(1);
    overlay_autocapture_set_enabled(1);
    g_ram[0x10000] = 0x11;
    overlay_capture_on_dma(0x10000u, 4u, &g_ram[0x10000]);
    set_exec(0x10000u);
    overlay_autocapture_replay_set(1);   /* a playback paces the captures */

    s_frame_count = 2714u;               /* the recorded capture boundary */
    if (!overlay_autocapture_replay_capture())
        return fail("the replay capture did not start", root);

    // The first write fails because the output directory is temporarily absent.
    while (!overlay_capture_test_write_attempts()) {
        if (!overlay_autocapture_replay_has_work())
            return fail("the initial capture stopped before the writer completed", root);
        overlay_autocapture_tick();
        SDL_Delay(5);
    }
    std::filesystem::create_directories(root);

    /* The writer retries while the guest is held. The provider then refuses. */
    while (g_requests == 0) {
        if (!overlay_autocapture_replay_has_work())
            return fail("the transient write failure stopped playback at the held frame", root);
        overlay_autocapture_tick();
        SDL_Delay(5);
    }
    if (!overlay_capture_test_provider_pending())
        return fail("the refused request is not kept pending", root);

    /* The playback now holds frame 2714 and waits for the unit. */
    if (!overlay_autocapture_replay_has_work())
        return fail("at a held boundary a refused request shows no work, so the playback "
                    "stops with the unit unmade", root);
    overlay_autocapture_tick();
    if (g_requests != 2 || g_accepts != 1)
        return fail("at a held boundary the refused request was not retried (requests " +
                    std::to_string(g_requests) + ")", root);
    if (overlay_capture_test_provider_pending() || overlay_autocapture_replay_has_work())
        return fail("the accepted request still shows as pending", root);

    overlay_capture_wait_pending();
    const auto unavailable = root / "unavailable" / "overlay_captures.json";
    overlay_capture_set_path(unavailable.string().c_str());
    set_exec(0x10000u);
    if (!overlay_autocapture_replay_capture())
        return fail("the permanent write failure scenario could not start", root);
    while (overlay_autocapture_replay_has_work()) {
        overlay_autocapture_tick();
        SDL_Delay(5);
    }
    if (overlay_capture_test_write_attempts() < 2 || g_requests != 2)
        return fail("the failed capture did not retry before reporting unavailable work", root);

    // A new playback can retry the retained snapshot once the directory returns.
    overlay_autocapture_replay_set(0);
    std::filesystem::create_directories(unavailable.parent_path());
    overlay_autocapture_replay_set(1);
    while (!std::filesystem::exists(unavailable)) {
        if (!overlay_autocapture_replay_has_work())
            return fail("the next playback did not retry after the directory recovered", root);
        overlay_autocapture_tick();
        SDL_Delay(5);
    }

    overlay_autocapture_replay_set(0);
    overlay_capture_wait_pending();
    std::filesystem::remove_all(root, ignored);
    SDL_Quit();
    std::puts("PASS: a held playback retries transient writer and compiler failures "
              "and returns an error for a persistently unavailable writer");
    return 0;
}
