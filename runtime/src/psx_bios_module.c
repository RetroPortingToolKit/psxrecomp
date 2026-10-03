/* psx_bios_module.c — host side of loadable BIOS backends (psx_bios_module.h).
 *
 * A bundled release links OpenBIOS only. When the player picks a retail dump
 * whose identity matches a shipped profile, this file (1) looks for a module
 * already built for that dump under <exe>/cache/bios, (2) otherwise runs the
 * bundled overlay toolchain's bios_module_build.py to emit and compile one
 * from the dump, then (3) loads it overlay-style — ABI and codegen-hash
 * gated, callback tables handed over at init — and registers its descriptor
 * so the ordinary selection code (main.cpp) treats it as one more linked
 * backend.
 *
 * Nothing here formats the overlay cache tag; the BIOS module directory has
 * its own shape (bm<abi>_<codegen hash>_f<flavor>), written here and read
 * back by bios_module_build.py --print-cache-dir, which are the only two
 * places that know it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#else
#  include <sys/wait.h>
#  include "overlay_posix.h"
#endif

#include "autocompile.h"
#include "cpu_state.h"
#include "crc32.h"
#include "debug_server.h"
#include "dirty_ram_interp.h"
#include "fntrace.h"
#include "overlay_api.h"
#include "overlay_loader.h"
#include "psx_bios_backend.h"
#include "psx_bios_known_images.h"
#include "psx_bios_module.h"
#include "psx_memory.h"
#include "psx_segment_miss.h"

#if defined(_WIN32)
#  define PSX_BM_EXT "dll"
#else
#  define PSX_BM_EXT "so"
#endif

static char s_last_msg[512];
static PsxBiosModuleProgressFn s_progress_fn;
static void *s_progress_ctx;

void psx_bios_module_set_progress(PsxBiosModuleProgressFn fn, void *ctx) {
    s_progress_fn = fn;
    s_progress_ctx = ctx;
}

/* Builder output -> progress. Stage boundaries come from the lines
 * bios_module_build.py prints at each step; everything else is indeterminate. */
static void report_line(const char *line) {
    float pct = -1.0f;
    const char *msg = line;
    while (*msg == ' ') msg++;
    if (!*msg || strstr(msg, "opcode 0x2F")) return;   /* emitter noise */
    if (!strncmp(msg, "dump:", 5))             pct = 0.05f;
    else if (!strncmp(msg, "emit:", 5))        pct = 0.15f;
    else if (!strncmp(msg, "compile:", 8))     pct = 0.30f;
    else if (!strncmp(msg, "self-check:", 11)) pct = 0.90f;
    else if (!strncmp(msg, "PSX_BIOS_MODULE_PUBLISHED", 25)) pct = 1.0f;
    if (s_progress_fn) {
        char trimmed[200];
        size_t n = strlen(msg);
        while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
        if (n >= sizeof(trimmed)) n = sizeof(trimmed) - 1;
        memcpy(trimmed, msg, n);
        trimmed[n] = '\0';
        /* The compile line is a 500-character command; say what it means. */
        if (!strncmp(trimmed, "compile:", 8)) strcpy(trimmed, "Compiling the BIOS backend (this is the long step)…");
        else if (!strncmp(trimmed, "emit:", 5)) strcpy(trimmed, "Recompiling your BIOS image…");
        else if (!strncmp(trimmed, "self-check:", 11)) strcpy(trimmed, "Checking the built backend…");
        else if (!strncmp(trimmed, "PSX_BIOS_MODULE_PUBLISHED", 25)) strcpy(trimmed, "BIOS backend ready.");
        s_progress_fn(s_progress_ctx, pct, trimmed);
    }
}

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
    fprintf(stdout, "psx_bios_module: %s\n", s_last_msg);
    fflush(stdout);
}

const char *psx_bios_module_last_message(void) { return s_last_msg; }

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) {
        strncpy(err, msg, cap - 1);
        err[cap - 1] = '\0';
    }
}

static int file_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && (st.st_mode & S_IFREG);
}

static int dir_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

/* ---- Toolchain beside the executable ------------------------------------- */
typedef struct {
    char dir[1024];
    char python[1100];
    char builder[1100];
    char emitter[1100];
    char tcc[1100];
    char gcc[1100];      /* compiler.txt, or "" */
    int  has_tcc;
    int  has_gcc;        /* compiler.txt or a cc on PATH */
} BmToolchain;

