/* The durable capture history keeps one whole record per snapshot, whichever
 * threads commit at the same time.
 *
 * With game.toml [runtime] overlay_capture_history = true (off by default) the
 * runtime appends one line per committed snapshot to
 * overlay_captures.addendum.jsonl. With a persist folder it also keeps a copy
 * of the snapshot there, and the line names the copy instead of holding it.
 *
 * Three threads commit snapshots: the writer of the periodic capture, the
 * writer of the queue of outgoing snapshots, and the emulation thread (at a
 * manual dump, at shutdown). The history step runs after the commit lock is
 * released and had no lock of its own. A record that holds a snapshot is far
 * longer than the stdio buffer and leaves it in several writes, so the records
 * of two threads could mix in the file. Such lines are not JSON, and the
 * reader (tools/coverage_vault.py) drops them: both snapshots are lost from
 * the history. Two records could also take the same sequence number.
 *
 * This test makes disc reads over pages where interpreted code ran, and after
 * each read a manual dump of other code, so that the queue's writer and the
 * emulation thread commit at the same time. In two runs the periodic capture
 * fires as well. An outgoing snapshot is 192 pages, about 1 MB of text. The
 * history file is then held against the store:
 *
 *   - every line is a whole record: its header, then exactly the snapshot it
 *     names (the store's file with that signature), then the closing brace;
 *   - every snapshot in the store has one record, so none is lost;
 *   - the sequence numbers are 1, 2, 3, ... in the order of the lines;
 *   - each caller wrote records (the reason field of a record names it).
 *
 *   A  history off: the store of the series, and no history file;
 *   B  history on, two callers: the same store as A, and a whole history;
 *   C  history on, three callers;
 *   D  history on with a persist folder, three callers: each record names its
 *      own copy of the snapshot, and the folder holds those copies only.
 */
#include "code_provider.h"
#include "dirty_ram_interp.h"
#include "overlay_capture.h"

#include "psx_sdl.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
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

const char *const kGame = "TEST-00001";
const char *const kHistoryFile = "overlay_captures.addendum.jsonl";
const uint32_t kPages = 192u;                       /* pages in one snapshot */
const uint32_t kBytes = kPages * 4096u;
const uint32_t kReadBase = 0x00020000u;             /* where the disc reads land */
const uint32_t kPeriodicBase = 0x00100000u;         /* code that only the periodic capture sees */
const uint32_t kManualBase = 0x001D0000u;           /* code the manual dumps see */
const uint32_t kManualPages = 16u;
const unsigned kReads = 16u;

