#include "gpu.h"
#include <stdlib.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#define CHECK(c) do { if (!(c)) abort(); } while (0)
/* This fixture has no decoder traffic; frame/classification are input state. */
uint64_t s_frame_count = 17;
int mdec_recently_active(uint32_t within_frames) { (void)within_frames; return 0; }
static atomic_int phase;
static GpuExecutionSnapshot *snapshot;
static uint16_t *live;
static void replay(void) {
    CHECK(gpu_execution_snapshot_bind(snapshot));
    uint16_t *private_vram = gpu_get_vram_ptr();
    CHECK(private_vram != live && private_vram[2345] == 17);
    private_vram[2345] = 29;
    atomic_store_explicit(&phase, 1, memory_order_release);
    while (atomic_load_explicit(&phase, memory_order_acquire) != 2) { }
    CHECK(private_vram[2345] == 29);
    gpu_execution_snapshot_unbind();
    CHECK(gpu_get_vram_ptr() == live);
}
#ifdef _WIN32
static DWORD WINAPI entry(LPVOID p) { (void)p; replay(); return 0; }
#else
static void *entry(void *p) { (void)p; replay(); return NULL; }
#endif
int main(void) {
    live = gpu_get_vram_ptr();
    live[2345] = 17;
    snapshot = gpu_execution_snapshot_create();
    CHECK(snapshot && gpu_execution_snapshot_capture(snapshot));
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, entry, NULL, 0, NULL);
    CHECK(thread);
#else
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, entry, NULL));
#endif
    while (atomic_load_explicit(&phase, memory_order_acquire) != 1) { }
    CHECK(gpu_get_vram_ptr() == live && live[2345] == 17);
    live[2345] = 41;
    atomic_store_explicit(&phase, 2, memory_order_release);
#ifdef _WIN32
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    CHECK(!pthread_join(thread, NULL));
#endif
    CHECK(gpu_execution_snapshot_capture(snapshot));
    CHECK(gpu_execution_snapshot_bind(snapshot));
    CHECK(gpu_get_vram_ptr()[2345] == 41);
    gpu_execution_snapshot_unbind();
    gpu_execution_snapshot_destroy(snapshot);
    return 0;
}