static int probe_toolchain(const char *exe_dir, BmToolchain *tk) {
    memset(tk, 0, sizeof(*tk));
    if (!exe_dir || !exe_dir[0]) return 0;
    snprintf(tk->dir, sizeof(tk->dir), "%s/overlay_toolchain", exe_dir);
#if defined(_WIN32)
    snprintf(tk->python, sizeof(tk->python), "%s/python/python.exe", tk->dir);
    snprintf(tk->emitter, sizeof(tk->emitter), "%s/psxrecomp-bios.exe", tk->dir);
    snprintf(tk->tcc, sizeof(tk->tcc), "%s/tcc/tcc.exe", tk->dir);
#else
    snprintf(tk->python, sizeof(tk->python), "%s/python/bin/python3", tk->dir);
    snprintf(tk->emitter, sizeof(tk->emitter), "%s/psxrecomp-bios", tk->dir);
    snprintf(tk->tcc, sizeof(tk->tcc), "%s/tcc/tcc", tk->dir);
#endif
    snprintf(tk->builder, sizeof(tk->builder), "%s/bios_module_build.py", tk->dir);
    if (!file_exists(tk->python) || !file_exists(tk->builder) || !file_exists(tk->emitter))
        return 0;
    tk->has_tcc = file_exists(tk->tcc);
    {
        char cf[1100];
        FILE *f;
        snprintf(cf, sizeof(cf), "%s/compiler.txt", tk->dir);
        f = fopen(cf, "r");
        if (f) {
            if (fgets(tk->gcc, sizeof(tk->gcc), f)) {
                size_t n = strlen(tk->gcc);
                while (n && (tk->gcc[n - 1] == '\n' || tk->gcc[n - 1] == '\r' ||
                             tk->gcc[n - 1] == ' '))
                    tk->gcc[--n] = '\0';
                if (!file_exists(tk->gcc)) tk->gcc[0] = '\0';
            }
            fclose(f);
        }
    }
    tk->has_gcc = tk->gcc[0] || autocompile_toolchain_available();
    return tk->has_gcc || tk->has_tcc;
}

int psx_bios_module_supported(const char *exe_dir) {
    BmToolchain tk;
    if (!psx_bios_bundled()) return 0;   /* setup host: nothing to add to */
    return probe_toolchain(exe_dir, &tk);
}

const char *psx_bios_module_known_id(uint32_t crc32, uint32_t size) {
    size_t i;
    for (i = 0; i < sizeof(psx_known_bios_images) / sizeof(psx_known_bios_images[0]); ++i) {
        if (psx_known_bios_images[i].crc32 == crc32 && psx_known_bios_images[i].size == size)
            return psx_known_bios_images[i].id;
    }
    return 0;
}

static const PsxKnownBiosImage *known_for(uint32_t crc32, uint32_t size) {
    size_t i;
    for (i = 0; i < sizeof(psx_known_bios_images) / sizeof(psx_known_bios_images[0]); ++i) {
        if (psx_known_bios_images[i].crc32 == crc32 && psx_known_bios_images[i].size == size)
            return &psx_known_bios_images[i];
    }
    return 0;
}

/* ---- Cache layout --------------------------------------------------------- */
int psx_bios_module_cache_path(const char *exe_dir, const char *stem,
                               uint32_t crc32, char *out, size_t cap) {
    int n;
    if (!exe_dir || !stem || !out || !cap) return 0;
    n = snprintf(out, cap, "%s/cache/bios/%s/bm%d_%08x_f%d/%s_%08X." PSX_BM_EXT,
                 exe_dir, overlay_loader_arch_abi(),
                 PSX_BIOS_MODULE_ABI_VERSION, (unsigned)PSX_OVERLAY_CODEGEN_HASH,
                 (int)PSX_OVERLAY_FLAVOR, stem, crc32);
    return n > 0 && (size_t)n < cap;
}

/* ---- Callback tables ------------------------------------------------------ */
void psx_bios_module_fill_callbacks(PsxBiosModuleCallbacks *out) {
    extern int (*g_psx_bios_hle_hook)(struct CPUState *cpu, uint32_t phys);
    extern uint64_t g_dispatch_static_hits;
    extern uint64_t g_psx_bail_flattened;
    extern int psx_game_address_in_text(uint32_t addr);
    extern uint64_t s_frame_count;
    memset(out, 0, sizeof(*out));
    out->size = (uint32_t)sizeof(*out);
    out->arith_overflow            = psx_arith_overflow;
    out->brk                       = psx_break;
    out->unaligned_access          = psx_unaligned_access;
    out->rfe_escape_check          = psx_rfe_escape_check;
    out->dirty_ram_dispatch        = dirty_ram_dispatch;
    out->dirty_ram_is_dirty        = dirty_ram_is_dirty;
    out->dirty_ram_text_native_ok  = dirty_ram_text_native_ok;
    out->kernel_bless_dispatchable = psx_kernel_bless_dispatchable;
    out->game_address_in_text      = psx_game_address_in_text;
    out->fntrace_is_game_started   = fntrace_is_game_started;
    out->fntrace_record            = fntrace_record;
    out->trace_dispatch            = debug_server_trace_dispatch;
    out->log_probe                 = debug_server_log_probe;
    out->segment_miss_record_kind  = psx_segment_miss_record_kind;
    out->hle_hook                  = &g_psx_bios_hle_hook;
    out->dispatch_depth            = &g_psx_dispatch_depth;
    out->static_hits               = &g_dispatch_static_hits;
    out->current_func_addr         = &g_debug_current_func_addr;
    out->bail_flattened            = &g_psx_bail_flattened;
    out->ram_mask                  = &g_psx_ram_mask;
    out->frame_count               = &s_frame_count;
}