uint8_t g_ram[2u * 1024u * 1024u]{};
uint32_t g_dirty_pages[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
int g_failures = 0;

int provider_available() { return 1; }
int provider_request() { return 1; }
int provider_busy() { return 0; }

const CodeProvider kProvider = {
    "capture-history-test", provider_available, provider_request,
    provider_busy, nullptr,
};

void check(bool ok, const std::string &what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

std::string n(unsigned long long value) { return std::to_string(value); }

bool bit_is_set(const uint32_t *bitmap, uint32_t phys) {
    const uint32_t word = phys >> 2;
    return ((bitmap[word >> 5] >> (word & 31u)) & 1u) != 0;
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

/* The store: the latest file and every file of the additive history. */
std::map<std::string, std::string> store_of(const fs::path &capture) {
    std::map<std::string, std::string> files;
    files["(latest)"] = read_all(capture);
    const fs::path history = capture.string() + ".d";
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(history, ec))
        if (entry.is_regular_file())
            files[entry.path().filename().string()] = read_all(entry.path());
    return files;
}

void same_store(const std::map<std::string, std::string> &expected,
                const std::map<std::string, std::string> &got, const char *run) {
    check(got.size() == expected.size(),
          std::string(run) + ": " + n(got.size()) + " files in the store, expected " + n(expected.size()));
    for (const auto &entry : expected) {
        const auto found = got.find(entry.first);
        if (found == got.end())
            check(false, std::string(run) + ": the store lacks " + entry.first);
        else if (found->second != entry.second)
            check(false, std::string(run) + ": " + entry.first + " has other content");
    }
    for (const auto &entry : got)
        if (!expected.count(entry.first))
            check(false, std::string(run) + ": the store has an extra file " + entry.first);
}

/* Interpreted code runs in `pages` pages from `base`: bytes that no other
 * call makes, and one executed instruction in each page. */
void run_code_at(uint32_t base, uint32_t seed, uint32_t pages = kPages) {
    uint32_t x = seed * 2654435761u + 0x9E3779B9u;
    for (uint32_t i = 0; i < pages * 4096u; ++i) {
        x = x * 1664525u + 1013904223u;
        g_ram[base + i] = static_cast<uint8_t>(x >> 24);
    }
    for (uint32_t page = 0; page < pages; ++page)
        set_exec(base + page * 4096u);
}

/* The same guest history every time: code runs in the pages, a disc read is
 * about to overwrite them, the read happens, and the emulation thread makes a
 * manual dump of other code while the queue's writer writes the outgoing
 * snapshot. With `periodic` other code runs elsewhere after each read and the
 * periodic capture gets its turn. Returns how many times the periodic capture
 * fired. */
unsigned make_reads(unsigned reads, bool periodic) {
    unsigned fired = 0;
    for (unsigned i = 0; i < reads; ++i) {
        run_code_at(kReadBase, i + 1u);
        overlay_capture_before_dma(0x80000000u | kReadBase, kBytes);
        std::memset(&g_ram[kReadBase], static_cast<int>(0xE0u ^ i), kBytes);   /* the read lands */
        run_code_at(kManualBase, 500u + i, kManualPages);
        overlay_capture_write_json();
        if (!periodic) continue;
        run_code_at(kPeriodicBase, 1000u + i);
        s_frame_count += 1000u;
        g_dirty_window_dispatches += 1000u;
        overlay_autocapture_tick();
        /* A capture that fires takes the executed instructions with it. */
        if (!bit_is_set(g_dirty_ram_exec_pc_bitmap, kPeriodicBase)) ++fired;
    }
    return fired;
}

void fresh_guest() {
    std::memset(g_ram, 0, sizeof g_ram);
    std::memset(g_dirty_ram_exec_pc_bitmap, 0, sizeof g_dirty_ram_exec_pc_bitmap);
    std::memset(g_dirty_ram_dispatch_pc_bitmap, 0, sizeof g_dirty_ram_dispatch_pc_bitmap);
    std::memset(g_dirty_ram_dispatch_seg_bitmap, 0, sizeof g_dirty_ram_dispatch_seg_bitmap);
    std::memset(g_dirty_ram_exec_page_bitmap, 0, sizeof g_dirty_ram_exec_page_bitmap);
    std::memset(g_dirty_pages, 0, sizeof g_dirty_pages);
}

fs::path begin_run(const fs::path &folder, bool history, const fs::path *persist) {
    fs::create_directories(folder);
    if (persist) fs::create_directories(*persist);
    const fs::path capture = folder / "overlay_captures.json";
    fresh_guest();
    overlay_capture_set_path(capture.string().c_str());
    overlay_capture_set_enabled(1);
    /* After the path, as main() does: the history file goes beside it. */
    overlay_capture_configure_history(history ? 1 : 0,
                                      persist ? persist->string().c_str() : nullptr, kGame);
    /* the store wakes at the first DMA after the game has started */
    overlay_capture_on_dma(0x10000u, 4u, &g_ram[0x10000]);
    return capture;
}

std::map<std::string, std::string> end_run(const fs::path &capture) {
    overlay_capture_wait_pending();         /* a normal quit: drain, then stop */
    return store_of(capture);
}

/* What a record holds of a snapshot: the store's file without the white space
 * outside its strings. */
std::string minified(const std::string &json) {
    std::string out;
    out.reserve(json.size());
    bool in_string = false, escape = false;
    for (const char c : json) {
        if (in_string) {
            out.push_back(c);
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
            out.push_back(c);
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(c);
        }
    }
    return out;
}

std::string json_string(const std::string &value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20u) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04X", static_cast<unsigned>(c));
            out += escaped;
        } else {
            out.push_back(c);
        }
    }
    return out + "\"";
}

bool eat(const std::string &line, size_t &at, const std::string &literal) {
    if (at > line.size() || line.compare(at, literal.size(), literal) != 0) return false;
    at += literal.size();
    return true;
}

