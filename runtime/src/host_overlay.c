/* host_overlay.c -- see host_overlay.h. */
#include "host_overlay.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

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
 * behind an atomic pointer (release on store, acquire on load), so a reader
 * can never pair a live function with a stale or NULL context. Records are
 * never freed: registration happens a handful of times per process, and a
 * reader may still hold the previous record when it is replaced. */
typedef struct {
    PsxHostOverlayDrawFn fn;
    void *ctx;
} PsxHostOverlayDraw;

static _Atomic(PsxHostOverlayDraw *) s_draw = NULL;

void psx_host_overlay_set_draw_cb(PsxHostOverlayDrawFn fn, void *ctx) {
    PsxHostOverlayDraw *rec = NULL;
    if (fn) {
        rec = (PsxHostOverlayDraw *)malloc(sizeof(*rec));
        if (!rec) return;
        rec->fn = fn;
        rec->ctx = ctx;
    }
    atomic_store_explicit(&s_draw, rec, memory_order_release);
}

int psx_host_overlay_has_draw(void) {
    return atomic_load_explicit(&s_draw, memory_order_acquire) != NULL;
}

void psx_host_overlay_draw(int width, int height) {
    const PsxHostOverlayDraw *rec = atomic_load_explicit(&s_draw, memory_order_acquire);
    if (rec && rec->fn && width > 0 && height > 0) rec->fn(width, height, rec->ctx);
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
    if (a->texture_filter != b->texture_filter) m |= PSX_VIDEO_LIVE_TEXTURE_FILTER;
    if (a->fmv_filter != b->fmv_filter) m |= PSX_VIDEO_LIVE_FMV_FILTER;
    if (a->scanlines != b->scanlines || a->scanline_strength_pct != b->scanline_strength_pct)
        m |= PSX_VIDEO_LIVE_SCANLINES;
    if (a->present_linear != b->present_linear) m |= PSX_VIDEO_LIVE_PRESENT_LINEAR;
    if (a->dynamic_resolution != b->dynamic_resolution) m |= PSX_VIDEO_LIVE_DYNRES;
    if (a->frame_generation != b->frame_generation) m |= PSX_VIDEO_LIVE_FRAME_GEN;
    return m;
}

unsigned psx_host_video_restart_bits(const PsxHostVideoSettings *boot,
                                     const PsxHostVideoSettings *next) {
    unsigned m = 0;
    if (!boot || !next) return 0;
    if (boot->renderer != next->renderer) m |= PSX_VIDEO_RESTART_RENDERER;
    if (boot->internal_resolution != next->internal_resolution ||
        boot->supersampling != next->supersampling)
        m |= PSX_VIDEO_RESTART_RESOLUTION;
    if (boot->render_thread != next->render_thread ||
        boot->present_thread != next->present_thread)
        m |= PSX_VIDEO_RESTART_THREADS;
    return m;
}

int psx_texture_filter_from_ui(int value) {
    return (value >= PSX_TEXTURE_FILTER_NEAREST && value <= PSX_TEXTURE_FILTER_XBR)
               ? value : PSX_TEXTURE_FILTER_NEAREST;
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
