/* host_overlay.c -- see host_overlay.h. */
#include "host_overlay.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#endif

/* ---- P1: host pause ------------------------------------------------------ */
#define PSX_HOST_PAUSE_MAX 8
static int s_pause_depth = 0;
static const char *s_pause_reason[PSX_HOST_PAUSE_MAX];
static PsxHostPauseRefuseFn s_pause_refuse = NULL;

int psx_host_pause_push(const char *reason) {
    if (s_pause_refuse && s_pause_refuse()) return 0;
    if (s_pause_depth >= PSX_HOST_PAUSE_MAX) return 0;
    s_pause_reason[s_pause_depth++] = reason ? reason : "host";
    return 1;
}

void psx_host_pause_pop(void) {
    if (s_pause_depth > 0) s_pause_reason[--s_pause_depth] = NULL;
}

int psx_host_pause_depth(void) { return s_pause_depth; }

const char *psx_host_pause_reason(void) {
    return s_pause_depth > 0 ? s_pause_reason[s_pause_depth - 1] : NULL;
}

void psx_host_pause_set_refuse_probe(PsxHostPauseRefuseFn fn) { s_pause_refuse = fn; }

/* ---- P2: overlay draw callback -------------------------------------------- */
/* Registered on the main thread, read on the render / present thread. The
 * function and its context are published together as one immutable record
 * behind an atomic pointer, so a reader never pairs a live function with a
 * stale context.
 *
 * Lifetime: every call runs inside an in-flight count. Replacing or removing
 * the callback swaps the record and then waits until no call that could have
 * read the old record is still running, so when psx_host_overlay_set_draw_cb
 * returns, the previous fn will not run again and its ctx (and the GL objects
 * it draws with) may be freed by the caller. The old record is freed there
 * too. A change made from inside the callback itself does not wait (it would
 * wait for itself); that caller's own frame is the only one still using the
 * old ctx and it returns before the next frame reads the new record. */
typedef struct {
    PsxHostOverlayDrawFn fn;
    void *ctx;
} PsxHostOverlayDraw;

static _Atomic(PsxHostOverlayDraw *) s_draw = NULL;
static atomic_int s_draw_inflight = 0;
#if defined(_MSC_VER)
static __declspec(thread) int s_in_draw = 0;
#else
static _Thread_local int s_in_draw = 0;
#endif

static void draw_yield(void) {
#if defined(_WIN32)
    Sleep(0);
#else
    sched_yield();
#endif
}

void psx_host_overlay_set_draw_cb(PsxHostOverlayDrawFn fn, void *ctx) {
    PsxHostOverlayDraw *rec = NULL, *old;
    if (fn) {
        rec = (PsxHostOverlayDraw *)malloc(sizeof(*rec));
        if (!rec) return;
        rec->fn = fn;
        rec->ctx = ctx;
    }
    old = atomic_exchange(&s_draw, rec); /* seq_cst: ordered with the readers' count */
    if (s_in_draw) return; /* from inside the callback: see above; old stays (rare) */
    while (atomic_load(&s_draw_inflight) > 0) draw_yield();
    free(old);
}

int psx_host_overlay_has_draw(void) {
    return atomic_load_explicit(&s_draw, memory_order_acquire) != NULL;
}

void psx_host_overlay_draw(int width, int height) {
    PsxHostOverlayDraw *rec;
    if (width <= 0 || height <= 0) return;
    atomic_fetch_add(&s_draw_inflight, 1); /* seq_cst, before reading the record */
    rec = atomic_load(&s_draw);
    if (rec && rec->fn) {
        s_in_draw = 1;
        rec->fn(width, height, rec->ctx);
        s_in_draw = 0;
    }
    atomic_fetch_sub(&s_draw_inflight, 1);
}

/* ---- Game HUD layer ------------------------------------------------------- */
static PsxHostOverlayDrawFn s_hud_fn = NULL;
static void *s_hud_ctx = NULL;
static atomic_flag s_hud_lock = ATOMIC_FLAG_INIT;
static atomic_int s_hud_inflight = 0;
#if defined(_MSC_VER)
static __declspec(thread) int s_in_hud = 0;
#else
static _Thread_local int s_in_hud = 0;
#endif

static void hud_lock(void) {
    while (atomic_flag_test_and_set_explicit(&s_hud_lock, memory_order_acquire))
        draw_yield();
}
static void hud_unlock(void) {
    atomic_flag_clear_explicit(&s_hud_lock, memory_order_release);
}