bool eat_to_quote(const std::string &line, size_t &at, std::string &out) {
    const size_t end = at > line.size() ? std::string::npos : line.find('"', at);
    if (end == std::string::npos) return false;
    out = line.substr(at, end - at);
    at = end;
    return true;
}

struct Record {
    std::string session, reason, signature, rest;
    unsigned long sequence = 0;
};

/* The header of one line, up to the snapshot. `rest` is what follows it. */
bool read_header(const std::string &line, bool reference, Record &record) {
    size_t at = 0;
    if (!eat(line, at, std::string("{\"schema\":\"psxrecomp overlay capture addendum ") +
                           (reference ? "v2" : "v1") + "\",\"game\":\"" + kGame + "\",\"session\":\"") ||
        !eat_to_quote(line, at, record.session) || !eat(line, at, "\",\"sequence\":"))
        return false;
    const size_t digits = at;
    while (at < line.size() && std::isdigit(static_cast<unsigned char>(line[at]))) ++at;
    if (at == digits || at - digits > 9u) return false;
    record.sequence = std::strtoul(line.substr(digits, at - digits).c_str(), nullptr, 10);
    if (!eat(line, at, ",\"reason\":\"") || !eat_to_quote(line, at, record.reason) ||
        !eat(line, at, "\",\"fnv64\":\"") || !eat_to_quote(line, at, record.signature) ||
        record.signature.size() != 16u ||
        !eat(line, at, reference ? "\",\"snapshot\":" : "\",\"captures\":"))
        return false;
    record.rest = line.substr(at);
    return true;
}

struct History {
    unsigned records = 0;       /* whole records */
    unsigned queued = 0;        /* written by the queue's writer */
    unsigned manual = 0;        /* written by the emulation thread, at a manual dump */
    unsigned periodic = 0;      /* written by the periodic capture's writer */
    bool whole = false;         /* every line is a whole record */
};

/* Hold the history file against the store. With `persist`, a record names its
 * copy of the snapshot in that folder instead of holding the snapshot. */
History check_history(const std::string &run, const fs::path &capture,
                      const std::map<std::string, std::string> &store, const fs::path *persist) {
    History history;
    const std::string text = read_all(capture.parent_path() / kHistoryFile);
    check(!text.empty(), run + ": there is no history file");
    if (text.empty()) return history;
    check(text.back() == '\n', run + ": the history file does not end with a line end");

    std::string session;
    std::map<std::string, unsigned> records_of;         /* signature -> whole records */
    std::map<unsigned long, unsigned> uses_of;          /* sequence number -> whole records */
    std::set<std::string> copies;
    unsigned lines = 0, broken = 0, first_broken = 0, misnumbered = 0;

    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1u;
        ++lines;

        Record record;
        bool whole = read_header(line, persist != nullptr, record);
        if (whole && session.empty()) session = record.session;
        whole = whole && record.session == session;
        if (whole) {
            const auto snapshot = store.find(record.signature + ".json");
            if (snapshot == store.end()) {
                whole = false;
            } else if (!persist) {
                whole = record.rest == minified(snapshot->second) + "}";
            } else {
                char number[16];
                std::snprintf(number, sizeof number, "%04lu", record.sequence);
                const std::string name = std::string(kGame) + "_" + record.session + "_" + number +
                                         "_" + record.signature + ".json";
                const std::string copy = persist->string() + "/" + name;
                whole = record.rest == json_string(copy) + "}" &&
                        read_all(fs::path(copy)) == snapshot->second;
                copies.insert(name);
            }
        }
        if (!whole) {
            if (!broken++) first_broken = lines;
            continue;
        }
        ++history.records;
        ++records_of[record.signature];
        ++uses_of[record.sequence];
        if (record.sequence != history.records) ++misnumbered;
        if (record.reason == "preserve-outgoing") ++history.queued;
        else if (record.reason == "shutdown-or-manual") ++history.manual;
        else if (record.reason == "autocap") ++history.periodic;
    }

    check(broken == 0u, run + ": " + n(broken) + " of " + n(lines) +
              " lines of the history file are not whole records; the first is line " + n(first_broken));
    unsigned shared = 0;
    for (const auto &use : uses_of)
        if (use.second > 1u) ++shared;
    check(shared == 0u, run + ": " + n(shared) + " sequence numbers are on more than one record");
    unsigned snapshots = 0, lost = 0;
    for (const auto &entry : store) {
        if (entry.first == "(latest)") continue;
        ++snapshots;
        const std::string signature = entry.first.substr(0, entry.first.find('.'));
        if (!records_of.count(signature)) ++lost;
    }
    check(lost == 0u, run + ": " + n(lost) + " of " + n(snapshots) +
              " snapshots in the store have no whole record in the history file");
    history.whole = broken == 0u;
    if (!history.whole) return history;         /* the counts below would only repeat it */

    check(history.records == snapshots, run + ": " + n(history.records) + " records for " + n(snapshots) +
              " snapshots; each snapshot is committed once");
    check(misnumbered == 0u, run + ": " + n(misnumbered) +
              " records do not carry the number of their line; the numbers should be 1, 2, 3, ...");
    if (persist) {
        unsigned files = 0, strangers = 0;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(*persist, ec)) {
            ++files;
            if (!copies.count(entry.path().filename().string())) ++strangers;
        }
        check(files == history.records && strangers == 0u,
              run + ": the persist folder holds " + n(files) + " files, " + n(strangers) +
                  " of them named by no record; expected one copy for each of the " +
                  n(history.records) + " records");
    }
    return history;
}

