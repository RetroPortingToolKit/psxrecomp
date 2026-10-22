#include "mod_execution.h"
#include "mod_plugins.h"
#include "psx_thread_local.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
static PSX_THREAD_LOCAL struct { int enabled; unsigned values[256]; } state;
static ModExecutionSnapshot *image;
static atomic_int phase;
static void capture(void *out) { memcpy(out, &state, sizeof state); }
static void bind(const void *in) { memcpy(&state, in, sizeof state); }
static void *worker(void *unused) {
    (void)unused;
    mod_execution_bind(image);
    CHECK(state.enabled && state.values[93] == 17);
    state.values[93] = 91;
    atomic_store_explicit(&phase, 1, memory_order_release);
    while (atomic_load_explicit(&phase, memory_order_acquire) != 2) { }
    CHECK(state.values[93] == 91);
    mod_execution_bind(image);
    CHECK(state.values[93] == 23);
    return NULL;
}
int main(void) {
    const PSXModExecutionState callbacks = {sizeof(callbacks), "fixture", sizeof state, capture, bind};
    const PSXModExecutionState stateless = {sizeof(stateless), "stateless", 0, NULL, NULL};
    CHECK(psx_mod_register_execution_state(&callbacks));
    CHECK(psx_mod_register_execution_state(&stateless));
    CHECK(!psx_mod_register_execution_state(&callbacks));
    state.enabled = 1; state.values[93] = 17;
    image = mod_execution_create(); CHECK(image);
    mod_execution_capture(image);
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, worker, NULL));
    while (!atomic_load_explicit(&phase, memory_order_acquire)) { }
    CHECK(state.values[93] == 17);
    state.values[93] = 23; mod_execution_capture(image);
    atomic_store_explicit(&phase, 2, memory_order_release);
    CHECK(!pthread_join(thread, NULL));
    CHECK(state.values[93] == 23);
    mod_execution_destroy(image);
    CHECK(!psx_mod_register_execution_state(&callbacks));
    return 0;
}
