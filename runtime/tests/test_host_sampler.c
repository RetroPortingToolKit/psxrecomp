#include "host_sampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t s_frame_count;
int g_psx_render_pass_active;
volatile uint32_t g_psx_last_fn_entry;
static volatile int in_exception;
int psx_get_in_exception(void) { return in_exception; }

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } \
} while (0)

static void busy_interval(void) {
    const clock_t end = clock() + CLOCKS_PER_SEC / 10;
    volatile uint32_t value = 1;
    while (clock() < end) value = value * 1664525u + 1013904223u;
}

#if defined(_WIN32) && !(defined(PSX_DISABLE_HOST_SAMPLER) && PSX_DISABLE_HOST_SAMPLER)
/* Registered before host_sampler_start, so it runs after the sampler's own
 * atexit handler. exit() then terminates the sampler thread; if it still
 * suspends this thread at that point, the process can stay suspended for
 * good (PS1B-674: a Spyro recording that never exited). */
static void sampler_stopped_at_exit(void) {
    const uint64_t at_exit = host_sampler_seq();
    busy_interval();
    if (host_sampler_seq() != at_exit) {
        fprintf(stderr, "FAIL: the sampler still suspends the exiting thread during exit (%llu samples)\n",
                (unsigned long long)(host_sampler_seq() - at_exit));
        fflush(stderr);
        _Exit(1);
    }
    puts("PASS: the sampler is stopped before exit terminates its thread");
    fflush(stdout);
}
#endif

int main(void) {
    HostSample sample;
#if defined(_WIN32) && !(defined(PSX_DISABLE_HOST_SAMPLER) && PSX_DISABLE_HOST_SAMPLER)
    atexit(sampler_stopped_at_exit);
#endif
    CHECK(!host_sampler_get(0, &sample));
    s_frame_count = 42;
    g_psx_last_fn_entry = 0x80010000u;
    host_sampler_start();
#if defined(PSX_DISABLE_HOST_SAMPLER) && PSX_DISABLE_HOST_SAMPLER
    CHECK(!host_sampler_supported() && !host_sampler_seq());
#elif defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    CHECK(host_sampler_supported());
    busy_interval();
    const uint64_t first_end = host_sampler_seq();
    CHECK(first_end > 0);
    s_frame_count = 43;
    g_psx_last_fn_entry = 0x80020000u;
    g_psx_render_pass_active = 1;
    in_exception = 1;
    busy_interval();
    const uint64_t end = host_sampler_seq();
    CHECK(end > first_end);
    int before = 0, after = 0;
    for (uint64_t i = 0; i < end; ++i) {
        CHECK(host_sampler_get(i, &sample));
        if (sample.frame == 42 && sample.guest_pc == 0x80010000u &&
            sample.in_pass == 0 && sample.flags == 0) ++before;
        if (sample.frame == 43 && sample.guest_pc == 0x80020000u &&
            sample.in_pass == 1 && sample.flags == HOST_SAMPLE_EXCEPTION) ++after;
    }
    CHECK(before > 0 && after > 0);
    CHECK(!host_sampler_get(host_sampler_seq() + 1u, &sample));
#else
    CHECK(!host_sampler_supported() && !host_sampler_seq());
#endif
    if (failures) return 1;
    puts("PASS: Release sampler retains pre-client history and guest/exception context");
    return 0;
}