/* ---- Loading -------------------------------------------------------------- */
typedef int (*BmAbiFn)(void);
typedef uint32_t (*BmHashFn)(void);
typedef int (*BmInitFn)(const OverlayCallbacks *, const PsxBiosModuleCallbacks *);
typedef const PsxBiosBackend *(*BmBackendFn)(void);

#if defined(_WIN32)
typedef HMODULE BmHandle;
static BmHandle bm_open(const char *path, char *err, size_t cap) {
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        char b[256];
        snprintf(b, sizeof(b), "LoadLibrary failed (%lu)", (unsigned long)GetLastError());
        set_err(err, cap, b);
    }
    return h;
}
static void *bm_sym(BmHandle h, const char *name) { return (void *)GetProcAddress(h, name); }
static void bm_close(BmHandle h) { FreeLibrary(h); }
#else
typedef void *BmHandle;
static BmHandle bm_open(const char *path, char *err, size_t cap) {
    char e[256];
    void *h = psx_overlay_posix_library_open(path, e, sizeof(e));
    if (!h) set_err(err, cap, e);
    return h;
}
static void *bm_sym(BmHandle h, const char *name) { return psx_overlay_posix_library_symbol(h, name); }
static void bm_close(BmHandle h) { psx_overlay_posix_library_close(h); }
#endif

const PsxBiosBackend *psx_bios_module_load(const char *path, char *err, size_t err_cap) {
    BmHandle h;
    BmAbiFn abi_fn;
    BmHashFn hash_fn;
    BmInitFn init_fn;
    BmBackendFn backend_fn;
    const PsxBiosBackend *b;
    PsxBiosModuleCallbacks bcbs;
    char e[256] = {0};

    if (!file_exists(path)) { set_err(err, err_cap, "module file missing"); return 0; }
    h = bm_open(path, e, sizeof(e));
    if (!h) { set_err(err, err_cap, e); return 0; }

    abi_fn     = (BmAbiFn)bm_sym(h, "psx_bios_module_abi");
    hash_fn    = (BmHashFn)bm_sym(h, "psx_bios_module_codegen_hash");
    init_fn    = (BmInitFn)bm_sym(h, "psx_bios_module_init");
    backend_fn = (BmBackendFn)bm_sym(h, "psx_bios_module_backend");
    if (!abi_fn || !hash_fn || !init_fn || !backend_fn) {
        bm_close(h);
        set_err(err, err_cap, "not a BIOS module (exports missing)");
        return 0;
    }
    if (abi_fn() != PSX_BIOS_MODULE_ABI_TAG) {
        char b[160];
        snprintf(b, sizeof(b), "ABI tag %08x != host %08x", (unsigned)abi_fn(),
                 (unsigned)PSX_BIOS_MODULE_ABI_TAG);
        bm_close(h);
        set_err(err, err_cap, b);
        return 0;
    }
    if (hash_fn() != (uint32_t)PSX_OVERLAY_CODEGEN_HASH) {
        char b[160];
        snprintf(b, sizeof(b), "codegen hash %08x != host %08x", (unsigned)hash_fn(),
                 (unsigned)PSX_OVERLAY_CODEGEN_HASH);
        bm_close(h);
        set_err(err, err_cap, b);
        return 0;
    }
    psx_bios_module_fill_callbacks(&bcbs);
    if (!init_fn(overlay_loader_callbacks(), &bcbs)) {
        bm_close(h);
        set_err(err, err_cap, "module init refused the callback tables");
        return 0;
    }
    b = backend_fn();
    if (!b || !b->image || !b->dispatch || !b->dispatch_call) {
        bm_close(h);
        set_err(err, err_cap, "module descriptor incomplete");
        return 0;
    }
    if (!psx_bios_register(b)) {
        bm_close(h);
        set_err(err, err_cap, "backend registry full");
        return 0;
    }
    /* The handle is never closed: the registry and, once selected, the
     * published psx_bios_image/kernel tables point into the module. */
    note("loaded %s (%s, CRC %08X)", path, b->image->image_id ? b->image->image_id : "?",
         (unsigned)b->image->image_crc32);
    return b;
}

