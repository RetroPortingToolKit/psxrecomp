#include "psx_cycles.h"
#include "psx_cycle_freeze.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#define CHECK(c) do { if (!(c)) { fputs(#c "\n", stderr); abort(); } } while (0)
int g_ls_replay_active;
static atomic_int phase;
static void overrun(void) { abort(); }
static void replay(void) {
    PsxCycleFreeze freeze;
    psx_cycle_count = 1000;
    psx_guest_cycle_scale_set(2);
    CHECK(psx_cycle_freeze_begin(&freeze, 8000000, overrun));
    CHECK(g_psx_guest_time_frozen && g_psx_render_pass_active);
    psx_cpu_charge(200);
    if (psx_get_cycle_count() != 1100) {
        char detail[160];
        snprintf(detail, sizeof detail, "clock=%llu scale=%u recip=%u frac=%u\n",
                 (unsigned long long)psx_get_cycle_count(), psx_guest_cycle_scale(),
                 g_psx_gcs_recip_q16, g_psx_gcs_frac);
        fputs(detail, stderr);
    }
    CHECK(psx_get_cycle_count() == 1100);
    atomic_store_explicit(&phase, 1, memory_order_release);
    while (atomic_load_explicit(&phase, memory_order_acquire) != 2) { }
    CHECK(psx_get_cycle_count() == 1100);
    psx_cycle_freeze_end(&freeze);
    CHECK(psx_get_cycle_count() == 1000);
    CHECK(!g_psx_guest_time_frozen && !g_psx_render_pass_active);
}
#ifdef _WIN32
static DWORD WINAPI entry(LPVOID p) { (void)p; replay(); return 0; }
#else
static void *entry(void *p) { (void)p; replay(); return NULL; }
#endif
int main(void) {
    psx_cycle_count = 5000;
    psx_guest_cycle_scale_set(1);
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, entry, NULL, 0, NULL);
    CHECK(thread);
#else
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, entry, NULL));
#endif
    while (atomic_load_explicit(&phase, memory_order_acquire) != 1) { }
    CHECK(!g_psx_guest_time_frozen && !g_psx_render_pass_active);
    CHECK(psx_get_cycle_count() == 5000 && psx_guest_cycle_scale() == 1);
    psx_cycle_count = 6000;
    atomic_store_explicit(&phase, 2, memory_order_release);
#ifdef _WIN32
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    CHECK(!pthread_join(thread, NULL));
#endif
    CHECK(psx_get_cycle_count() == 6000 && psx_guest_cycle_scale() == 1);
    return 0;
}
