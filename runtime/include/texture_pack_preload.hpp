#ifndef PSX_TEXTURE_PACK_PRELOAD_HPP
#define PSX_TEXTURE_PACK_PRELOAD_HPP

/* Shared machinery for the elective full texture-pack preload
 * (hd_texture_pack_preload, duck_texture_pack_preload): an aggregate
 * decoded-byte reservation and a bounded worker pool that can neither leak a
 * joinable std::thread nor let an exception escape a thread entry. */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

namespace psx_texpreload {

/* Test-only fault injection. 1: the first item's decode throws
 * std::bad_alloc. 2: creating the second pool thread throws std::system_error.
 * 3: publication fails after the first pinned image. */
inline std::atomic<int>& fault_mode() {
    static std::atomic<int> mode{0};
    return mode;
}
inline void after_publish(size_t count) {
    if (count == 1 && fault_mode().load() == 3) throw std::bad_alloc();
}

/* Aggregate decoded bytes, reserved before an image is decoded. */
class Budget {
public:
    explicit Budget(size_t cap) : cap_(cap) {}
    bool reserve(size_t bytes) {
        size_t cur = used_.load();
        do {
            if (bytes > cap_ || cur > cap_ - bytes) return false;
        } while (!used_.compare_exchange_weak(cur, cur + bytes));
        return true;
    }
    void release(size_t bytes) { used_.fetch_sub(bytes); }
private:
    size_t cap_;
    std::atomic<size_t> used_{0};
};

/* Per-item outcome, written by exactly one worker. */
enum ItemStatus : unsigned char { kPending = 0, kLoaded, kFailed, kOverBudget };

/* Joins every started thread when the pool goes out of scope, including while
 * unwinding from a failed std::thread construction. */
struct Joiner {
    std::vector<std::thread>& threads;
    ~Joiner() {
        for (auto& t : threads)
            if (t.joinable()) t.join();
    }
};

/* Runs item(k) for every k < n on at most max_threads threads, the caller
 * being one of them. An exception from item(k) is caught and counted, never
 * propagated out of a thread entry. A failure to start a thread only lowers
 * concurrency. Returns the number of items whose item(k) threw. */
template <class F>
size_t run(size_t n, unsigned max_threads, F&& item) {
    std::atomic<size_t> next{0}, errors{0};
    auto work = [&]() noexcept {
        for (size_t k; (k = next.fetch_add(1)) < n;) {
            try {
                if (k == 0 && fault_mode().load() == 1) throw std::bad_alloc();
                item(k);
            } catch (...) {
                errors.fetch_add(1);
            }
        }
    };
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const unsigned threads = std::max(1u, std::min(max_threads ? max_threads : 1u, hw));
    std::vector<std::thread> pool;
    {
        Joiner joiner{pool};
        for (unsigned t = 1; t < threads && t < n; ++t) {
            try {
                if (t == 1 && fault_mode().load() == 2)
                    throw std::system_error(
                        std::make_error_code(std::errc::resource_unavailable_try_again));
                pool.emplace_back(work);
            } catch (...) {
                break;
            }
        }
        work();
    }
    return errors.load();
}

} // namespace psx_texpreload

#endif
