/* host_sampler.c - always-on host CPU sampler for the emulation thread
 * (debug-tools builds; TCP host_profile, tools/host_profile.py).
 *
 * A background thread samples the emulation thread's instruction pointer
 * every millisecond into a ring, tagged with the guest frame and whether a
 * render pass was running. The ring covers the last ~65 seconds, so a probe
 * asks for a window that already happened instead of arming a profiler
 * before a workload. Addresses are stored relative to the executable's load
 * base; tools/host_profile.py maps them to functions with the image's own
 * symbol table (nm), so no debug info is needed.
 *
 * Windows: SuspendThread / GetThreadContext / ResumeThread from a sampler
 * thread. POSIX: the process CPU-time timer (SIGPROF) delivered to the
 * emulation thread, which reads its own interrupted PC from the ucontext.
 * Other hosts, and builds without debug tools, report the sampler as
 * unsupported. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* REG_RIP, dladdr */
#endif
#include "host_sampler.h"

#include <stdint.h>
#include <string.h>

extern int g_psx_render_pass_active;
extern uint64_t s_frame_count;

static HostSample s_ring[HOST_SAMPLER_CAP];
static volatile uint64_t s_seq;
static uintptr_t s_image_base;
static int s_supported;

#ifndef PSX_NO_DEBUG_TOOLS
static void record(uintptr_t pc) {
    const uint64_t n = s_seq;
    HostSample *e = &s_ring[n % HOST_SAMPLER_CAP];
    e->rva = (pc >= s_image_base) ? (uint64_t)(pc - s_image_base) : 0;
    e->frame = (uint32_t)s_frame_count;
    e->in_pass = g_psx_render_pass_active ? 1u : 0u;
    s_seq = n + 1;
}
#endif

#if defined(PSX_NO_DEBUG_TOOLS)
/* Production builds carry no sampler: it reports unsupported. */
void host_sampler_start(void) {}

#elif defined(_WIN32)
#include <windows.h>

static HANDLE s_target;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    /* A 1 ms period needs a high-resolution timer; Sleep(1) would follow
     * whatever timer resolution the process last asked for. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms, relative */
        if (!SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) {
            CloseHandle(timer);
            timer = NULL;
        }
    }
    for (;;) {
        if (timer) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        if (SuspendThread(s_target) == (DWORD)-1) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_target, &ctx)) {
#if defined(_M_X64) || defined(__x86_64__)
            record((uintptr_t)ctx.Rip);
#elif defined(_M_ARM64) || defined(__aarch64__)
            record((uintptr_t)ctx.Pc);
#endif
        }
        ResumeThread(s_target);
    }
    return 0;
}

void host_sampler_start(void) {
    if (s_supported) return;
    s_image_base = (uintptr_t)GetModuleHandleW(NULL);
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    HANDLE t = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (!t) return;
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
    s_supported = 1;
}

#elif defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <pthread.h>
#include <dlfcn.h>

static pthread_t s_thread;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    if (!pthread_equal(pthread_self(), s_thread)) return;
    ucontext_t *uc = (ucontext_t *)uc_;
#if defined(__linux__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext.pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__rip);
#elif defined(__APPLE__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__pc);
#else
    (void)uc;
#endif
}

void host_sampler_start(void) {
    if (s_supported) return;
    Dl_info info;
    if (dladdr((void *)&host_sampler_start, &info) && info.dli_fbase)
        s_image_base = (uintptr_t)info.dli_fbase;
    s_thread = pthread_self();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) return;
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0) return;
    s_supported = 1;
}

#else
void host_sampler_start(void) {}
#endif

int host_sampler_supported(void) { return s_supported; }

uint64_t host_sampler_seq(void) { return s_seq; }

uint64_t host_sampler_image_base(void) { return (uint64_t)s_image_base; }

int host_sampler_get(uint64_t seq, HostSample *out) {
    const uint64_t now = s_seq;
    if (seq >= now || now - seq > HOST_SAMPLER_CAP) return 0;
    *out = s_ring[seq % HOST_SAMPLER_CAP];
    return 1;
}
