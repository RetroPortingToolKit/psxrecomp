/* Feature macros before any system headers (strict C11 / -std=c11). */
#if !defined(_WIN32)
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "host_time.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static LARGE_INTEGER s_qpc_freq;
static INIT_ONCE s_time_once = INIT_ONCE_STATIC_INIT;
static DWORD s_sleep_timer_slot = FLS_OUT_OF_INDEXES;

static void CALLBACK sleep_timer_close(void *timer)
{
    if (timer) CloseHandle((HANDLE)timer);
}

static BOOL CALLBACK time_initialize(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    if (!QueryPerformanceFrequency(&s_qpc_freq)) s_qpc_freq.QuadPart = 0;
    /* An auto-reset timer has one pending deadline and wakes one waiter.
     * Sharing it lets a render/present worker cancel the guest pacer's wait.
     * FLS also releases the handle when its thread/guest fiber is destroyed. */
    s_sleep_timer_slot = FlsAlloc(sleep_timer_close);
    return TRUE;
}

static HANDLE sleep_timer_get(void)
{
    HANDLE timer;
    if (!InitOnceExecuteOnce(&s_time_once, time_initialize, NULL, NULL) ||
        s_sleep_timer_slot == FLS_OUT_OF_INDEXES)
        return NULL;
    timer = (HANDLE)FlsGetValue(s_sleep_timer_slot);
    if (timer) return timer;
    timer =
        CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                               TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (timer && !FlsSetValue(s_sleep_timer_slot, timer)) {
        CloseHandle(timer);
        return NULL;
    }
    return timer;
}

uint64_t psx_host_mono_ms(void)
{
    LARGE_INTEGER counter;

    InitOnceExecuteOnce(&s_time_once, time_initialize, NULL, NULL);
    if (s_qpc_freq.QuadPart == 0)
        return (uint64_t)GetTickCount64();
    if (!QueryPerformanceCounter(&counter))
        return (uint64_t)GetTickCount64();
    return (uint64_t)((counter.QuadPart * 1000ULL) / (uint64_t)s_qpc_freq.QuadPart);
}

void psx_host_sleep_micros(unsigned usec)
{
    LARGE_INTEGER due;
    LONGLONG hundred_ns;
    HANDLE timer;

    if (usec == 0u)
        return;

    timer = sleep_timer_get();
    if (timer) {
        hundred_ns = -((LONGLONG)usec * 10LL);
        if (hundred_ns >= 0)
            hundred_ns = -1LL;
        due.QuadPart = hundred_ns;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
            WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
            return;
        }
    }
    if (usec < 1000u) {
        Sleep(1u);
        return;
    }
    Sleep((DWORD)(usec / 1000u));
}

#else /* !_WIN32 */

#include <errno.h>
#include <time.h>
#include <unistd.h>

uint64_t psx_host_mono_ms(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000ull +
               (uint64_t)ts.tv_nsec / 1000000ull;
#endif
    return (uint64_t)time(NULL) * 1000ull;
}

void psx_host_sleep_micros(unsigned usec)
{
    struct timespec req;
    struct timespec rem;

    if (usec == 0u)
        return;
    req.tv_sec = (time_t)(usec / 1000000u);
    req.tv_nsec = (long)(usec % 1000000u) * 1000L;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR)
            break;
        req = rem;
    }
}

#endif /* _WIN32 */

void psx_host_sleep_ms(unsigned ms)
{
    if (ms == 0u)
        return;
    if (ms > 1000000u)
        ms = 1000000u;
    psx_host_sleep_micros(ms * 1000u);
}
