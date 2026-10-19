/* The capture writer records PC evidence for the dirty-page run only, never
 * for the trailing delay-slot guard word it appends (bead beads-eio.3.190).
 *
 * write_json_window grows a run by one guard word so a branch at the run's
 * last word has its delay slot. That word is the first word of the NEXT page.
 * The writer used to walk phys..phys+size for executed_pcs /
 * dispatch_entry_pcs / seeds AFTER adding the guard, so a PC recorded in the
 * next page's first word was attributed to this run. compile_overlays then
 * requested it as a fragment and the recompiler (whose analysis ends before
 * the guard word) returned an empty manifest: Ace Combat 3 `no-func-ids` at
 * 0x800D1000 / 0x800EA000.
 *
 * Two shapes, both through the real writer (overlay_capture_write_json):
 *   A. A dispatch-only PC in the guard word of an ordinary run.
 *   B. Two executed runs on either side of the kernel/boot capture-window
 *      boundary (0x10000). The kernel run's guard word is the boot run's
 *      first word, executed: it must be listed by the boot run and not by the
 *      kernel run. */
#include "code_provider.h"
#include "dirty_ram_interp.h"
#include "overlay_capture.h"

#include "psx_sdl.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern "C" {
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
uint64_t g_dirty_ram_insns_run = 0;
uint64_t g_dirty_window_dispatches = 0;
uint64_t s_frame_count = 0;
uint32_t g_overlay_region_floor = 0x00010000u;
}

namespace {

uint8_t g_ram[2u * 1024u * 1024u]{};
uint32_t g_dirty_pages[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};

int provider_available() { return 0; }
int provider_request() { return 0; }
int provider_busy() { return 0; }

const CodeProvider kProvider = {
    "capture-guard-word-test", provider_available, provider_request,
    provider_busy, nullptr,
};

void set_bit(uint32_t *bitmap, uint32_t phys) {
    const uint32_t word = phys >> 2;
    bitmap[word >> 5] |= 1u << (word & 31u);
}

void set_exec(uint32_t phys) {
    set_bit(g_dirty_ram_exec_pc_bitmap, phys);
    set_bit(g_dirty_ram_dispatch_pc_bitmap, phys);
    const uint32_t page = phys >> 12;
    g_dirty_ram_exec_page_bitmap[page >> 5] |= 1u << (page & 31u);
    g_dirty_pages[page >> 5] |= 1u << (page & 31u);
}

std::string read_all(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

/* The JSON object of the region whose load_addr is `virt`, or "". */
std::string region(const std::string &json, uint32_t virt) {
    char key[64];
    std::snprintf(key, sizeof(key), "\"load_addr\": \"0x%08X\"", virt);
    const size_t at = json.find(key);
    if (at == std::string::npos) return std::string();
    const size_t open = json.rfind('{', at);
    const size_t close = json.find('}', at);
    return json.substr(open, close - open + 1);
}

/* The array text of `field` inside one region object. */
std::string field(const std::string &obj, const char *name) {
    const std::string key = std::string("\"") + name + "\": [";
    const size_t at = obj.find(key);
    if (at == std::string::npos) return std::string();
    const size_t start = at + key.size();
    return obj.substr(start, obj.find(']', start) - start);
}

bool lists(const std::string &obj, const char *name, uint32_t virt) {
    char pc[16];
    std::snprintf(pc, sizeof(pc), "\"0x%08X\"", virt);
    return field(obj, name).find(pc) != std::string::npos;
}

int failures = 0;

void expect(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
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
extern "C" void overlay_loader_check_cache(uint32_t, uint32_t,
                                             const uint8_t *) {}
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
        ("psxrecomp-capture-guard-" + std::to_string(
            static_cast<unsigned long long>(SDL_GetPerformanceCounter())));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    const auto capture = root / "overlay_captures.json";
    overlay_capture_set_path(capture.string().c_str());
    overlay_capture_set_enabled(1);
    /* The writer activates on the first post-handoff DMA. */
    overlay_capture_on_dma(0x30000u, 4u, &g_ram[0x30000]);

    /* A: run [0x30000, 0x31000) + guard word 0x31000, dispatch-only there. */
    set_exec(0x30000u);
    set_exec(0x30FFCu);
    set_bit(g_dirty_ram_dispatch_pc_bitmap, 0x31000u);
    /* B: kernel run [0xF000, 0x10000) + guard 0x10000; boot run
     * [0x10000, 0x11000) whose first word is executed. */
    set_exec(0x0F000u);
    set_exec(0x0FFFCu);
    set_exec(0x10000u);
    set_exec(0x10FFCu);

    overlay_capture_write_json();
    overlay_capture_wait_pending();
    const std::string json = read_all(capture);

    const std::string a = region(json, 0x80030000u);
    expect(!a.empty(), "A: region 0x80030000 written");
    expect(a.find("\"size\": 4100") != std::string::npos,
           "A: run still carries its guard word");
    expect(a.find("\"guard_bytes\": 4") != std::string::npos,
           "A: guard word declared");
    expect(lists(a, "dispatch_entry_pcs", 0x80030FFCu),
           "A: last word of the run is listed");
    expect(!lists(a, "dispatch_entry_pcs", 0x80031000u),
           "A: guard-word PC not a dispatch entry of the run");
    expect(!lists(a, "seeds", 0x80031000u),
           "A: guard-word PC not a seed of the run");
    expect(region(json, 0x80031000u).empty(),
           "A: a dispatch-only guard word does not make a region");

    const std::string kernel = region(json, 0x8000F000u);
    const std::string boot = region(json, 0x80010000u);
    expect(!kernel.empty() && !boot.empty(), "B: both window runs written");
    expect(kernel.find("\"guard_bytes\": 4") != std::string::npos,
           "B: kernel run carries the cross-window guard word");
    expect(lists(kernel, "executed_pcs", 0x8000FFFCu),
           "B: kernel run lists its last word");
    expect(!lists(kernel, "executed_pcs", 0x80010000u),
           "B: kernel run does not list the boot run's first word");
    expect(!lists(kernel, "dispatch_entry_pcs", 0x80010000u),
           "B: nor as a dispatch entry");
    expect(lists(boot, "executed_pcs", 0x80010000u) &&
           lists(boot, "dispatch_entry_pcs", 0x80010000u),
           "B: the boot run still owns its first word");

    overlay_capture_wait_pending();
    std::filesystem::remove_all(root, ignored);
    SDL_Quit();
    if (failures) return 1;
    std::puts("PASS: capture writer records no PC in a run's guard word");
    return 0;
}
