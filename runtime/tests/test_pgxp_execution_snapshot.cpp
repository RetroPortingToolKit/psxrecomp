#include "pgxp.h"
#include "pgxp_hooks.h"
#include <atomic>
#include <cstdlib>
#include <thread>
#define CHECK(c) do { if (!(c)) std::abort(); } while (0)
extern "C" int gte_geometry_correction_lookup(uint32_t, int32_t*, int32_t*) { return 0; }
extern "C" int gte_geometry_correction_lookup_probe(uint32_t, int32_t*, int32_t*) { return 0; }
extern "C" int psx_mod_gpu_dma_memory_contains(uint32_t, uint32_t) { return 0; }
static constexpr uint32_t address = 0x80001000, packed = 0x000C0008;
static void seed(int32_t x) {
    pgxp_gte_push_sxy(x, 12 << 16, 300, packed);
    pgxp_store_gte_reg(address, 15);
}
static void check(int32_t x) {
    int32_t actual_x, y;
    uint16_t z;
    CHECK(pgxp_load_precise_word(address, packed, &actual_x, &y, &z));
    CHECK(actual_x == x && y == 12 << 16 && z == 300);
}
int main() {
    pgxp_set_enabled(1);
    pgxp_set_cpu_mode(1);
    seed(8 << 16);
    PGXPExecutionSnapshot *snapshot = pgxp_execution_snapshot_create();
    CHECK(snapshot && pgxp_execution_snapshot_capture(snapshot));
    std::atomic<int> phase{0};
    std::thread worker([&] {
        CHECK(pgxp_execution_snapshot_bind(snapshot));
        check(8 << 16);
        seed((8 << 16) + 32768);
        check((8 << 16) + 32768);
        phase.store(1, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) != 2) { }
        check((8 << 16) + 32768);
        pgxp_execution_snapshot_unbind();
    });
    while (phase.load(std::memory_order_acquire) != 1) { }
    check(8 << 16);
    seed((8 << 16) + 16384);
    phase.store(2, std::memory_order_release);
    worker.join();
    check((8 << 16) + 16384);
    CHECK(pgxp_execution_snapshot_capture(snapshot));
    std::thread next([&] {
        CHECK(pgxp_execution_snapshot_bind(snapshot));
        check((8 << 16) + 16384);
        seed((8 << 16) + 49152);
        pgxp_execution_snapshot_unbind();
    });
    next.join();
    /* Re-publication must undo worker writes even if live RAM shadows have
     * not changed since the preceding capture. */
    CHECK(pgxp_execution_snapshot_capture(snapshot));
    std::thread unchanged([&] {
        CHECK(pgxp_execution_snapshot_bind(snapshot));
        check((8 << 16) + 16384);
        pgxp_execution_snapshot_unbind();
    });
    unchanged.join();
    pgxp_execution_snapshot_destroy(snapshot);
    return 0;
}