void psx_host_set_hud_draw_cb(PsxHostOverlayDrawFn fn, void *ctx) {
    hud_lock();
    s_hud_ctx = ctx;
    s_hud_fn = fn;
    hud_unlock();
    /* Match the overlay lifetime contract without allocating callback records.
     * The single GL owner may unregister itself; its current call then ends
     * before another frame can consume the replacement. */
    if (!s_in_hud)
        while (atomic_load(&s_hud_inflight) > 0) draw_yield();
}

void psx_host_hud_draw(int width, int height) {
    if (width <= 0 || height <= 0) return;
    hud_lock();
    PsxHostOverlayDrawFn fn = s_hud_fn;
    void *ctx = s_hud_ctx;
    atomic_fetch_add(&s_hud_inflight, 1);
    hud_unlock();
    if (fn) {
        s_in_hud = 1;
        fn(width, height, ctx);
        s_in_hud = 0;
    }
    atomic_fetch_sub(&s_hud_inflight, 1);
}

/* ---- P3: input sink -------------------------------------------------------- */
static PsxHostInputSinkFn s_sink_fn = NULL;
static void *s_sink_ctx = NULL;
static volatile int s_ui_capture = 0;

void psx_host_set_input_sink(PsxHostInputSinkFn fn, void *ctx) {
    s_sink_fn = fn;
    s_sink_ctx = ctx;
}

int psx_host_input_sink_dispatch(const void *event) {
    return (s_sink_fn && event) ? (s_sink_fn(event, s_sink_ctx) != 0) : 0;
}

void psx_host_set_ui_capture(int on) { s_ui_capture = on ? 1 : 0; }
int psx_host_ui_capture_active(void) { return s_ui_capture; }

/* ---- P4: live video settings ------------------------------------------------ */
unsigned psx_host_video_live_changes(const PsxHostVideoSettings *a,
                                     const PsxHostVideoSettings *b) {
    unsigned m = 0;
    if (!a || !b) return 0;
    if (a->fullscreen != b->fullscreen) m |= PSX_VIDEO_LIVE_FULLSCREEN;
    if (a->frame_generation != b->frame_generation) m |= PSX_VIDEO_LIVE_FRAME_GEN;
    if (a->internal_resolution != b->internal_resolution ||
        a->supersample_milli != b->supersample_milli)
        m |= PSX_VIDEO_LIVE_RESOLUTION;
    return m;
}

unsigned psx_host_video_restart_bits(const PsxHostVideoSettings *boot,
                                     const PsxHostVideoSettings *next) {
    unsigned m = 0;
    if (!boot || !next) return 0;
    if (boot->renderer != next->renderer) m |= PSX_VIDEO_RESTART_RENDERER;
    /* Internal resolution and supersample apply live; only the legacy
     * integer factor still needs a relaunch. */
    if (boot->supersampling != next->supersampling)
        m |= PSX_VIDEO_RESTART_RESOLUTION;
    if (boot->render_thread != next->render_thread ||
        boot->present_thread != next->present_thread)
        m |= PSX_VIDEO_RESTART_THREADS;
    return m;
}

unsigned psx_host_video_restart_bits_backend(const PsxHostVideoSettings *boot,
                                             const PsxHostVideoSettings *next,
                                             int res_live_ok) {
    unsigned m = psx_host_video_restart_bits(boot, next);
    if (boot && next && !res_live_ok &&
        (boot->internal_resolution != next->internal_resolution ||
         boot->supersample_milli != next->supersample_milli))
        m |= PSX_VIDEO_RESTART_RESOLUTION;
    return m;
}

/* ---- seamless relaunch ----------------------------------------------------- */
#include <stdlib.h>
#include <string.h>

/* Windows command-line quoting (the rules CommandLineToArgvW and the MSVC CRT
 * parse with): an argument is quoted when empty or holding a space, tab or
 * quote; inside quotes, backslashes are literal except before a quote, so a
 * run of n backslashes before a quote becomes 2n+1 plus the quote, and a run
 * before the closing quote becomes 2n. */