/* ---- Building -------------------------------------------------------------- */
static int mkdirs(const char *path) {
    char buf[1200];
    size_t i, n = strlen(path);
    if (n >= sizeof(buf)) return 0;
    memcpy(buf, path, n + 1);
    for (i = 1; i < n; ++i) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i];
            buf[i] = '\0';
#if defined(_WIN32)
            _mkdir(buf);
#else
            mkdir(buf, 0755);
#endif
            buf[i] = c;
        }
    }
    return 1;
}

/* Run a command line, streaming its output to our stdout, and return its
 * exit status (or -1 when it could not be started). Windows: no console
 * window; the executable is a GUI-subsystem process. */
static int run_command(const char *cmd) {
#if defined(_WIN32)
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    char *mutable_cmd;
    DWORD code = (DWORD)-1;
    size_t n = strlen(cmd);
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    memset(&pi, 0, sizeof(pi));
    mutable_cmd = (char *)malloc(n + 1);
    if (!mutable_cmd) { CloseHandle(rd); CloseHandle(wr); return -1; }
    memcpy(mutable_cmd, cmd, n + 1);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        NULL, NULL, &si, &pi)) {
        free(mutable_cmd);
        CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    free(mutable_cmd);
    CloseHandle(wr);
    {
        char buf[512];
        char line[1024];
        size_t ln = 0;
        DWORD got;
        while (ReadFile(rd, buf, sizeof(buf) - 1, &got, NULL) && got) {
            DWORD i;
            buf[got] = '\0';
            fputs(buf, stdout);
            for (i = 0; i < got; i++) {
                if (buf[i] == '\n' || ln + 1 >= sizeof(line)) {
                    line[ln] = '\0';
                    report_line(line);
                    ln = 0;
                } else {
                    line[ln++] = buf[i];
                }
            }
        }
        if (ln) { line[ln] = '\0'; report_line(line); }
        fflush(stdout);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return (int)code;
#else
    FILE *p = popen(cmd, "r");
    char buf[1024];
    int status;
    if (!p) return -1;
    while (fgets(buf, sizeof(buf), p)) {
        fputs(buf, stdout);
        report_line(buf);
    }
    fflush(stdout);
    status = pclose(p);
    if (status == -1) return -1;
#  if defined(WIFEXITED)
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#  else
    return status;
#  endif
#endif
}

static int append_q(char *buf, size_t cap, const char *s) {
    size_t n = strlen(buf), m = strlen(s);
    if (n + m + 3 >= cap) return 0;
    buf[n] = '"';
    memcpy(buf + n + 1, s, m);
    buf[n + 1 + m] = '"';
    buf[n + 2 + m] = '\0';
    return 1;
}
static int append_s(char *buf, size_t cap, const char *s) {
    size_t n = strlen(buf), m = strlen(s);
    if (n + m + 1 >= cap) return 0;
    memcpy(buf + n, s, m + 1);
    return 1;
}

static int build_module(const BmToolchain *tk, const char *dump_path,
                        const char *stem, const char *out_path,
                        char *err, size_t err_cap) {
    char cmd[6144] = {0};
    char flavor[32];
    int rc;
    const char *compiler = tk->has_gcc ? "gcc" : "tcc";
    snprintf(flavor, sizeof(flavor), "%d", (int)PSX_OVERLAY_FLAVOR);
    if (!append_q(cmd, sizeof(cmd), tk->python) ||
        !append_s(cmd, sizeof(cmd), " ") || !append_q(cmd, sizeof(cmd), tk->builder) ||
        !append_s(cmd, sizeof(cmd), " --dump ") || !append_q(cmd, sizeof(cmd), dump_path) ||
        !append_s(cmd, sizeof(cmd), " --toolchain ") || !append_q(cmd, sizeof(cmd), tk->dir) ||
        !append_s(cmd, sizeof(cmd), " --stem ") || !append_s(cmd, sizeof(cmd), stem) ||
        !append_s(cmd, sizeof(cmd), " --out ") || !append_q(cmd, sizeof(cmd), out_path) ||
        !append_s(cmd, sizeof(cmd), " --flavor ") || !append_s(cmd, sizeof(cmd), flavor) ||
        /* This build's architecture, not the Python's or the compiler's (an
         * x86_64 runtime under Rosetta, arm64-only Xcode clang). */
        !append_s(cmd, sizeof(cmd), " --arch-abi ") ||
        !append_s(cmd, sizeof(cmd), overlay_loader_arch_abi()) ||
        !append_s(cmd, sizeof(cmd), " --compiler ") || !append_s(cmd, sizeof(cmd), compiler)) {
        set_err(err, err_cap, "build command too long");
        return 0;
    }
    if (tk->has_gcc && tk->gcc[0]) {
        append_s(cmd, sizeof(cmd), " --gcc ");
        append_q(cmd, sizeof(cmd), tk->gcc);
    }
    if (!tk->has_gcc) {
        append_s(cmd, sizeof(cmd), " --tcc ");
        append_q(cmd, sizeof(cmd), tk->tcc);
    }
    note("building %s backend from %s with %s (one-time; this can take a while)",
         stem, dump_path, compiler);
    if (s_progress_fn) s_progress_fn(s_progress_ctx, 0.02f, "Starting the BIOS build…");
    fprintf(stdout, "psx_bios_module: %s\n", cmd);
    fflush(stdout);
    rc = run_command(cmd);
    if (rc != 0) {
        char b[200];
        snprintf(b, sizeof(b), "bios_module_build.py exited %d (see log above)", rc);
        set_err(err, err_cap, b);
        return 0;
    }
    if (!file_exists(out_path)) {
        set_err(err, err_cap, "builder reported success but wrote no module");
        return 0;
    }
    return 1;
}

/* ---- Entry point ------------------------------------------------------------ */
static int read_identity(const char *path, uint32_t *crc, uint32_t *size);

int psx_bios_module_is_cached(const char *dump_path, const char *exe_dir) {
    uint32_t crc = 0, size = 0;
    const PsxKnownBiosImage *img;
    char path[1400];
    if (!read_identity(dump_path, &crc, &size)) return 0;
    img = known_for(crc, size);
    if (!img) return 0;
    if (!psx_bios_module_cache_path(exe_dir, img->stem, crc, path, sizeof(path))) return 0;
    return file_exists(path);
}

static int read_identity(const char *path, uint32_t *crc, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long n;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    n = ftell(f);
    if (n <= 0 || n > (8 << 20)) { fclose(f); return 0; }
    rewind(f);
    data = (unsigned char *)malloc((size_t)n);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return 0; }
    fclose(f);
    *crc = crc32_compute(data, (size_t)n);
    *size = (uint32_t)n;
    free(data);
    return 1;
}

