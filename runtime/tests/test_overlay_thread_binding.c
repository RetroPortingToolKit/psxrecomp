/* Concurrent calls through the production shard shim must keep the host's
 * callbacks, pending cycles and mutable dispatch breadcrumbs separate. */
#include <stdlib.h>
#include <stdatomic.h>
#include "overlay_dispatch_preamble.c.inc"
/* Checks and thread creation must still execute in Release builds. */
#define assert(condition) do { if (!(condition)) abort(); } while (0)
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static atomic_int phase;
static unsigned live_cycles, replay_cycles;
static int live_bail, replay_bail;
static uint32_t live_pc, replay_pc;
static uint64_t live_first, replay_first, live_resolved, replay_resolved;
static void charge_live(uint32_t cycles) { live_cycles += cycles; }
static void charge_replay(uint32_t cycles) { replay_cycles += cycles; }
static void wait_for(int value) {
    while (atomic_load_explicit(&phase, memory_order_acquire) != value) { }
}
static void signal_phase(int value) {
    atomic_store_explicit(&phase, value, memory_order_release);
}
static void replay(void) {
    OverlayCallbacks cbs = {0};
    cbs.cpu_charge = charge_replay;
    cbs.call_bail_flag = &replay_bail;
    cbs.last_store_pc = &replay_pc;
    cbs.bail_first = &replay_first;
    cbs.bail_resolved = &replay_resolved;
    overlay_init(&cbs);
    psx_cpu_charge(11);
    g_debug_last_store_pc = 0x80002000u;
    g_psx_call_bail = 2;
    g_psx_bail_first = 3;
    g_psx_bail_resolved = 4;
    signal_phase(1);
    wait_for(2);
    assert(g_debug_last_store_pc == 0x80002000u);
    assert(g_psx_call_bail == 2);
    overlay_flush_cycles();
    assert(replay_cycles == 11);
    /* Rebinding with missing pointers must not retain the former host's
     * state, and fallback words must belong to this thread too. */
    cbs.call_bail_flag = 0;
    cbs.last_store_pc = 0;
    cbs.bail_first = 0;
    cbs.bail_resolved = 0;
    psx_cpu_charge(13);
    overlay_init(&cbs);
    assert(replay_cycles == 24);
    g_psx_call_bail = 99;
    g_debug_last_store_pc = 99;
    signal_phase(3);
}
#ifdef _WIN32
static DWORD WINAPI replay_entry(LPVOID arg) { (void)arg; replay(); return 0; }
#else
static void *replay_entry(void *arg) { (void)arg; replay(); return 0; }
#endif
int main(void) {
    OverlayCallbacks cbs = {0};
    cbs.cpu_charge = charge_live;
    cbs.call_bail_flag = &live_bail;
    cbs.last_store_pc = &live_pc;
    cbs.bail_first = &live_first;
    cbs.bail_resolved = &live_resolved;
    overlay_init(&cbs);
#ifdef _WIN32
    HANDLE thread = CreateThread(0, 0, replay_entry, 0, 0, 0);
    assert(thread);
#else
    pthread_t thread;
    assert(pthread_create(&thread, 0, replay_entry, 0) == 0);
#endif
    wait_for(1);
    psx_cpu_charge(7);
    g_debug_last_store_pc = 0x80001000u;
    g_psx_call_bail = 5;
    g_psx_bail_first = 6;
    g_psx_bail_resolved = 7;
    overlay_flush_cycles();
    assert(live_cycles == 7);
    signal_phase(2);
    wait_for(3);
#ifdef _WIN32
    assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    assert(pthread_join(thread, 0) == 0);
#endif
    assert(live_bail == 5 && replay_bail == 2);
    assert(live_pc == 0x80001000u && replay_pc == 0x80002000u);
    assert(live_first == 6 && replay_first == 3);
    assert(live_resolved == 7 && replay_resolved == 4);
    assert(g_debug_last_store_pc == live_pc);
    assert(g_psx_call_bail == live_bail);
    return 0;
}