/* Each caller the run was meant to have did write records. */
void check_callers(const std::string &run, const History &history, unsigned reads, unsigned fired) {
    if (!history.whole) return;
    check(history.queued == reads && history.manual == reads,
          run + ": " + n(history.queued) + " records from the queue's writer and " + n(history.manual) +
              " from the emulation thread's manual dumps; expected " + n(reads) + " of each");
    check(history.periodic == fired,
          run + ": " + n(history.periodic) + " records from the periodic capture, which fired " +
              n(fired) + " times");
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
        ("psxrecomp-capture-history-" + std::to_string(
            static_cast<unsigned long long>(SDL_GetPerformanceCounter())));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    /* A: the option is off. This is the store the series leaves. */
    fs::path folder = root / "a-history-off";
    fs::path capture = begin_run(folder, false, nullptr);
    (void)make_reads(kReads, false);
    const auto plain = end_run(capture);
    check(plain.size() == 2u * kReads + 1u,
          "A: the store holds " + n(plain.size()) + " files; expected the latest file and two for each of the " +
              n(kReads) + " reads");
    check(!fs::exists(folder / kHistoryFile), "A: a history file was written with the option off");
    fs::remove_all(folder, ignored);

    /* B: the option is on. The emulation thread and the queue's writer commit
     * at the same time. The store is the one of A. */
    folder = root / "b-two-callers";
    capture = begin_run(folder, true, nullptr);
    (void)make_reads(kReads, false);
    auto store = end_run(capture);
    same_store(plain, store, "B");
    check_callers("B", check_history("B", capture, store, nullptr), kReads, 0u);
    fs::remove_all(folder, ignored);

    /* C: the periodic capture's writer commits beside the other two. */
    overlay_autocapture_set_enabled(1);
    folder = root / "c-three-callers";
    capture = begin_run(folder, true, nullptr);
    unsigned fired = make_reads(kReads, true);
    store = end_run(capture);
    check(fired >= 1u, "C: the periodic capture never fired");
    check_callers("C", check_history("C", capture, store, nullptr), kReads, fired);
    fs::remove_all(folder, ignored);

    /* D: with a persist folder a record names its own copy of the snapshot. */
    folder = root / "d-persist-folder";
    const fs::path persist = folder / "kept";
    capture = begin_run(folder, true, &persist);
    fired = make_reads(kReads / 2u, true);
    store = end_run(capture);
    check(fired >= 1u, "D: the periodic capture never fired");
    check_callers("D", check_history("D", capture, store, &persist), kReads / 2u, fired);

    overlay_autocapture_set_enabled(0);
    fs::remove_all(root, ignored);
    SDL_Quit();
    if (g_failures) {
        std::fprintf(stderr, "overlay capture history: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: every snapshot has one whole history record, numbered once and in the order of the "
              "lines, with two and with three threads committing; the store is the same with the history "
              "on and off");
    return 0;
}
