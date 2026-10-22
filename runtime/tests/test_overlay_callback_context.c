/* A background initializer must not publish its TLS addresses into the
 * emulation owner's callback table, even after the initializer exits. */
#include "overlay_loader.h"
#include "overlay_api.h"
#include "psx_thread_local.h"
#include <stdlib.h>
#include <pthread.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
PSX_THREAD_LOCAL uint32_t g_debug_last_store_pc;
PSX_THREAD_LOCAL int g_psx_call_bail;
PSX_THREAD_LOCAL uint64_t g_psx_bail_first, g_psx_bail_resolved;
static const OverlayCallbacks *live;
static void *initializer(void *unused) {
    (void)unused;
    const OverlayCallbacks *background = overlay_loader_callbacks();
    CHECK(background != live);
    CHECK(background->last_store_pc == &g_debug_last_store_pc);
    CHECK(background->call_bail_flag == &g_psx_call_bail);
    *background->last_store_pc = 29;
    *background->call_bail_flag = 31;
    return NULL;
}
int main(void) {
    live = overlay_loader_callbacks();
    g_debug_last_store_pc = 17; g_psx_call_bail = 19;
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, initializer, NULL));
    CHECK(!pthread_join(thread, NULL));
    CHECK(live->last_store_pc == &g_debug_last_store_pc);
    CHECK(live->call_bail_flag == &g_psx_call_bail);
    CHECK(*live->last_store_pc == 17 && *live->call_bail_flag == 19);
    return 0;
}
