#include "psx_thread_local.h"
#include "psx_memory.h"
#include "mod_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#define CHECK(c) do { if (!(c)) abort(); } while (0)
uint32_t g_psx_ram_size = PSX_MAIN_RAM_EXPANDED_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_EXPANDED_BYTES - 1;
PSX_THREAD_LOCAL int g_psx_render_pass_active;
extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
static atomic_int phase;
static PsxMemoryBacking *copy;
static PsxMemoryExecutionState *metadata;
extern PSX_THREAD_LOCAL uint32_t i_stat, i_mask;
extern uint32_t psx_read_word(uint32_t);
static uint32_t worker_status;
static uint8_t *live;
static void replay(void) {
    CHECK(memory_get_ram_ptr() == live);
    CHECK(memory_backing_bind(copy) == NULL);
    memory_execution_bind(metadata, &worker_status);
    g_psx_render_pass_active = 1;
    CHECK(psx_read_word(0x1F801070u) == 0x35);
    CHECK(psx_read_word(0x1F801074u) == 0x17);
    i_stat = 0x62;
    CHECK(g_psx_ram != live && g_psx_ram == memory_get_ram_ptr());
    for (size_t i = 0; i < PSX_MAIN_RAM_EXPANDED_BYTES; ++i)
        CHECK(g_psx_ram[i] == (i == 0x700000u ? 37u : (uint8_t)(i * 13u + 7u)));
    CHECK(g_psx_ram[0x700000] == 37);
    CHECK(memory_get_scratchpad_ptr()[17] == 19);
    g_psx_ram[0x700000] = 71;
    memory_get_scratchpad_ptr()[17] = 91;
    atomic_store_explicit(&phase, 1, memory_order_release);
    while (atomic_load_explicit(&phase, memory_order_acquire) != 2) { }
    CHECK(g_psx_ram[0x700000] == 71);
    CHECK(i_stat == 0x62 && i_mask == 0x17);
    CHECK(memory_backing_bind(NULL) == copy);
    CHECK(g_psx_ram == live && live[0x700000] == 41);
}
#ifdef _WIN32
static DWORD WINAPI entry(LPVOID p) { (void)p; replay(); return 0; }
#else
static void *entry(void *p) { (void)p; replay(); return NULL; }
#endif
int main(void) {
    live = memory_get_ram_ptr();
    for (size_t i = 0; i < PSX_MAIN_RAM_EXPANDED_BYTES; ++i)
        live[i] = (uint8_t)(i * 13u + 7u);
    live[0x700000] = 37;
    memory_get_scratchpad_ptr()[17] = 19;
    copy = memory_backing_create();
    CHECK(copy);
    memory_backing_capture(copy);
    metadata = memory_execution_create();
    CHECK(metadata);
    i_stat = 0x35; i_mask = 0x17;
    memory_execution_capture(metadata);
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, entry, NULL, 0, NULL);
    CHECK(thread);
#else
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, entry, NULL));
#endif
    while (atomic_load_explicit(&phase, memory_order_acquire) != 1) { }
    CHECK(g_psx_ram == live && live[0x700000] == 37);
    CHECK(memory_get_scratchpad_ptr()[17] == 19);
    live[0x700000] = 41;
    CHECK(i_stat == 0x35 && i_mask == 0x17);
    i_stat = 0x43; i_mask = 0x29;
    atomic_store_explicit(&phase, 2, memory_order_release);
#ifdef _WIN32
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    CHECK(!pthread_join(thread, NULL));
#endif
    CHECK(memory_backing_bind(copy) == NULL);
    uint32_t word = 0;
    CHECK(psx_peek_word(0x80700000, &word) && (word & 255) == 71);
    CHECK(memory_get_scratchpad_ptr()[17] == 91);
    CHECK(memory_backing_bind(NULL) == copy);
    CHECK(i_stat == 0x43 && i_mask == 0x29);
    memory_backing_capture(copy);
    CHECK(memory_backing_bind(copy) == NULL);
    CHECK(g_psx_ram[0x700000] == 41);
    CHECK(memory_get_scratchpad_ptr()[17] == 19);
    CHECK(memory_backing_bind(NULL) == copy);
    memory_execution_destroy(metadata);
    memory_backing_destroy(copy);
    return 0;
}
