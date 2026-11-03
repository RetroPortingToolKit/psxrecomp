/* Bounded background capture, following snesrecomp's diagnostic writers
 * (a6be9756285f26ccc74539c811d4d23d57d8c33d) and MegaManX4Recomp's port of it.
 * Only owned per-VBlank rows cross the queue; the worker never reads emulated
 * memory or runtime APIs. One simulation-thread producer. Close/flush require
 * that producer to stop. Normal close drains; a full queue stops capture rather
 * than dropping rows or blocking gameplay. File formatting and disk I/O run on
 * the worker thread; the game thread only copies one small row. */
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace psx_playtest_diagnostic {
// sequence, buttons P1/P2, {analog, lx, ly, rx, ry} for P1/P2, digest,
// connected P1/P2. Presence is independent of the analog capability.
constexpr unsigned RowColumns = 16;
using Row = std::array<int64_t, RowColumns>;

template<size_t Capacity = 4096, class Record = Row> class Writer {
    enum class Kind { Native, Flush };
    enum Error { None, Full, Io, Start };
    struct Event {
        Record row{};
        Kind kind = Kind::Native;
    };
    static_assert(Capacity > 1, "queue must hold at least one event");
    std::unique_ptr<Event[]> queue;
    std::ofstream file;
    std::string prefix = "N ";
    std::thread thread;
    std::atomic<size_t> read{0}, write{0};
    std::atomic<unsigned> flushes{0};
    unsigned requested_flushes = 0;
    std::atomic<int> error{None};
    std::atomic<bool> stop{false};
    bool opened = false;
#ifdef PSX_DIAGNOSTIC_WRITER_TEST
    void (*gate)() = nullptr;
#endif
    void fail(Error value) { int expected = None; error.compare_exchange_strong(expected, value); }
    Event *reserve() {
        if (!opened || error.load(std::memory_order_relaxed)) return nullptr;
        size_t head = write.load(std::memory_order_relaxed);
        if ((head + 1) % Capacity == read.load(std::memory_order_acquire)) { fail(Full); return nullptr; }
        return &queue[head];
    }
    void publish() {
        write.store((write.load(std::memory_order_relaxed) + 1) % Capacity, std::memory_order_release);
    }
    void run() {
#ifdef PSX_DIAGNOSTIC_WRITER_TEST
        if (gate) gate();
#endif
        auto last_flush = std::chrono::steady_clock::now();
        bool dirty = false;
        for (;;) {
            size_t tail = read.load(std::memory_order_relaxed);
            if (tail != write.load(std::memory_order_acquire)) {
                const auto& event = queue[tail];
                switch (event.kind) {
                case Kind::Native:
                    file << prefix;
                    for (unsigned i = 0; i < event.row.size(); ++i) file << (i ? "," : "") << event.row[i];
                    file << '\n';
                    break;
                case Kind::Flush:
                    file.flush();
                    flushes.fetch_add(1, std::memory_order_release);
                    break;
                }
                // Do not release a slot until formatting has finished.
                read.store((tail + 1) % Capacity, std::memory_order_release);
                dirty = true;
                if (!file) { fail(Io); break; }
            } else {
                if (stop.load(std::memory_order_acquire) || error.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            auto now = std::chrono::steady_clock::now();
            // Wall time also flushes a hung/paused simulation's final input.
            if (dirty && now - last_flush >= std::chrono::milliseconds(100)) {
                file.flush(); last_flush = now; dirty = false;
                if (!file) { fail(Io); break; }
            }
        }
        file.close();
        if (!file) fail(Io);
    }
public:
    Writer() = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer() { close(); }
#ifdef PSX_DIAGNOSTIC_WRITER_TEST
    void test_gate(void (*callback)()) { gate = callback; }
#endif
    void open(const std::filesystem::path& path, const std::string& header,
              const std::string& row_prefix = "N ") {
        close(); clear();
        prefix = row_prefix;
        // Session setup is outside steady-state recording. Make the header
        // observable immediately and report path/permissions errors now.
        file.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
        file << header; file.flush();
        if (!file) { fail(Io); file.close(); return; }
        try {
            queue = std::make_unique<Event[]>(Capacity);
            opened = true; thread = std::thread([this] { run(); });
        } catch (...) { opened = false; fail(Start); file.close(); queue.reset(); }
    }
    bool native(const Record& row) {
        auto *event = reserve(); if (!event) return false;
        event->kind = Kind::Native; event->row = row;
        publish(); return true;
    }
    // Explicit barrier for tests/tools, never called by the recording loop.
    void flush() {
        auto *event = reserve(); if (!event) return;
        unsigned target = ++requested_flushes; event->kind = Kind::Flush; publish();
        while (flushes.load(std::memory_order_acquire) < target && !error.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void close() {
        stop.store(true, std::memory_order_release);
        if (thread.joinable()) thread.join();
        else if (file.is_open()) { file.close(); if (!file) fail(Io); }
        opened = false; queue.reset();
    }
    void clear() {
        // Caller must close first; a worker can never outlive its queue.
        if (opened) return;
        file.clear(); error.store(None); stop.store(false);
        read.store(0); write.store(0); flushes.store(0); requested_flushes = 0;
    }
    bool is_open() const { return opened; }
    explicit operator bool() const { return error.load(std::memory_order_relaxed) == None; }
    const char *error_text() const {
        switch (error.load(std::memory_order_relaxed)) {
        case Full: return "background diagnostic queue full; capture stopped";
        case Io: return "diagnostic file write failed";
        case Start: return "cannot start diagnostic background writer";
        default: return nullptr;
        }
    }
};
}