size_t psx_win_cmdline_quote(const char* arg, char* out, size_t cap) {
    size_t n = 0, i, bs;
    const char* p;
#define PUT(c) do { if (out && n + 1 < cap) out[n] = (c); ++n; } while (0)
    if (!arg) arg = "";
    if (*arg && !strpbrk(arg, " \t\n\v\"")) {
        for (p = arg; *p; ++p) PUT(*p);
    } else {
        PUT('"');
        for (p = arg;; ++p) {
            bs = 0;
            while (*p == '\\') { ++bs; ++p; }
            if (!*p) { for (i = 0; i < bs * 2; ++i) PUT('\\'); break; }
            if (*p == '"') { for (i = 0; i < bs * 2 + 1; ++i) PUT('\\'); PUT('"'); continue; }
            for (i = 0; i < bs; ++i) PUT('\\');
            PUT(*p);
        }
        PUT('"');
    }
#undef PUT
    if (out && cap) out[n < cap ? n : cap - 1] = '\0';
    return n;
}

char* psx_win_cmdline_join(int argc, const char* const* argv) {
    size_t total = 1;
    int i;
    char* out;
    char* w;
    for (i = 0; i < argc; ++i) total += psx_win_cmdline_quote(argv[i], NULL, 0) + 1;
    out = (char*)malloc(total);
    if (!out) return NULL;
    w = out;
    for (i = 0; i < argc; ++i) {
        if (i) *w++ = ' ';
        w += psx_win_cmdline_quote(argv[i], w, total - (size_t)(w - out));
    }
    *w = '\0';
    return out;
}

#if defined(_WIN32)
#include <stdio.h>
#include <windows.h>
#include <shellapi.h>
/* Re-run this exe with the original arguments. The arguments come from
 * GetCommandLineW (full Unicode, no ANSI code-page loss, any count or length),
 * are re-quoted by the rules above, and the new process is started before
 * this one exits. argc/argv are only a fallback. */
static wchar_t* utf8_to_wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = n > 0 ? (wchar_t*)malloc(sizeof(wchar_t) * (size_t)n) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); w = NULL; }
    return w;
}
static char* wide_to_utf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char* u = n > 0 ? (char*)malloc((size_t)n) : NULL;
    if (u && WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL) <= 0) { free(u); u = NULL; }
    return u;
}
void psx_relaunch_self(int argc, char** argv) {
    int wargc = 0, i, ok = 1;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    const char** args = NULL;
    char* line = NULL;
    wchar_t* wline = NULL;
    wchar_t* exe = NULL;
    DWORD cap = MAX_PATH, got = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (wargv && wargc > 0) {
        args = (const char**)calloc((size_t)wargc, sizeof(*args));
        for (i = 0; args && i < wargc; ++i)
            if (!(args[i] = wide_to_utf8(wargv[i]))) ok = 0;
        if (args && ok) line = psx_win_cmdline_join(wargc, args);
        for (i = 0; args && i < wargc; ++i) free((void*)args[i]);
        free((void*)args);
        LocalFree(wargv);
    } else if (argv) {
        line = psx_win_cmdline_join(argc, (const char* const*)argv);
    }
    if (!line) return;
    wline = utf8_to_wide(line);
    free(line);
    for (;;) {  /* long paths: grow until the module path fits */
        free(exe);
        exe = (wchar_t*)malloc(sizeof(wchar_t) * cap);
        if (!exe) break;
        got = GetModuleFileNameW(NULL, exe, cap);
        if (got && got < cap) break;
        if (!got || cap >= 32768) { free(exe); exe = NULL; break; }
        cap *= 2;
    }
    if (wline && exe) {
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        fflush(NULL);
        if (CreateProcessW(exe, wline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            free(wline);
            free(exe);
            _exit(0);
        }
    }
    free(wline);
    free(exe);
}
#else
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
void psx_relaunch_self(int argc, char** argv) {
    char path[4096];
    (void)argc;
    if (!argv || !argv[0]) return;
    path[0] = '\0';
#if defined(__APPLE__)
    {
        uint32_t cap = sizeof(path);
        if (_NSGetExecutablePath(path, &cap) != 0) path[0] = '\0';
    }
#elif defined(__linux__)
    {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        path[n > 0 ? n : 0] = '\0';
    }
#endif
    /* Interval timers and the signal mask survive exec: a still-armed
     * profiling/watchdog timer would kill the new image (default SIGPROF)
     * before it reaches main. */
    {
        struct itimerval off;
        sigset_t none;
        memset(&off, 0, sizeof(off));
        setitimer(ITIMER_REAL, &off, NULL);
        setitimer(ITIMER_VIRTUAL, &off, NULL);
        setitimer(ITIMER_PROF, &off, NULL);
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
    }
    fflush(NULL);
    execv(path[0] ? path : argv[0], argv);
}
#endif