const PsxBiosBackend *psx_bios_module_acquire(const char *dump_path, const char *exe_dir,
                                              int allow_build, char *err, size_t err_cap) {
    uint32_t crc = 0, size = 0;
    const PsxKnownBiosImage *img;
    char path[1400];
    char e[256] = {0};
    const PsxBiosBackend *b;
    BmToolchain tk;

    if (!psx_bios_bundled()) { set_err(err, err_cap, "no bundled backend (setup host)"); return 0; }
    if (!read_identity(dump_path, &crc, &size)) { set_err(err, err_cap, "cannot read the BIOS file"); return 0; }
    img = known_for(crc, size);
    if (!img) {
        set_err(err, err_cap, "no shipped profile matches this image (size/CRC)");
        return 0;
    }
    /* Already registered (a prior acquire in this process)? */
    {
        uint32_t i;
        for (i = 0; i < psx_bios_registry_count; i++) {
            const PsxBiosBackend *r = psx_bios_registry[i];
            if (r && r->image && r->image->image_crc32 == crc && r->image->image_size == size)
                return r;
        }
    }
    if (!psx_bios_module_cache_path(exe_dir, img->stem, crc, path, sizeof(path))) {
        set_err(err, err_cap, "cache path too long");
        return 0;
    }
    if (file_exists(path)) {
        b = psx_bios_module_load(path, e, sizeof(e));
        if (b) return b;
        note("cached module %s rejected: %s (rebuilding)", path, e);
        remove(path);
    }
    if (!allow_build) { set_err(err, err_cap, "not built yet"); return 0; }
    if (!probe_toolchain(exe_dir, &tk)) {
        set_err(err, err_cap, "no overlay toolchain beside the executable (or no compiler)");
        return 0;
    }
    mkdirs(path);
    if (!build_module(&tk, dump_path, img->stem, path, err, err_cap)) return 0;
    b = psx_bios_module_load(path, e, sizeof(e));
    if (!b) {
        char buf[320];
        snprintf(buf, sizeof(buf), "freshly built module rejected: %s", e);
        set_err(err, err_cap, buf);
        return 0;
    }
    return b;
}
