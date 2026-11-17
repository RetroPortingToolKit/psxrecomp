/* Portable generate → rebuild (--no-pgo from setup) → relaunch host. */

#include "psxrecomp_codegen_host.h"

#include "psx_bios_known_images.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <spawn.h>
#  include <strings.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
extern char** environ;
#endif

/* Forward decls — used by toolchain cache helpers before their definitions. */
static int rmtree_path(const char* path);
static int mkdir_p(const char* path);
static int cmake_path_runs(const char* cmake_path);
static int toolchain_bin_is_healthy(const char* bin);
static void heal_broken_toolchain_pointers(void);
static void clear_project_toolchain_stamp(void);
static int host_paths_same_file(const char* a, const char* b);
static int restore_dir_aside(const char* aside, const char* path);
static int pack_root_has_cmake_direct(const char* root);
static void prune_old_toolchain_tags(const char* keep_pack);
#if defined(_WIN32)
static int junction_dir(const char* link_path, const char* target_path);
static int run_cmdline_wait(const char* cmdline, DWORD* out_code);
#endif

static const PsxrecompCodegenHostConfig* g_cfg;
static char g_project_root[1024];
static char g_cli_path[1100];
static char g_game_toml[1100];
static char g_python[512];
static char g_cmake[512];
static char g_build_dir[1100];
static char g_exe_path[1100];
static char g_helper_path[1100];
static char g_cmake_target[256];
static char g_exe_basename[256];
static char g_display[128];
static char g_toolchain_bin[1400];
/* Last ensure-toolchain JSONL result path (bin/); shared-cache fallback. */
static char g_cli_toolchain_bin[1400];
static int g_ready;
static int g_relaunch_is_helper;
/* Wizard BIOS pick (survives cwd-relative bios.cfg misses on Windows). */
static char g_wizard_bios[1100];
/* Set when heal removes a broken latest/ / stamp at wizard open. */
static char g_tc_repair_note[320];
/* The system's error of the last rename aside that failed (GetLastError on
 * Windows, errno elsewhere). */
static long g_tc_aside_error;
/* The step of the toolchain check that failed last (toolchain_bin_is_healthy);
 * "" after a check that passed. The player reads it in the sentences below. */
static const char* g_tc_check_step = "";
#define TC_STEP_CMAKE   "cmake does not run"
#define TC_STEP_MISSING "clang or the linker is missing from the pack"
#define TC_STEP_CLANG   "clang does not run"
#define TC_STEP_TEMP    "a test file could not be written to the temporary folder"
#define TC_STEP_LINK    "a one-line test program did not compile and link"
/* Set when host_persist_setup receives an explicit bios_path (including ""
 * for OpenBIOS). Distinguishes intentional OpenBIOS clear from "unset". */
static int g_wizard_bios_explicit;

static const char* cfg_or(const char* v, const char* d) {
    return (v && v[0]) ? v : d;
}

static int ascii_strcasecmp(const char* a, const char* b) {
#if defined(_WIN32)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

static int path_is_file(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* Read the first line of a small text file into out, trimming EOL and any
 * trailing spaces. Returns 1 on success. Used for the exe-name marker CMake
 * writes; anything unreadable just leaves the caller on its fallback. */
static int read_first_line(const char* path, char* out, size_t cap) {
    FILE* f;
    size_t n;
    if (!path || !out || cap == 0)
        return 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        out[0] = '\0';
        return 0;
    }
    fclose(f);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return out[0] != '\0';
}

static int path_is_absolute(const char* path) {
    if (!path || !path[0]) return 0;
#if defined(_WIN32)
    if (path[0] == '/' || path[0] == '\\') return 1;
    if (path[0] && path[1] == ':') return 1;
    return 0;
#else
    return path[0] == '/';
#endif
}

static int join_path(char* out, size_t cap, const char* a, const char* b);
static int dirname_copy(char* out, size_t cap, const char* path);
static int absolutize_existing_file(char* out, size_t cap, const char* path);

static int path_is_dir(const char* path) {
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int join_path(char* out, size_t cap, const char* a, const char* b) {
    size_t na = strlen(a);
    int need_slash = na > 0 && a[na - 1] != '/' && a[na - 1] != '\\';
    int n = snprintf(out, cap, "%s%s%s", a, need_slash ? "/" : "", b);
    return n > 0 && (size_t)n < cap;
}

#if defined(_WIN32)
/* Microsoft Store Python redirects %LOCALAPPDATA% writes into
 * Packages\PythonSoftwareFoundation.Python.*\LocalCache\Local\...
 * Map a virtual LocalAppData path to those on-disk mirrors. */
static int store_python_localcache_mirror(const char* virtual_path, char* out,
                                          size_t cap) {
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !local[0] || !virtual_path || !virtual_path[0])
        return 0;
    size_t llen = strlen(local);
    if (_strnicmp(virtual_path, local, (int)llen) != 0)
        return 0;
    const char* suffix = virtual_path + llen;
    if (*suffix != '\\' && *suffix != '/')
        return 0;
    while (*suffix == '\\' || *suffix == '/')
        ++suffix;
    if (!suffix[0])
        return 0;

    char packages[1100];
    if (!join_path(packages, sizeof(packages), local, "Packages"))
        return 0;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern),
             "%s\\PythonSoftwareFoundation.Python.*", packages);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        char mirror[1400];
        int n = snprintf(mirror, sizeof(mirror),
                         "%s\\%s\\LocalCache\\Local\\%s", packages,
                         fd.cFileName, suffix);
        if (n <= 0 || (size_t)n >= sizeof(mirror))
            continue;
        DWORD attr = GetFileAttributesA(mirror);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s", mirror);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* Prefer the real on-disk directory (handles Store Python redirection). */
static int resolve_existing_dir(const char* path, char* out, size_t cap) {
    if (path && path[0] && path_is_dir(path)) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
    char alt[1400];
    if (path && store_python_localcache_mirror(path, alt, sizeof(alt)) &&
        path_is_dir(alt)) {
        snprintf(out, cap, "%s", alt);
        return 1;
    }
    return 0;
}

static int python_path_is_store(const char* path) {
    if (!path || !path[0])
        return 0;
    char lower[1100];
    size_t n = strlen(path);
    if (n >= sizeof(lower))
        n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = path[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    return strstr(lower, "windowsapps") != NULL ||
           strstr(lower, "pythonsoftwarefoundation") != NULL;
}

/* Run a short cmdline and capture the first stdout line (trimmed). */
static int capture_cmd_first_line(const char* cmdline, char* out, size_t cap) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[1024];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    size_t got = 0;
    DWORD n = 0;
    out[0] = '\0';
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n') {
                out[got] = '\0';
                got = cap; /* mark complete */
                break;
            }
            if (got + 1 < cap)
                out[got++] = c;
        }
        if (got >= cap)
            break;
    }
    if (got > 0 && got < cap)
        out[got] = '\0';
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 8000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out[0] != '\0';
}
#else
static int resolve_existing_dir(const char* path, char* out, size_t cap) {
    if (path && path[0] && path_is_dir(path)) {
        snprintf(out, cap, "%s", path);
        return 1;
    }
    return 0;
}

#endif

static int dirname_copy(char* out, size_t cap, const char* path) {
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\')
        --n;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))
        --n;
    if (n == 0) {
        if (cap < 2) return 0;
        out[0] = '.';
        out[1] = '\0';
        return 1;
    }
    if (n >= cap) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

/* Resolve an existing file to an absolute path. Tries the string as-is, then
 * under g_project_root, then next to g_exe_path. Relative bios.cfg lines like
 * "bios/SCPH1001.BIN" otherwise break after relaunch when cwd is build/. */
static int absolutize_existing_file(char* out, size_t cap, const char* path) {
    char cand[1100];
    if (!out || cap < 2 || !path || !path[0]) return 0;
    out[0] = '\0';

    for (int pass = 0; pass < 3; ++pass) {
        const char* candidate = NULL;
        if (pass == 0) {
            candidate = path;
        } else if (pass == 1) {
            if (path_is_absolute(path) || !g_project_root[0]) continue;
            if (!join_path(cand, sizeof(cand), g_project_root, path)) continue;
            candidate = cand;
        } else {
            char dir[1100];
            if (path_is_absolute(path) || !g_exe_path[0]) continue;
            if (!dirname_copy(dir, sizeof(dir), g_exe_path)) continue;
            if (!join_path(cand, sizeof(cand), dir, path)) continue;
            candidate = cand;
        }
        if (!candidate || !path_is_file(candidate)) continue;
#if defined(_WIN32)
        {
            DWORD n = GetFullPathNameA(candidate, (DWORD)cap, out, NULL);
            if (n > 0 && n < (DWORD)cap) return 1;
        }
#else
        /* glibc _FORTIFY_SOURCE aborts if the realpath destination is smaller
         * than PATH_MAX (4096), even when the resolved path fits. Allocate. */
        {
            char* rp = realpath(candidate, NULL);
            if (rp) {
                snprintf(out, cap, "%s", rp);
                free(rp);
                return 1;
            }
        }
#endif
        if ((size_t)snprintf(out, cap, "%s", candidate) < cap) return 1;
    }
    return 0;
}

static int resolve_cli_path(const char* root, char* out, size_t cap) {
    const char* candidates[] = {
        cfg_or(g_cfg->psxrecomp_cli_relpath, "psxrecomp/psxrecomp_cli.py"),
        "psxrecomp/psxrecomp_cli.py",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i] || !candidates[i][0])
            continue;
        if (!join_path(out, cap, root, candidates[i]))
            continue;
        if (path_is_file(out))
            return 1;
    }
    return 0;
}

static int looks_like_project_root(const char* root) {
    char cli[1100], toml[1100];
    if (!join_path(toml, sizeof(toml), root,
                   cfg_or(g_cfg->seed_cfg_relpath, "game.toml")))
        return 0;
    if (!path_is_file(toml))
        return 0;
    return resolve_cli_path(root, cli, sizeof(cli));
}

static int find_on_path(const char* name, char* out, size_t cap) {
#if defined(_WIN32)
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "where %s >nul 2>nul", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#else
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    if (system(cmd) == 0) {
        snprintf(out, cap, "%s", name);
        return 1;
    }
#endif
    return 0;
}

/* resolve_toolchain_bin is defined later; pack python lives beside bin/. */
static int resolve_toolchain_bin(char* out, size_t cap);

/* cmake-clang-v1 1.0.6+: python-build-standalone under <pack>/python/. */
static int python_exe_under_py_root(const char* py_root, char* out, size_t cap) {
    char cand[1200];
    if (!py_root || !py_root[0] || !path_is_dir(py_root))
        return 0;
#if defined(_WIN32)
    if (join_path(cand, sizeof(cand), py_root, "python.exe") &&
        path_is_file(cand) && !python_path_is_store(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
    if (join_path(cand, sizeof(cand), py_root, "python3.exe") &&
        path_is_file(cand) && !python_path_is_store(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#endif
    if (join_path(cand, sizeof(cand), py_root, "bin/python3") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
    if (join_path(cand, sizeof(cand), py_root, "bin/python") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#if defined(__APPLE__)
#if defined(__aarch64__)
    if (join_path(cand, sizeof(cand), py_root,
                  "aarch64-apple-darwin/bin/python3") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#else
    if (join_path(cand, sizeof(cand), py_root,
                  "x86_64-apple-darwin/bin/python3") &&
        path_is_file(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }
#endif
#endif
    return 0;
}

static int find_toolchain_python(char* out, size_t cap) {
    char bin[1400], pack[1400], py_root[1400];
    if (!resolve_toolchain_bin(bin, sizeof(bin)))
        return 0;
    if (!dirname_copy(pack, sizeof(pack), bin) || !pack[0])
        return 0;
    if (!join_path(py_root, sizeof(py_root), pack, "python"))
        return 0;
    return python_exe_under_py_root(py_root, out, cap);
}

static int python_env_usable(const char* env) {
    if (!env || !env[0] || !path_is_file(env))
        return 0;
#if defined(_WIN32)
    if (python_path_is_store(env))
        return 0;
#endif
    return 1;
}

/* Prefer portable pack CPython (Retro / cmake-clang-v1), then system. */
static int find_python(char* out, size_t cap) {
    const char* env = getenv("RETCOMM_PYTHON");
    if (python_env_usable(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
    env = getenv("PYTHON");
    if (python_env_usable(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
#if defined(_WIN32)
    if (find_toolchain_python(out, cap))
        return 1;
    /* Prefer python.org / py-launcher installs over the Microsoft Store
     * stub: Store Python redirects LocalAppData writes into LocalCache. */
    char resolved[1100];
    if (capture_cmd_first_line(
            "py -3 -c \"import sys; print(sys.executable)\"", resolved,
            sizeof(resolved)) &&
        path_is_file(resolved) && !python_path_is_store(resolved)) {
        snprintf(out, cap, "%s", resolved);
        return 1;
    }
    if (capture_cmd_first_line(
            "python -c \"import sys; print(sys.executable)\"", resolved,
            sizeof(resolved)) &&
        path_is_file(resolved) && !python_path_is_store(resolved)) {
        snprintf(out, cap, "%s", resolved);
        return 1;
    }
    const char* candidates[] = {"python.exe", "python3.exe", "py.exe"};
#else
    const char* candidates[] = {"python3", "python"};
#endif
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!find_on_path(candidates[i], out, cap))
            continue;
#if defined(_WIN32)
        if (python_path_is_store(out))
            continue;
#endif
        return 1;
    }
    return 0;
}

/* Pack bin folders that did not pass their check in this process. A pack that
 * fails its check is not removed and not renamed: one failed check does not
 * prove that it is broken, and a running build may be using it. It is passed
 * over for the rest of this process instead, so that the lookup can find
 * another pack. An install clears the list. */
static char g_tc_rejected[8][1400];
static const char* g_tc_rejected_step[8];   /* the step each one failed at */
static int g_tc_rejected_n = 0;

static int toolchain_bin_is_rejected(const char* bin) {
    int i;
    if (!bin || !bin[0])
        return 0;
    for (i = 0; i < g_tc_rejected_n; ++i) {
#if defined(_WIN32)
        if (_stricmp(g_tc_rejected[i], bin) == 0)
#else
        if (strcmp(g_tc_rejected[i], bin) == 0)
#endif
            return 1;
    }
    return 0;
}

static void toolchain_bin_reject(const char* bin) {
    if (!bin || !bin[0] || toolchain_bin_is_rejected(bin))
        return;
    if (g_tc_rejected_n < (int)(sizeof(g_tc_rejected) / sizeof(g_tc_rejected[0]))) {
        g_tc_rejected_step[g_tc_rejected_n] = g_tc_check_step;
        snprintf(g_tc_rejected[g_tc_rejected_n++], sizeof(g_tc_rejected[0]), "%s", bin);
    }
}

/* The step a passed-over pack failed at, "" for a pack that is not on the list. */
static const char* toolchain_bin_rejected_step(const char* bin) {
    int i;
    if (!bin || !bin[0])
        return "";
    for (i = 0; i < g_tc_rejected_n; ++i) {
#if defined(_WIN32)
        if (_stricmp(g_tc_rejected[i], bin) == 0)
#else
        if (strcmp(g_tc_rejected[i], bin) == 0)
#endif
            return g_tc_rejected_step[i] ? g_tc_rejected_step[i] : "";
    }
    return "";
}

/* What a player reads about the toolchain. Each sentence says only what the
 * program knows: which step of the check failed, or which error the system
 * gave, and what was left as it was. */
static void toolchain_text_check_failed(char* out, size_t cap, const char* step) {
    snprintf(out, cap,
             "The portable toolchain did not pass its check: %s. It was not "
             "removed. Download the latest pack to replace it.", step);
}

static void toolchain_text_new_pack_failed(char* out, size_t cap, const char* step) {
    snprintf(out, cap,
             "The new toolchain pack did not pass its check: %s. The installed "
             "toolchain was not changed.", step);
}

/* The download into the temporary folder, and the put back of the installed
 * pack after the new one failed its check. */
#define TOOLCHAIN_DOWNLOAD_NO_FOLDER "the temporary folder could not be created"
#define TOOLCHAIN_DOWNLOAD_NO_WRITE \
    "the pack could not be written into the temporary folder"

static void toolchain_text_download_failed(char* out, size_t cap, const char* reason) {
    snprintf(out, cap,
             "The toolchain pack was not downloaded: %s. The installed "
             "toolchain was not changed. Check the free space and the "
             "permissions of the temporary folder, then try again.", reason);
}

static void toolchain_text_put_back_failed(char* out, size_t cap, const char* step) {
    snprintf(out, cap,
             "The new toolchain pack did not pass its check: %s. The installed "
             "toolchain could not be put back under its name. It was left in "
             "the same place, with .old- at the start of its name. Download the "
             "latest pack to install the toolchain again.", step);
}

/* `error` is GetLastError() on Windows and errno elsewhere, from the rename. */
static void toolchain_text_rename_failed(char* out, size_t cap, long error) {
#if defined(_WIN32)
    if (error == 32) {
        snprintf(out, cap,
                 "Another program holds the installed toolchain folder open "
                 "(Windows error 32). The toolchain was not changed. Close the "
                 "programs that use it, for example a running build, and try again.");
        return;
    }
    if (error == 5) {
        snprintf(out, cap,
                 "Windows refused to rename the installed toolchain folder (error "
                 "5, access denied). The toolchain was not changed. A file in it "
                 "may be open in another program, or the folder may be protected.");
        return;
    }
#endif
    snprintf(out, cap,
             "The installed toolchain folder could not be renamed (system error "
             "%ld). The toolchain was not changed.", error);
}

static void toolchain_rejects_clear(void) {
    g_tc_rejected_n = 0;
}

static int toolchain_bin_has_cmake(const char* bin, char* out, size_t cap) {
    char cmake[1200];
    if (toolchain_bin_is_rejected(bin))
        return 0;
#if defined(_WIN32)
    if (join_path(cmake, sizeof(cmake), bin, "cmake.exe") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#else
    if (join_path(cmake, sizeof(cmake), bin, "cmake") && path_is_file(cmake)) {
        snprintf(out, cap, "%s", bin);
        return 1;
    }
#endif
    return 0;
}

static int resolve_toolchain_bin_under(const char* wrap, char* out, size_t cap) {
    char cand[1100], cmake[1200], root[1400];
    if (!wrap || !wrap[0])
        return 0;
    if (!resolve_existing_dir(wrap, root, sizeof(root)))
        return 0;
    wrap = root;
    if (join_path(cand, sizeof(cand), wrap, "bin") &&
        toolchain_bin_has_cmake(cand, out, cap))
        return 1;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%s\\*", wrap);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, fd.cFileName))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (!toolchain_bin_is_rejected(nbin) &&
            join_path(cmake, sizeof(cmake), nbin, "cmake.exe") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR* dir = opendir(wrap);
    if (!dir)
        return 0;
    int found = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char nested[1100], nbin[1100];
        if (!join_path(nested, sizeof(nested), wrap, ent->d_name))
            continue;
        if (!path_is_dir(nested))
            continue;
        if (!join_path(nbin, sizeof(nbin), nested, "bin"))
            continue;
        if (!toolchain_bin_is_rejected(nbin) &&
            join_path(cmake, sizeof(cmake), nbin, "cmake") && path_is_file(cmake)) {
            snprintf(out, cap, "%s", nbin);
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
#endif
}

/* Prefer tagged installs (CLI writes latest/ / offline/) then any child. */
static int resolve_toolchain_cache_base(const char* base, char* out, size_t cap) {
    static const char* prefer[] = {"latest", "offline", NULL};
    char cand[1100];
    if (!base || !base[0])
        return 0;
    for (int i = 0; prefer[i]; ++i) {
        if (join_path(cand, sizeof(cand), base, prefer[i]) &&
            resolve_toolchain_bin_under(cand, out, cap))
            return 1;
    }
    return resolve_toolchain_bin_under(base, out, cap);
}

/* Collect …/cmake-clang-v1 cache roots (same order as toolchain_pack.py).
 * Honors RETCOMM_TOOLCHAIN_CACHE / RETCOMM_DATA_HOME; searches both XDG and
 * ~/.local/share so a pack installed by install.sh is always found. */
static int collect_toolchain_cache_bases(char bases[][1400], int max_n) {
    int n = 0;
    const char* cache = getenv("RETCOMM_TOOLCHAIN_CACHE");
    if (cache && cache[0] && n < max_n) {
        snprintf(bases[n++], 1400, "%s", cache);
    }
    const char* data = getenv("RETCOMM_DATA_HOME");
    if (data && data[0] && n < max_n) {
        if (join_path(bases[n], 1400, data, "toolchains/cmake-clang-v1"))
            ++n;
    }
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (local && local[0]) {
        if (n < max_n &&
            join_path(bases[n], 1400, local, "retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < max_n &&
            join_path(bases[n], 1400, local,
                      "psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
        /* Store Python may have written only into LocalCache mirrors. */
        for (int i = 0, lim = n; i < lim && n < max_n; ++i) {
            char mirror[1400];
            if (store_python_localcache_mirror(bases[i], mirror,
                                              sizeof(mirror)))
                snprintf(bases[n++], 1400, "%s", mirror);
        }
    }
#endif
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0]) {
        if (n < max_n &&
            join_path(bases[n], 1400, xdg, "retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < max_n &&
            join_path(bases[n], 1400, xdg,
                      "psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
    }
    if (home && home[0]) {
        if (n < max_n &&
            join_path(bases[n], 1400, home,
                      ".local/share/retcomm/toolchains/cmake-clang-v1"))
            ++n;
        if (n < max_n &&
            join_path(bases[n], 1400, home,
                      ".local/share/psxrecomp/toolchains/cmake-clang-v1"))
            ++n;
    }
    /* Dedup in-place. */
    int w = 0;
    for (int i = 0; i < n; ++i) {
        int dup = 0;
        for (int j = 0; j < w; ++j) {
#if defined(_WIN32)
            if (_stricmp(bases[i], bases[j]) == 0) {
#else
            if (strcmp(bases[i], bases[j]) == 0) {
#endif
                dup = 1;
                break;
            }
        }
        if (!dup) {
            if (w != i)
                snprintf(bases[w], 1400, "%s", bases[i]);
            ++w;
        }
    }
    return w;
}

/* Preferred install root for new downloads (first retcomm cache base). */
static int preferred_toolchain_cache_root(char* out, size_t cap) {
    char bases[12][1400];
    int n = collect_toolchain_cache_bases(bases, 12);
    for (int i = 0; i < n; ++i) {
        if (strstr(bases[i], "retcomm") != NULL) {
            snprintf(out, cap, "%s", bases[i]);
            return 1;
        }
    }
    if (n < 1)
        return 0;
    snprintf(out, cap, "%s", bases[0]);
    return 1;
}

static int resolve_shared_toolchain_cache(char* out, size_t cap) {
    char bases[12][1400];
    int n = collect_toolchain_cache_bases(bases, 12);
    for (int i = 0; i < n; ++i) {
        if (resolve_toolchain_cache_base(bases[i], out, cap))
            return 1;
    }
    return 0;
}

/* CLI writes project_root/toolchain/.psxrecomp-bin with the bin path. */
static int resolve_toolchain_stamp(char* out, size_t cap) {
    char stamp[1200], line[1400], resolved[1400];
    FILE* f;
    if (!g_project_root[0])
        return 0;
    if (!join_path(stamp, sizeof(stamp), g_project_root,
                   "toolchain/.psxrecomp-bin"))
        return 0;
#if defined(_WIN32)
    {
        char alt[1400];
        if (!path_is_file(stamp) &&
            store_python_localcache_mirror(stamp, alt, sizeof(alt)) &&
            path_is_file(alt))
            snprintf(stamp, sizeof(stamp), "%s", alt);
    }
#endif
    if (!path_is_file(stamp))
        return 0;
    f = fopen(stamp, "r");
    if (!f)
        return 0;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                 line[n - 1] == ' '))
        line[--n] = '\0';
    if (!line[0])
        return 0;
    if (resolve_existing_dir(line, resolved, sizeof(resolved)) &&
        toolchain_bin_has_cmake(resolved, out, cap))
        return 1;
    return 0;
}

static int resolve_toolchain_bin(char* out, size_t cap) {
    /* Fresh ensure-toolchain result (bin directory). */
    if (g_cli_toolchain_bin[0]) {
        char resolved[1400];
        if (resolve_existing_dir(g_cli_toolchain_bin, resolved,
                                 sizeof(resolved)) &&
            toolchain_bin_has_cmake(resolved, out, cap))
            return 1;
    }

    const char* env_keys[] = {
        "RETCOMM_TOOLCHAIN_DIR", "PSXRECOMP_TOOLCHAIN_DIR", "TOOLCHAIN_DIR",
        "BPE_TOOLCHAIN_DIR", NULL};
    for (int i = 0; env_keys[i]; ++i) {
        const char* e = getenv(env_keys[i]);
        if (e && e[0] && resolve_toolchain_bin_under(e, out, cap))
            return 1;
    }
    if (resolve_toolchain_stamp(out, cap))
        return 1;
    if (g_project_root[0]) {
        char wrap[1100];
        if (join_path(wrap, sizeof(wrap), g_project_root, "toolchain") &&
            resolve_toolchain_bin_under(wrap, out, cap))
            return 1;
    }
    /* CLI downloads land here — must match toolchain_pack.py. */
    if (resolve_shared_toolchain_cache(out, cap))
        return 1;
    return 0;
}

/* Two PATH entries name the same folder: case and the two slashes do not
 * matter on Windows, a trailing slash does not matter anywhere. */
static int path_entry_is(const char* entry, size_t len, const char* folder) {
    size_t flen = strlen(folder), i;
    while (len > 1 && (entry[len - 1] == '/' || entry[len - 1] == '\\'))
        --len;
    while (flen > 1 && (folder[flen - 1] == '/' || folder[flen - 1] == '\\'))
        --flen;
    if (len != flen)
        return 0;
    for (i = 0; i < len; ++i) {
        char a = entry[i], b = folder[i];
#if defined(_WIN32)
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
        if (a != b)
            return 0;
    }
    return 1;
}

/* Put `first` and `second` (either may be empty) once at the head of PATH and
 * take their other copies out. Calling it again changes nothing.
 *
 * Activation used to prepend both at every call into a buffer of 8,192
 * characters. A host that relaunched itself grew PATH by a few folders a
 * generation, and a PATH longer than the buffer was cut off in the middle of
 * an entry. A PATH grown that way was then too long for the readiness check's
 * command line, and a whole pack was judged unusable. */
static void path_put_in_front(const char* first, const char* second) {
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const char* old = getenv("PATH");
    const char* front[2];
    size_t cap, pos = 0;
    char* neu;
    int k;
    front[0] = (first && first[0]) ? first : NULL;
    front[1] = (second && second[0] && !(front[0] && path_entry_is(second, strlen(second), front[0])))
                   ? second : NULL;
    if (!front[0] && !front[1])
        return;
    cap = (old ? strlen(old) : 0) + (front[0] ? strlen(front[0]) : 0) +
          (front[1] ? strlen(front[1]) : 0) + 4;
    neu = (char*)malloc(cap);
    if (!neu)
        return;
    for (k = 0; k < 2; ++k) {
        if (!front[k])
            continue;
        if (pos)
            neu[pos++] = sep;
        memcpy(neu + pos, front[k], strlen(front[k]));
        pos += strlen(front[k]);
    }
    if (old) {
        const char* p = old;
        while (*p) {
            const char* end = strchr(p, sep);
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (!(front[0] && path_entry_is(p, len, front[0])) &&
                !(front[1] && path_entry_is(p, len, front[1]))) {
                neu[pos++] = sep;
                memcpy(neu + pos, p, len);
                pos += len;
            }
            if (!end)
                break;
            p = end + 1;
        }
    }
    neu[pos] = '\0';
    if (!old || strcmp(old, neu) != 0) {
#if defined(_WIN32)
        if (_putenv_s("PATH", neu) != 0)
#else
        if (setenv("PATH", neu, 1) != 0)
#endif
            fprintf(stderr,
                    "psxrecomp-codegen: PATH could not be set with the toolchain "
                    "folders in front (%u characters); it stays as it is\n",
                    (unsigned)pos);
    }
    free(neu);
}

static void activate_toolchain_path(void) {
    char pack_root[1400];
    g_toolchain_bin[0] = '\0';
#if !defined(_WIN32)
    /* A cached portable pack must not shadow Linux/macOS native tools. */
    return;
#endif
    if (!resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return;
    /* Pack root (parent of bin/) — Windows cmake-clang-v1 ships zlib here. */
    pack_root[0] = '\0';
    (void)dirname_copy(pack_root, sizeof(pack_root), g_toolchain_bin);

    /* Prepend pack python/ (Windows) or python/bin (Unix), then bin/. */
    char py_path_dir[1400];
    char py_exe[1400];
    py_path_dir[0] = '\0';
    py_exe[0] = '\0';
    if (pack_root[0]) {
        char py_root[1400];
        if (join_path(py_root, sizeof(py_root), pack_root, "python") &&
            python_exe_under_py_root(py_root, py_exe, sizeof(py_exe))) {
#if defined(_WIN32)
            /* PBS Windows: <pack>/python/python.exe */
            snprintf(py_path_dir, sizeof(py_path_dir), "%s", py_root);
#else
            /* PBS Unix: <pack>/python/bin/python3 */
            if (!dirname_copy(py_path_dir, sizeof(py_path_dir), py_exe))
                py_path_dir[0] = '\0';
#endif
        }
    }

    path_put_in_front(py_path_dir, g_toolchain_bin);
#if defined(_WIN32)
    if (py_exe[0])
        _putenv_s("RETCOMM_PYTHON", py_exe);
#else
    if (py_exe[0])
        setenv("RETCOMM_PYTHON", py_exe, 1);
#endif
    if (pack_root[0]) {
#if defined(_WIN32)
        _putenv_s("RETCOMM_TOOLCHAIN_DIR", pack_root);
        _putenv_s("ZLIB_ROOT", pack_root);
        {
            const char* prev = getenv("CMAKE_PREFIX_PATH");
            char pref[8192];
            if (prev && prev[0] && !strstr(prev, pack_root))
                snprintf(pref, sizeof(pref), "%s;%s", pack_root, prev);
            else if (!prev || !prev[0])
                snprintf(pref, sizeof(pref), "%s", pack_root);
            else
                pref[0] = '\0';
            if (pref[0])
                _putenv_s("CMAKE_PREFIX_PATH", pref);
        }
#else
        setenv("RETCOMM_TOOLCHAIN_DIR", pack_root, 1);
        setenv("ZLIB_ROOT", pack_root, 1);
        {
            const char* prev = getenv("CMAKE_PREFIX_PATH");
            char pref[8192];
            if (prev && prev[0] && !strstr(prev, pack_root))
                snprintf(pref, sizeof(pref), "%s:%s", pack_root, prev);
            else if (!prev || !prev[0])
                snprintf(pref, sizeof(pref), "%s", pack_root);
            else
                pref[0] = '\0';
            if (pref[0])
                setenv("CMAKE_PREFIX_PATH", pref, 1);
        }
        /* Linux packs 1.0.4+: bundled libxml2.so.2 for lld / thin LTO. */
        {
            char libdir[1500];
            if (join_path(libdir, sizeof(libdir), pack_root, "lib") &&
                path_is_dir(libdir)) {
                const char* prev = getenv("LD_LIBRARY_PATH");
                char llp[8192];
                if (prev && prev[0] && !strstr(prev, libdir))
                    snprintf(llp, sizeof(llp), "%s:%s", libdir, prev);
                else if (!prev || !prev[0])
                    snprintf(llp, sizeof(llp), "%s", libdir);
                else
                    llp[0] = '\0';
                if (llp[0])
                    setenv("LD_LIBRARY_PATH", llp, 1);
            }
        }
#endif
    }
}

static int find_cmake(char* out, size_t cap) {
    const char* env = getenv("CMAKE");
    if (env && env[0] && path_is_file(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
#if !defined(_WIN32)
    return find_on_path("cmake", out, cap);
#endif
    char tc[1100], cand[1200];
    if (resolve_toolchain_bin(tc, sizeof(tc))) {
#if defined(_WIN32)
        if (join_path(cand, sizeof(cand), tc, "cmake.exe") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#else
        if (join_path(cand, sizeof(cand), tc, "cmake") && path_is_file(cand)) {
            snprintf(out, cap, "%s", cand);
            return 1;
        }
#endif
    }
#if defined(_WIN32)
    return find_on_path("cmake.exe", out, cap);
#else
    return find_on_path("cmake", out, cap);
#endif
}

/* Walk start and up to 10 parents for game.toml + psxrecomp_cli.py. */
static int walk_up_for_project_root(const char* start, char* out, size_t cap) {
    char cur[1024];
    if (!start || !start[0])
        return 0;
    snprintf(cur, sizeof(cur), "%s", start);
    for (int i = 0; i < 10; ++i) {
        if (looks_like_project_root(cur)) {
            snprintf(out, cap, "%s", cur);
            return 1;
        }
        char parent[1024];
        if (!dirname_copy(parent, sizeof(parent), cur))
            break;
        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

#if !defined(_WIN32)
/* Canonical path of the running executable on POSIX hosts. /proc/self/exe is
 * Linux-only; macOS has no procfs, so there the path comes from dyld. */
static int host_posix_exe_path(char* out, size_t cap) {
    char* rp = NULL;
#  if defined(__APPLE__)
    char raw[1100];
    uint32_t n = (uint32_t)sizeof(raw);
    if (_NSGetExecutablePath(raw, &n) != 0)
        return 0;
    rp = realpath(raw, NULL);
#  else
    rp = realpath("/proc/self/exe", NULL);
#  endif
    if (!rp)
        return 0;
    snprintf(out, cap, "%s", rp);
    free(rp);
    return out[0] != '\0';
}
#endif

/* Directory containing this process's executable (not cwd). Dolphin / .desktop
 * and Finder launches leave cwd as $HOME — VIDEO/PGO/setup must still find the
 * zip tree. Linux: $APPIMAGE parent, else /proc/self/exe. macOS: dyld's
 * executable path. Windows: GetModuleFileName. */
static int resolve_host_exe_dir(char* out, size_t cap) {
    char exe[1100];
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
    exe[0] = '\0';

#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
        if (n == 0 || n >= (DWORD)sizeof(exe))
            return 0;
    }
#else
    {
        const char* appimg = getenv("APPIMAGE");
        if (appimg && appimg[0] && path_is_file(appimg)) {
            snprintf(exe, sizeof(exe), "%s", appimg);
        } else if (!host_posix_exe_path(exe, sizeof(exe))) {
            return 0;
        }
    }
#endif
    if (!exe[0])
        return 0;
    return dirname_copy(out, cap, exe);
}

/* Last path segment of `path` (points into `path`; not nul-trimmed copy). */
static const char* path_base_name(const char* path) {
    const char* base = path ? path : "";
    for (const char* p = base; *p; ++p) {
        if ((*p == '/' || *p == '\\') && p[1])
            base = p + 1;
    }
    return base;
}

static int name_is_releases(const char* name) {
    if (!name || !name[0]) return 0;
#if defined(_WIN32)
    return _stricmp(name, "releases") == 0;
#else
    return strcmp(name, "releases") == 0;
#endif
}

/* Retro stages Play under apps/<title>/releases/<tag>/ while the generate
 * tree lives at apps/<title>/src/current/. Walking parents of the release dir
 * never visits that sibling — probe it explicitly. */
static int try_retcomm_src_current(const char* start, char* out, size_t cap) {
    char cur[1024];
    if (!start || !start[0] || !out || cap < 2)
        return 0;
    snprintf(cur, sizeof(cur), "%s", start);
    for (int i = 0; i < 8; ++i) {
        char parent[1024];
        if (!dirname_copy(parent, sizeof(parent), cur))
            break;

        char cand[1100];
        /* …/releases  →  …/src/current */
        if (name_is_releases(path_base_name(cur))) {
            if (join_path(cand, sizeof(cand), parent, "src/current") &&
                looks_like_project_root(cand)) {
                snprintf(out, cap, "%s", cand);
                return 1;
            }
        }
        /* …/releases/<tag>  →  …/src/current */
        if (name_is_releases(path_base_name(parent))) {
            char install[1024];
            if (dirname_copy(install, sizeof(install), parent) &&
                join_path(cand, sizeof(cand), install, "src/current") &&
                looks_like_project_root(cand)) {
                snprintf(out, cap, "%s", cand);
                return 1;
            }
        }

        if (strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    return 0;
}

static int discover_project_root(char* out, size_t cap) {
    const char* env_name =
        cfg_or(g_cfg->project_root_env, "PSXRECOMP_PROJECT_ROOT");
    const char* env = getenv(env_name);
    if (env && env[0] && looks_like_project_root(env)) {
        snprintf(out, cap, "%s", env);
        return 1;
    }

    /* 1) cwd (terminal launches from the unzip / repo root). */
    char start[1024];
#if defined(_WIN32)
    if (!GetCurrentDirectoryA((DWORD)sizeof(start), start))
        start[0] = '\0';
#else
    if (!getcwd(start, sizeof(start)))
        start[0] = '\0';
#endif
    if (start[0] && walk_up_for_project_root(start, out, cap))
        return 1;
    if (start[0] && try_retcomm_src_current(start, out, cap))
        return 1;

    /* 2) exe dir (GUI double-click: cwd is often $HOME / Desktop). */
    char exe_dir[1024];
    if (resolve_host_exe_dir(exe_dir, sizeof(exe_dir)) &&
        walk_up_for_project_root(exe_dir, out, cap))
        return 1;
    if (resolve_host_exe_dir(exe_dir, sizeof(exe_dir)) &&
        try_retcomm_src_current(exe_dir, out, cap))
        return 1;

    return 0;
}

static int resolve_build_paths(void) {
    const char* env_name =
        cfg_or(g_cfg->build_dir_env, "PSXRECOMP_BUILD_DIR");
    const char* env = getenv(env_name);
    if (env && env[0]) {
        snprintf(g_build_dir, sizeof(g_build_dir), "%s", env);
    } else {
        const char* names[] = {
            cfg_or(g_cfg->build_dir_name, "build"),
            "build-release",
            "build",
            "build-ci",
        };
        int found = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (!names[i] || !names[i][0])
                continue;
            char cand[1100];
            if (!join_path(cand, sizeof(cand), g_project_root, names[i]))
                continue;
            if (path_is_dir(cand)) {
                snprintf(g_build_dir, sizeof(g_build_dir), "%s", cand);
                found = 1;
                break;
            }
        }
        /* Setup zips ship without a pre-made build tree. Prefer the configured
         * name so rebuild can cmake -B it on first Generate & rebuild. */
        if (!found) {
            const char* prefer = cfg_or(g_cfg->build_dir_name, "build-release");
            if (!join_path(g_build_dir, sizeof(g_build_dir), g_project_root,
                           prefer))
                return 0;
        }
    }

    /* Prefer the name CMake published over the one baked into codegen_setup.c.
     * Both are MAKE_C_IDENTIFIER(WINDOW_TITLE), but from two separate copies of
     * the title, so renaming a game leaves this one pointing at an executable
     * that is never produced. runtime.cmake writes the name it really used to
     * psxrecomp_exe_name-<target>.txt beside the build. */
    char basename[256];
    snprintf(basename, sizeof(basename), "%s", g_exe_basename);
    if (g_cfg && g_cfg->cmake_target && g_cfg->cmake_target[0]) {
        char marker[1200], published[256];
        snprintf(published, sizeof(published), "psxrecomp_exe_name-%s.txt",
                 g_cfg->cmake_target);
        if (join_path(marker, sizeof(marker), g_build_dir, published) &&
            read_first_line(marker, published, sizeof(published)) &&
            published[0])
            snprintf(basename, sizeof(basename), "%s", published);
    }

    char exe_name[300];
#if defined(_WIN32)
    snprintf(exe_name, sizeof(exe_name), "%s.exe", basename);
#else
    snprintf(exe_name, sizeof(exe_name), "%s", basename);
#endif
    return join_path(g_exe_path, sizeof(g_exe_path), g_build_dir, exe_name);
}

#ifndef PSX_SETUP_BIOS_STEMS
#define PSX_SETUP_BIOS_STEMS "OpenBIOS|SCPH1001"
#endif
#ifndef PSX_SETUP_FRAMEWORK_REL
#define PSX_SETUP_FRAMEWORK_REL "psxrecomp"
#endif

/* Match runtime.cmake: a requested pair with its backend descriptor. A stale
 * pre-descriptor pair or unrelated game dispatch cannot complete BIOS setup. */
static int generated_bios_backend_linkable(const char* dir, const char* stem) {
    char full[600], path[1200], descriptor[600], line[4096];
    FILE* f;
    int found = 0;
    if ((size_t)snprintf(full, sizeof(full), "%s_full.c", stem) >= sizeof(full))
        return 0;
    if (!join_path(path, sizeof(path), dir, full) || !path_is_file(path))
        return 0;
    snprintf(full, sizeof(full), "%s_dispatch.c", stem);
    if (!join_path(path, sizeof(path), dir, full))
        return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    snprintf(descriptor, sizeof(descriptor), "%s_psx_bios_backend", stem);
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, descriptor)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* Does the configured framework hold a linkable requested BIOS backend?
 *
 * This used to probe two hardcoded names, OpenBIOS_dispatch.c and
 * SCPH1001_dispatch.c. A port that pins a different image — via
 * PSXRECOMP_BIOS_STEMS / game.toml recompiler.bios_config, as every wave-3
 * kit does with SCPH5552 — emits its backend under that other stem, so the
 * probe was permanently unsatisfied: Generate kept succeeding, the wizard
 * kept reopening, and first-run setup could never complete. CMake supplies
 * the requested stems; accept any linkable member, not an unrelated pair. */
static int generated_has_bios_backend(const char* dir) {
    const char* next = PSX_SETUP_BIOS_STEMS;
    while (*next) {
        char stem[512];
        const char* end = strchr(next, '|');
        size_t len = end ? (size_t)(end - next) : strlen(next);
        if (len && len < sizeof(stem)) {
            memcpy(stem, next, len);
            stem[len] = 0;
            if (generated_bios_backend_linkable(dir, stem)) return 1;
        }
        if (!end) break;
        next = end + 1;
    }
    return 0;
}

static int bios_backends_missing(void) {
    char framework[1100], gen[1200];
    if (!join_path(framework, sizeof(framework), g_project_root, PSX_SETUP_FRAMEWORK_REL) ||
        !join_path(gen, sizeof(gen), framework, "generated"))
        return 1;
    return !generated_has_bios_backend(gen);
}

int psxrecomp_codegen_host_sources_missing(
    const PsxrecompCodegenHostConfig* cfg) {
    if (!cfg || !cfg->cmake_target || !cfg->exe_basename)
        return 0;
    g_cfg = cfg;
    if (!g_project_root[0] &&
        !discover_project_root(g_project_root, sizeof(g_project_root)))
        return 0;
    char marker[1100];
    if (!join_path(marker, sizeof(marker), g_project_root,
                   cfg_or(cfg->gen_marker_relpath,
                          "generated/SLUS_011.89_dispatch.c")))
        return 1;
    if (!path_is_file(marker))
        return 1;
    return bios_backends_missing();
}

static int read_line_file(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                 out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    return n > 0;
}

static int write_line_file(const char* path, const char* line) {
    FILE* f;
    if (!path || !path[0])
        return 0;
    if (!line || !line[0]) {
        remove(path);
        return 1;
    }
    f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", line);
    fclose(f);
    return 1;
}

/* Sidecars are loaded next to argv[0]. Setup host is often zip-root while the
 * rebuilt binary lives under build/ — write beside the game binary too. */
static void write_sidecar_near_exe(const char* near_exe, const char* name,
                                   const char* value) {
    char dir[1100], path[1200];
    if (!near_exe || !near_exe[0] || !name || !name[0])
        return;
    if (!dirname_copy(dir, sizeof(dir), near_exe))
        return;
    if (!join_path(path, sizeof(path), dir, name))
        return;
    write_line_file(path, value ? value : "");
}

/* IEEE CRC-32 (zlib / Ethernet) — retail BIOS identity for setup discovery. */
static uint32_t host_crc32(const unsigned char* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i, j;
    for (i = 0; i < len; ++i) {
        crc ^= data[i];
        for (j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

/* Does this file match the retail image THIS build pins? A setup host has no
 * linked backend to ask, so it consults psx_bios_known_images.h rather than
 * assuming SCPH-1001 — which made every non-SCPH1001 kit reject a perfectly
 * good dump. An unknown pinned stem adopts nothing and the player is asked. */
static int retail_bios_file_ok_c(const char* path) {
    const PsxKnownBiosImage* want = psx_expected_bios();
    FILE* f;
    long size;
    unsigned char* buf;
    uint32_t crc;
    if (!want) return 0;
    if (!path || !path[0] || !path_is_file(path)) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size != (long)want->size) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = (unsigned char*)malloc((size_t)size);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    crc = host_crc32(buf, (size_t)size);
    free(buf);
    return crc == want->crc32;
}

/* Prefer a player-supplied dump of the pinned retail image next to the
 * project/exe for Generate. Missing → leave empty (OpenBIOS). Does not
 * override an explicit OpenBIOS. */
static int discover_retail_bios_c(char* out, size_t cap) {
    char names[8][32];
    int nnames = psx_known_bios_filenames(psx_expected_bios(), names, 8);
    static const char* subs[] = {
        "bios", "", "system", "firmware", "psxrecomp/bios", "psxrecomp-v4/bios",
    };
    char roots[3][1100];
    int nroots = 0;
    int r, s, n;
    if (nnames <= 0) { out[0] = 0; return 0; }
    if (g_project_root[0]) {
        snprintf(roots[nroots], sizeof(roots[0]), "%s", g_project_root);
        ++nroots;
    }
    if (g_exe_path[0]) {
        char dir[1100];
        if (dirname_copy(dir, sizeof(dir), g_exe_path)) {
            snprintf(roots[nroots], sizeof(roots[0]), "%s", dir);
            ++nroots;
        }
    }
    for (r = 0; r < nroots; ++r) {
        char walk[1100];
        int depth;
        snprintf(walk, sizeof(walk), "%s", roots[r]);
        for (depth = 0; depth < 6 && walk[0]; ++depth) {
            for (s = 0; s < (int)(sizeof(subs) / sizeof(subs[0])); ++s) {
                char dir[1200];
                if (subs[s][0]) {
                    if (!join_path(dir, sizeof(dir), walk, subs[s])) continue;
                } else {
                    snprintf(dir, sizeof(dir), "%s", walk);
                }
                for (n = 0; n < nnames; ++n) {
                    char cand[1300];
                    if (!join_path(cand, sizeof(cand), dir, names[n])) continue;
                    if (!retail_bios_file_ok_c(cand)) continue;
                    if (!absolutize_existing_file(out, cap, cand))
                        snprintf(out, cap, "%s", cand);
                    return 1;
                }
            }
            {
                char parent[1100];
                if (!dirname_copy(parent, sizeof(parent), walk)) break;
                if (strcmp(parent, walk) == 0) break;
                snprintf(walk, sizeof(walk), "%s", parent);
            }
        }
    }
    out[0] = '\0';
    return 0;
}

static int bios_cfg_sidecar_exists(void) {
    char cand[1100];
    char dir[1100];
    if (g_project_root[0] &&
        join_path(cand, sizeof(cand), g_project_root, "bios.cfg") &&
        path_is_file(cand))
        return 1;
    if (g_exe_path[0] && dirname_copy(dir, sizeof(dir), g_exe_path) &&
        join_path(cand, sizeof(cand), dir, "bios.cfg") && path_is_file(cand))
        return 1;
    if (path_is_file("bios.cfg")) return 1;
    return 0;
}

static int resolve_bios_arg(char* out, size_t cap) {
    char cand[1100];
    char line[1100];
    char abs[1100];
    if (g_wizard_bios[0] && absolutize_existing_file(abs, sizeof(abs),
                                                     g_wizard_bios)) {
        snprintf(out, cap, "%s", abs);
        snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", abs);
        return 1;
    }
    if (join_path(cand, sizeof(cand), g_project_root, "bios.cfg") &&
        read_line_file(cand, line, sizeof(line)) &&
        absolutize_existing_file(out, cap, line))
        return 1;
    if (g_exe_path[0]) {
        char dir[1100];
        if (dirname_copy(dir, sizeof(dir), g_exe_path) &&
            join_path(cand, sizeof(cand), dir, "bios.cfg") &&
            read_line_file(cand, line, sizeof(line)) &&
            absolutize_existing_file(out, cap, line))
            return 1;
    }
    if (read_line_file("bios.cfg", line, sizeof(line)) &&
        absolutize_existing_file(out, cap, line))
        return 1;
    /* bios.cfg present but empty = intentional OpenBIOS — do not rediscover. */
    if (g_wizard_bios_explicit || bios_cfg_sidecar_exists()) {
        out[0] = '\0';
        return 0;
    }
    /* No remembered pick — adopt SCPH1001 if present; else OpenBIOS (empty). */
    if (discover_retail_bios_c(out, cap)) {
        snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", out);
        if (g_project_root[0] &&
            join_path(cand, sizeof(cand), g_project_root, "bios.cfg"))
            write_line_file(cand, out);
        write_line_file("bios.cfg", out);
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "bios.cfg", out);
        fprintf(stderr, "psxrecomp-codegen: setup adopted retail BIOS %s\n",
                out);
        return 1;
    }
    out[0] = '\0';
    return 0;
}

/* bios_path NULL = leave bios.cfg untouched; "" = clear (OpenBIOS); else write. */
/* ---- multi-disc roster (game.toml [game] discs) --------------------------
 *
 * The RUNTIME launcher has published a disc roster for a while, built from
 * game.toml [game] discs. The SETUP host never did -- so on a first run the
 * wizard could not know a title was a 3-disc set and asked for a single image.
 * The other discs were then discovered missing much later, mid-game, at the
 * first swap.
 *
 * Mirrors the runtime's rule: a roster is published only when there is more
 * than one image, so single-disc titles see no behavioural change at all.
 * Matches LNG_MAX_DISCS in recomp-ui; a longer list is clamped, exactly as the
 * launcher clamps it. */
#define PSX_HOST_MAX_DISCS 8

static char g_disc_paths[PSX_HOST_MAX_DISCS][1024];
/* What the PLAYER located in the wizard, as opposed to what game.toml already
 * claimed. Generate writes these into game.toml so the runtime's hot-swap
 * roster ([game] discs) describes the images this machine actually has. */
static char g_wizard_discs[PSX_HOST_MAX_DISCS][1024];
static int  g_wizard_disc_count;
static RecompLauncherCDisc g_disc_roster[PSX_HOST_MAX_DISCS];
static int  g_num_discs;

static int host_path_is_absolute(const char* p) {
    if (!p || !p[0]) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    return (p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') ||
                            (p[0] >= 'a' && p[0] <= 'z')));
}

/* Minimal reader for one known key rather than a TOML library: this host shells
 * out to Python for every real parse, and pulling in a parser to read a single
 * array of strings would be a dependency for one line of config.
 *
 * Only [game] discs is recognised, and only string entries -- anything else in
 * the file is skipped rather than guessed at. */
static void load_game_toml_disc_roster(void) {
    FILE* f;
    char line[1600];
    int in_game = 0, in_array = 0, i;

    g_num_discs = 0;
    if (!g_game_toml[0]) return;
    f = fopen(g_game_toml, "rb");
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!in_array) {
            if (*p == '[') {                    /* section header */
                in_game = (strncmp(p, "[game]", 6) == 0);
                continue;
            }
            if (*p == '#' || !*p) continue;
            if (!in_game) continue;
            if (strncmp(p, "discs", 5) != 0) continue;
            p += 5;
            while (*p == ' ' || *p == '\t') ++p;
            /* Guard against a longer key that merely starts with "discs". */
            if (*p != '=') continue;
            ++p;
            in_array = 1;
        }
        for (; *p; ++p) {
            if (*p == ']') { in_array = 0; break; }
            if (*p == '#') break;               /* trailing comment */
            if (*p != '"') continue;
            {
                const char* q = ++p;
                size_t n;
                while (*p && *p != '"') ++p;
                if (!*p) break;                 /* unterminated -- give up */
                n = (size_t)(p - q);
                if (g_num_discs < PSX_HOST_MAX_DISCS &&
                    n < sizeof(g_disc_paths[0])) {
                    memcpy(g_disc_paths[g_num_discs], q, n);
                    g_disc_paths[g_num_discs][n] = '\0';
                    ++g_num_discs;
                }
            }
        }
        if (!in_array) break;
    }
    fclose(f);

    /* game.toml may hold repo-relative paths. The launcher existence-checks
     * what we publish, and its cwd is not necessarily the project root, so a
     * relative entry would render as "missing" on a perfectly good install. */
    for (i = 0; i < g_num_discs; ++i) {
        char abs[1024];
        if (host_path_is_absolute(g_disc_paths[i])) continue;
        if (!g_project_root[0]) continue;
        if (join_path(abs, sizeof(abs), g_project_root, g_disc_paths[i]))
            snprintf(g_disc_paths[i], sizeof(g_disc_paths[i]), "%s", abs);
    }

    /* One image is not a set: leave GameInfo dark so nothing changes for
     * single-disc titles. */
    if (g_num_discs < 2) { g_num_discs = 0; return; }
    for (i = 0; i < g_num_discs; ++i) {
        g_disc_roster[i].number = i + 1;
        g_disc_roster[i].label = NULL;   /* launcher formats "Disc N" */
        g_disc_roster[i].path = g_disc_paths[i];
    }
}

static int host_persist_setup(void* ctx, const char* rom_path,
                              const char* bios_path) {
    char path[1200];
    char abs_bios[1100];
    (void)ctx;
    if (bios_path) {
        g_wizard_bios_explicit = 1;
        if (bios_path[0] &&
            absolutize_existing_file(abs_bios, sizeof(abs_bios), bios_path)) {
            snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", abs_bios);
        } else {
            g_wizard_bios[0] = '\0';
        }
        if (g_project_root[0] &&
            join_path(path, sizeof(path), g_project_root, "bios.cfg"))
            write_line_file(path, g_wizard_bios[0] ? g_wizard_bios : "");
        write_line_file("bios.cfg", g_wizard_bios[0] ? g_wizard_bios : "");
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "bios.cfg",
                                   g_wizard_bios[0] ? g_wizard_bios : "");
        if (g_build_dir[0]) {
            char build_exe[1200];
            if (join_path(build_exe, sizeof(build_exe), g_build_dir,
                          g_exe_basename))
                write_sidecar_near_exe(build_exe, "bios.cfg",
                                       g_wizard_bios[0] ? g_wizard_bios : "");
        }
    }
    if (rom_path && rom_path[0]) {
        if (g_project_root[0] &&
            join_path(path, sizeof(path), g_project_root, "disc.cfg"))
            write_line_file(path, rom_path);
        write_line_file("disc.cfg", rom_path);
        if (g_exe_path[0])
            write_sidecar_near_exe(g_exe_path, "disc.cfg", rom_path);
        if (g_build_dir[0]) {
            char build_exe[1200];
            if (join_path(build_exe, sizeof(build_exe), g_build_dir,
                          g_exe_basename))
                write_sidecar_near_exe(build_exe, "disc.cfg", rom_path);
        }
    }
    return 0;
}

/* Multi-disc flush (RecompLauncherCGameInfo.persist_setup_discs).
 *
 * disc.cfg becomes one path per line, in disc order. Readers that take only
 * the first line keep working and get disc 1 -- which is what they got before
 * this existed -- so the format change is additive in the direction that
 * matters. A slot the player has not located is written as an EMPTY line
 * rather than skipped, so line N is always disc N and filling one in later
 * cannot renumber the others.
 *
 * The BIOS half is delegated to host_persist_setup: passing rom_path NULL runs
 * its bios.cfg logic and skips its single-path disc write, so the two entry
 * points cannot drift on BIOS handling. */
static int host_persist_setup_discs(void* ctx, const char* const* disc_paths,
                                    int disc_count, const char* bios_path) {
    char body[PSX_HOST_MAX_DISCS * 1030];
    char path[1200];
    size_t o = 0;
    int i, any = 0;

    host_persist_setup(ctx, NULL, bios_path);

    if (!disc_paths || disc_count <= 0)
        return 0;
    if (disc_count > PSX_HOST_MAX_DISCS)
        disc_count = PSX_HOST_MAX_DISCS;

    g_wizard_disc_count = 0;
    for (i = 0; i < disc_count; ++i) {
        const char* d = disc_paths[i] ? disc_paths[i] : "";
        snprintf(g_wizard_discs[i], sizeof(g_wizard_discs[i]), "%s", d);
        if (d[0]) g_wizard_disc_count = i + 1;   /* trailing blanks are not a set */
    }

    body[0] = '\0';
    for (i = 0; i < disc_count; ++i) {
        const char* d = disc_paths[i] ? disc_paths[i] : "";
        size_t n = strlen(d);
        if (o + n + 2 >= sizeof(body))
            return 0;
        if (i) body[o++] = '\n';
        if (n) { memcpy(body + o, d, n); o += n; any = 1; }
        body[o] = '\0';
    }
    /* Nothing located: leave any existing disc.cfg alone rather than replacing
     * it with a file of blank lines. */
    if (!any)
        return 0;

    if (g_project_root[0] &&
        join_path(path, sizeof(path), g_project_root, "disc.cfg"))
        write_line_file(path, body);
    write_line_file("disc.cfg", body);
    if (g_exe_path[0])
        write_sidecar_near_exe(g_exe_path, "disc.cfg", body);
    if (g_build_dir[0]) {
        char build_exe[1200];
        if (join_path(build_exe, sizeof(build_exe), g_build_dir, g_exe_basename))
            write_sidecar_near_exe(build_exe, "disc.cfg", body);
    }
    return 0;
}

/* Give the product the disc set the wizard recorded, when it has none.
 *
 * The wizard writes disc.cfg at the project root and tries build-release/ too,
 * but before the first build build-release/ does not exist, so that copy fails
 * silently. The runtime reads disc.cfg only beside its own exe, so the first
 * start asked for the disc again while the BIOS (which has a project-root
 * fallback below) was remembered. The whole file is copied, so a multi-disc
 * set keeps one path per line. An existing product-side disc.cfg is a pick
 * the player made in the product and is left alone. */
static void sync_project_disc_cfg_to_product(const char* near_exe) {
    char dir[1100], dst[1200], src[1200];
    char body[PSX_HOST_MAX_DISCS * 1030];
    size_t n;
    FILE* f;

    if (!near_exe || !near_exe[0] || !g_project_root[0])
        return;
    if (!dirname_copy(dir, sizeof(dir), near_exe) ||
        !join_path(dst, sizeof(dst), dir, "disc.cfg") ||
        !join_path(src, sizeof(src), g_project_root, "disc.cfg"))
        return;
    if (host_paths_same_file(src, dst))
        return;
    f = fopen(dst, "rb");
    if (f) {
        int c, has_content = 0;
        while ((c = fgetc(f)) != EOF) {
            if (!isspace((unsigned char)c)) {
                has_content = 1;
                break;
            }
        }
        /* A read error is not evidence that the player's file is empty. */
        if (ferror(f)) has_content = 1;
        fclose(f);
        if (has_content)
            return;
    }
    f = fopen(src, "rb");
    if (!f)
        return;
    n = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    body[n] = '\0';
    while (n && (body[n - 1] == '\n' || body[n - 1] == '\r' ||
                 body[n - 1] == ' ' || body[n - 1] == '\t'))
        body[--n] = '\0';
    if (!n)
        return;
    write_line_file(dst, body);
}

/* Give the product the settings saved beside the setup exe, when it has none.
 *
 * The setup host and the product each read settings.toml beside their own
 * exe, so the first start of the product lost the settings made in the
 * setup launcher. Every byte is copied. An existing product settings file,
 * even an empty one, is the player's and is left alone: the copy is written
 * to a temporary file that this call creates new, and is put in place only
 * when nothing has the product name yet. */
static void sync_project_settings_to_product(const char* near_exe) {
    char dir[1100], src[1200], dst[1200], tmp[1250];
    unsigned char bytes[16384];
    size_t n;
    int ok = 1;
    FILE* in;
    FILE* existing;
#if defined(_WIN32)
    HANDLE out;
#else
    int out;
#endif

    if (!near_exe || !near_exe[0] || !g_project_root[0] ||
        !dirname_copy(dir, sizeof(dir), near_exe) ||
        !join_path(src, sizeof(src), g_project_root, "settings.toml") ||
        !join_path(dst, sizeof(dst), dir, "settings.toml") ||
        host_paths_same_file(src, dst))
        return;
    in = fopen(src, "rb");
    if (!in)
        return;
    existing = fopen(dst, "rb");
    if (existing) {
        fclose(existing);
        fclose(in);
        return;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.setup.tmp", dst) >= (int)sizeof(tmp)) {
        fclose(in);
        return;
    }
#if defined(_WIN32)
    out = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        fclose(in);
        return;
    }
#else
    out = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out < 0) {
        fclose(in);
        return;
    }
#endif
    while (ok && (n = fread(bytes, 1, sizeof(bytes), in)) != 0) {
        size_t done = 0;
        while (done < n) {
#if defined(_WIN32)
            DWORD written = 0;
            if (!WriteFile(out, bytes + done, (DWORD)(n - done), &written, NULL) || !written) {
                ok = 0;
                break;
            }
#else
            ssize_t written = write(out, bytes + done, n - done);
            if (written <= 0) {
                if (written < 0 && errno == EINTR)
                    continue;
                ok = 0;
                break;
            }
#endif
            done += (size_t)written;
        }
    }
    if (ferror(in))
        ok = 0;
    fclose(in);
#if defined(_WIN32)
    if (!CloseHandle(out))
        ok = 0;
    /* Without MOVEFILE_REPLACE_EXISTING the move refuses an existing file. */
    if (ok)
        ok = MoveFileExA(tmp, dst, 0) != 0;
    if (!ok)
        DeleteFileA(tmp);
#else
    if (close(out) != 0)
        ok = 0;
    /* link refuses an existing file; rename would replace it. */
    if (ok)
        ok = link(tmp, dst) == 0;
    unlink(tmp);   /* only the temporary file this call created */
#endif
    if (!ok)
        fprintf(stderr, "psxrecomp-codegen: could not hand settings.toml to the product\n");
}

static void persist_relaunch_sidecars(const char* near_exe,
                                      const char* disc_path) {
    char bios_line[1100];
    char project_sidecar[1200];
    char abs_bios[1100];

    if (disc_path && disc_path[0]) {
        write_sidecar_near_exe(near_exe, "disc.cfg", disc_path);
        write_line_file("disc.cfg", disc_path);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "disc.cfg"))
            write_line_file(project_sidecar, disc_path);
    } else {
        sync_project_disc_cfg_to_product(near_exe);
    }
    sync_project_settings_to_product(near_exe);

    bios_line[0] = '\0';
    if (g_wizard_bios[0] &&
        absolutize_existing_file(abs_bios, sizeof(abs_bios), g_wizard_bios)) {
        snprintf(bios_line, sizeof(bios_line), "%s", abs_bios);
    } else if (g_wizard_bios_explicit) {
        /* Intentional OpenBIOS: clear sidecars — do not revive a stale
         * bios.cfg (e.g. root-owned file that remove() could not delete). */
        write_sidecar_near_exe(near_exe, "bios.cfg", "");
        write_line_file("bios.cfg", "");
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            write_line_file(project_sidecar, "");
        if (g_build_dir[0]) {
            char build_exe[1200];
            if (join_path(build_exe, sizeof(build_exe), g_build_dir,
                          g_exe_basename))
                write_sidecar_near_exe(build_exe, "bios.cfg", "");
        }
        return;
    } else {
        char line[1100];
        line[0] = '\0';
        if (!read_line_file("bios.cfg", line, sizeof(line)) &&
            g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            read_line_file(project_sidecar, line, sizeof(line));
        if (line[0])
            absolutize_existing_file(bios_line, sizeof(bios_line), line);
    }
    if (bios_line[0]) {
        snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", bios_line);
        write_sidecar_near_exe(near_exe, "bios.cfg", bios_line);
        write_line_file("bios.cfg", bios_line);
        if (g_project_root[0] &&
            join_path(project_sidecar, sizeof(project_sidecar), g_project_root,
                      "bios.cfg"))
            write_line_file(project_sidecar, bios_line);
    }
}

static int json_get_string(const char* line, const char* key, char* out,
                           size_t out_cap) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static int json_get_number(const char* line, const char* key, double* out) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(line, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') ++p;
    char* end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v;
    return 1;
}

static void handle_progress_line(const char* line,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!line || line[0] != '{')
        return;
    char event[64] = "";
    json_get_string(line, "event", event, sizeof(event));
    /* Capture ensure-toolchain result even when UI progress is absent. */
    if (strcmp(event, "result") == 0) {
        char tb[1400], resolved[1400];
        if (json_get_string(line, "toolchain_bin", tb, sizeof(tb)) && tb[0] &&
            resolve_existing_dir(tb, resolved, sizeof(resolved))) {
            snprintf(g_cli_toolchain_bin, sizeof(g_cli_toolchain_bin), "%s",
                     resolved);
            /* Env expects pack root (parent of bin/), not bin/ itself. */
            char pack_root[1400];
            if (dirname_copy(pack_root, sizeof(pack_root), resolved) &&
                pack_root[0]) {
#if defined(_WIN32)
                _putenv_s("PSXRECOMP_TOOLCHAIN_DIR", pack_root);
#else
                setenv("PSXRECOMP_TOOLCHAIN_DIR", pack_root, 1);
#endif
            }
        }
    }
    if (!on_progress)
        return;
    if (strcmp(event, "phase") == 0) {
        char message[240] = "";
        char phase[64] = "";
        double pct = -1.0;
        json_get_string(line, "message", message, sizeof(message));
        json_get_string(line, "phase", phase, sizeof(phase));
        if (!json_get_number(line, "pct", &pct))
            pct = -1.0;
        if (!message[0] && phase[0])
            snprintf(message, sizeof(message), "%s", phase);
        on_progress(progress_ctx, (float)pct, message[0] ? message : NULL);
    } else if (strcmp(event, "log") == 0 || strcmp(event, "error") == 0) {
        char message[240] = "";
        if (json_get_string(line, "message", message, sizeof(message)))
            on_progress(progress_ctx, -1.0f, message);
    }
}

/* Last-lines capture so a failed CLI run can say WHY in the wizard, which has
 * no console: the child's stderr shares the progress pipe, JSON progress rows
 * contribute their "message", raw rows (compiler/traceback text) contribute
 * as-is, and the most recent error-looking line is appended to err_msg. This
 * is what turns "psxrecomp rebuild failed (exit 1)" into "… failed (exit 1):
 * Could NOT find OpenGL (missing: OPENGL_INCLUDE_DIR)". */
typedef struct {
    char last[480];
    char last_err[480];
} CliTail;

static int cli_tail_line_is_error(const char* s) {
    return strstr(s, "rror") != NULL || strstr(s, "ailed") != NULL ||
           strstr(s, "FAILED") != NULL || strstr(s, "Traceback") != NULL ||
           strstr(s, "fatal") != NULL || strstr(s, "Fatal") != NULL;
}

static void cli_tail_note(CliTail* t, const char* line) {
    char msg[480];
    const char* rec = line;
    int is_error_event = 0;
    if (line[0] == '{') {
        /* sdk_progress emits compact JSON: {"event":"error","message":…}. */
        is_error_event = strstr(line, "\"event\":\"error\"") != NULL;
        if (!json_get_string(line, "message", msg, sizeof(msg)))
            return; /* structured row without text (e.g. result) */
        rec = msg;
    }
    if (!rec[0])
        return;
    snprintf(t->last, sizeof(t->last), "%s", rec);
    if (is_error_event || cli_tail_line_is_error(rec))
        snprintf(t->last_err, sizeof(t->last_err), "%s", rec);
}

static void cli_fail_msg(char* err_msg, size_t err_cap, const char* fail_label,
                         long code, const CliTail* t) {
    const char* why = t->last_err[0] ? t->last_err : t->last;
    if (code == 3) {
        /* The CLI names the failing check (a digest mismatch, or a .chd it
         * cannot read) and cli_tail_note has captured that line. A flat
         * "wrong dump" contradicts it and sends players hunting a bad rip
         * when the dump is fine. Keep the reason when there is one. */
        if (why[0])
            snprintf(err_msg, err_cap, "Disc verification failed: %s", why);
        else
            snprintf(err_msg, err_cap, "Disc verification failed (wrong dump).");
        return;
    }
    if (why[0])
        snprintf(err_msg, err_cap, "%s failed (exit %ld): %s", fail_label,
                 code, why);
    else
        snprintf(err_msg, err_cap, "%s failed (exit %ld).", fail_label, code);
}

#if defined(_WIN32)
static int run_cli_win(const char* cmdline,
                       RecompLauncherCPrepareProgressFn on_progress,
                       void* progress_ctx, char* err_msg, size_t err_cap,
                       const char* fail_label) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        snprintf(err_msg, err_cap, "CreatePipe failed.");
        return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    /* stderr shares the pipe: Python tracebacks and raw tool output feed the
     * CliTail capture instead of vanishing (the wizard has no console). */
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    char mutable_cmd[4096];
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, 0, NULL,
                        g_project_root, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        snprintf(err_msg, err_cap, "Failed to spawn %s.", fail_label);
        return 0;
    }
    CloseHandle(wr);

    char buf[512];
    /* Long JSONL rows (toolchain paths under LocalAppData) need headroom. */
    char line[16384];
    size_t line_len = 0;
    DWORD n = 0;
    CliTail tail;
    memset(&tail, 0, sizeof(tail));
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = '\0';
                handle_progress_line(line, on_progress, progress_ctx);
                cli_tail_note(&tail, line);
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line))
                line[line_len++] = c;
        }
    }
    if (line_len) {
        line[line_len] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
        cli_tail_note(&tail, line);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0) return 1;
    cli_fail_msg(err_msg, err_cap, fail_label, (long)code, &tail);
    return 0;
}
#else
static int run_cli_posix(char* const argv[],
                         RecompLauncherCPrepareProgressFn on_progress,
                         void* progress_ctx, char* err_msg, size_t err_cap,
                         const char* fail_label) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(err_msg, err_cap, "pipe() failed: %s", strerror(errno));
        return 0;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    /* stderr shares the pipe: Python tracebacks and raw tool output feed the
     * CliTail capture instead of vanishing (the wizard has no console). */
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        snprintf(err_msg, err_cap, "Failed to spawn %s: %s", fail_label,
                 strerror(rc));
        return 0;
    }

    FILE* out = fdopen(pipefd[0], "r");
    if (!out) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        snprintf(err_msg, err_cap, "fdopen failed.");
        return 0;
    }
    char line[16384];
    CliTail tail;
    memset(&tail, 0, sizeof(tail));
    while (fgets(line, sizeof(line), out)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        handle_progress_line(line, on_progress, progress_ctx);
        cli_tail_note(&tail, line);
    }
    fclose(out);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(err_msg, err_cap, "waitpid failed: %s", strerror(errno));
        return 0;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 0) return 1;
    cli_fail_msg(err_msg, err_cap, fail_label, (long)code, &tail);
    return 0;
}
#endif

/* ---- Host-native toolchain install (no Store Python AppData redirect) ---- */

static const char* k_tc_repo = "RetroPortingToolKit/RetroPorting-Toolchains";

static const char* toolchain_zip_asset_name(void) {
#if defined(_WIN32)
    return "cmake-clang-v1-windows-x64.zip";
#elif defined(__APPLE__)
    return "cmake-clang-v1-macos-universal.zip";
#else
    return "cmake-clang-v1-linux-x64.zip";
#endif
}

/* Framework floor for accepted packs, not a per-title pin. v1.0.14 is the
 * first cmake-clang-v1 with the Linux build sysroot; older cached packs
 * compile against host headers (absent on stock SteamOS) and mis-link after a
 * pack upgrade (__isoc23_strtoul). The cache is otherwise accepted forever
 * once any cmake runs, so the floor is what makes a stale pre-sysroot pack
 * get replaced. RETCOMM_TOOLCHAIN_MIN_VERSION overrides in either direction;
 * "0" / "off" / "none" disables the floor (pre-floor behavior). Mirrors
 * tools/toolchain_pack.py default_min_version(). */
#define TOOLCHAIN_DEFAULT_MIN_VERSION "1.0.14"

static const char* toolchain_min_version(void) {
    const char* env = getenv("RETCOMM_TOOLCHAIN_MIN_VERSION");
    if (env && env[0]) {
        if (strcmp(env, "0") == 0 || ascii_strcasecmp(env, "off") == 0 ||
            ascii_strcasecmp(env, "none") == 0)
            return "";
        return env;
    }
    return TOOLCHAIN_DEFAULT_MIN_VERSION;
}

/* Parse leading dotted integers from a version / tag (optional leading 'v'). */
static int version_cmp(const char* a, const char* b) {
    const char* pa = a ? a : "";
    const char* pb = b ? b : "";
    if ((pa[0] == 'v' || pa[0] == 'V') && pa[1] >= '0' && pa[1] <= '9')
        ++pa;
    if ((pb[0] == 'v' || pb[0] == 'V') && pb[1] >= '0' && pb[1] <= '9')
        ++pb;
    for (;;) {
        long va = 0, vb = 0;
        int ha = 0, hb = 0;
        while (*pa >= '0' && *pa <= '9') {
            va = va * 10 + (*pa - '0');
            ++pa;
            ha = 1;
        }
        while (*pb >= '0' && *pb <= '9') {
            vb = vb * 10 + (*pb - '0');
            ++pb;
            hb = 1;
        }
        if (!ha && !hb)
            return 0;
        if (va != vb)
            return va < vb ? -1 : 1;
        if (*pa == '.')
            ++pa;
        if (*pb == '.')
            ++pb;
        if (!ha || !hb)
            return ha ? 1 : (hb ? -1 : 0);
    }
}

static int read_pack_version(const char* pack_root, char* out, size_t cap) {
    char meta[1400], buf[4096];
    FILE* f;
    const char* p;
    size_t n;
    if (!pack_root || !pack_root[0] || !out || cap < 2)
        return 0;
    out[0] = '\0';
    if (!join_path(meta, sizeof(meta), pack_root, "retcomm-toolchain.json"))
        return 0;
    f = fopen(meta, "rb");
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strstr(buf, "\"version\"");
    if (!p)
        return 0;
    p = strchr(p + 9, '"');
    if (!p)
        return 0;
    ++p;
    {
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < cap) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return i > 0;
    }
}

static int pack_meets_min_version(const char* pack_root) {
    char ver[64];
    const char* need = toolchain_min_version();
    if (!need || !need[0])
        return 1;
    if (!read_pack_version(pack_root, ver, sizeof(ver)))
        return 0;
    return version_cmp(ver, need) >= 0;
}

static int mkdir_p(const char* path) {
    char tmp[1400];
    size_t n;
    if (!path || !path[0])
        return 0;
    n = strlen(path);
    if (n >= sizeof(tmp))
        return 0;
    memcpy(tmp, path, n + 1);
#if defined(_WIN32)
    {
        char* p = tmp;
        if (n >= 2 && tmp[1] == ':')
            p = tmp + 2;
        while (*p == '\\' || *p == '/')
            ++p;
        for (; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                char save = *p;
                *p = '\0';
                if (tmp[0])
                    CreateDirectoryA(tmp, NULL);
                *p = save;
            }
        }
        return CreateDirectoryA(tmp, NULL) ||
               GetLastError() == ERROR_ALREADY_EXISTS || path_is_dir(tmp);
    }
#else
    {
        char* p = tmp;
        if (*p == '/')
            ++p;
        for (; *p; ++p) {
            if (*p == '/') {
                *p = '\0';
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST && !path_is_dir(tmp))
                    return 0;
                *p = '/';
            }
        }
        return mkdir(tmp, 0755) == 0 || errno == EEXIST || path_is_dir(tmp);
    }
#endif
}

static int rmtree_path(const char* path) {
    char cmd[1600];
    if (!path || !path[0] || !path_is_dir(path))
        return 1;
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", path);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
#endif
    return system(cmd) == 0 || !path_is_dir(path);
}

/* PSXRECOMP_TOOLCHAIN_READONLY=1: this process never deletes, renames, prunes
 * or installs a toolchain pack and never moves a pointer to one (the `latest`
 * link, the project's toolchain link and stamp). It still finds and uses a
 * pack that is there. Tests and gates that start a setup host set it, so that
 * such a host cannot install a pack over the machine's own toolchain while
 * builds run from it. Every function below that changes a pack or a pointer
 * asks toolchain_readonly_skip() first. */
static int toolchain_readonly(void) {
    const char* e = getenv("PSXRECOMP_TOOLCHAIN_READONLY");
    return e && e[0] && e[0] != '0';
}

/* True under the switch, after saying on stderr what was left undone. */
static int toolchain_readonly_skip(const char* what, const char* path) {
    if (!toolchain_readonly())
        return 0;
    fprintf(stderr,
            "psxrecomp-codegen: toolchain read-only (PSXRECOMP_TOOLCHAIN_READONLY): "
            "not done: %s%s%s\n",
            what, (path && path[0]) ? ": " : "", (path && path[0]) ? path : "");
    return 1;
}

/* The order of every change to an installed pack. A pack that may be in use
 * is never removed first: an install that removed the installed tag before
 * the new pack was in place left running builds with only the files they
 * held open.
 *
 *   - a new pack is unpacked beside the installed one and checked there;
 *   - the installed folder is then renamed aside (set_dir_aside). A folder with
 *     an open file cannot be renamed on Windows: the installed pack then stays
 *     and the new one is dropped;
 *   - the new pack takes the name, the pointer follows, and only then the
 *     folder that was set aside is removed. What cannot be removed whole stays
 *     under its dot-name (no lookup reads dot-names) and goes on a later pass;
 *   - a pack that fails its check is reported, never removed or renamed. */
#define TOOLCHAIN_ASIDE_PREFIX ".old-"

static int path_exists_any(const char* path) {
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(path, &st) == 0;
#endif
}

/* A symlink or a Windows junction: a pointer, not a pack. */
static int path_is_link(const char* path) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

/* Remove a link itself, never what it points at. */
static int remove_pointer_only(const char* path) {
    if (!path_is_link(path))
        return 0;
#if defined(_WIN32)
    return RemoveDirectoryA(path) || DeleteFileA(path);
#else
    return unlink(path) == 0;
#endif
}

/* Rename a folder to a dot-name beside it. 0 when it cannot be renamed (a file
 * in it is open): nothing has changed then. */
static int set_dir_aside(const char* path, char* aside, size_t cap) {
    char parent[1400], name[300];
    const char* base = path_base_name(path);
    int n;
    aside[0] = '\0';
    if (!base || !base[0] || !dirname_copy(parent, sizeof(parent), path))
        return 0;
    for (n = 0; n < 100; ++n) {
#if defined(_WIN32)
        unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        /* .old-<pid>[.<n>]-<name>: the owner's process id first, so that the
         * name of the folder can be read back whatever characters it has. */
        if (n)
            snprintf(name, sizeof(name), TOOLCHAIN_ASIDE_PREFIX "%lu.%d-%s", pid, n, base);
        else
            snprintf(name, sizeof(name), TOOLCHAIN_ASIDE_PREFIX "%lu-%s", pid, base);
        if (!join_path(aside, cap, parent, name))
            break;
        if (path_exists_any(aside))
            continue;
#if defined(_WIN32)
        if (MoveFileExA(path, aside, 0))
            return 1;
        g_tc_aside_error = (long)GetLastError();
#else
        if (rename(path, aside) == 0)
            return 1;
        g_tc_aside_error = (long)errno;
#endif
        break;
    }
    aside[0] = '\0';
    return 0;
}

/* Read ".old-<pid>[.<n>]-<name>". 1 and the two parts when the entry is one. */
static int aside_entry_parse(const char* entry, unsigned long* pid, const char** name) {
    const char* p = entry;
    unsigned long v = 0;
    size_t plen = sizeof(TOOLCHAIN_ASIDE_PREFIX) - 1;
    if (strncmp(p, TOOLCHAIN_ASIDE_PREFIX, plen) != 0)
        return 0;
    p += plen;
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (unsigned long)(*p++ - '0');
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9')
            return 0;
        while (*p >= '0' && *p <= '9')
            ++p;
    }
    if (*p != '-' || !p[1])
        return 0;
    *pid = v;
    *name = p + 1;
    return 1;
}

/* Is the process that set a folder aside still running? This process counts
 * as not running: what it set aside and did not put back is its own to handle. */
static int aside_owner_is_running(unsigned long pid) {
#if defined(_WIN32)
    HANDLE h;
    DWORD code = 0;
    int running;
    if (pid == (unsigned long)GetCurrentProcessId())
        return 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;   /* there, and not ours to read */
    running = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return running;
#else
    if (pid == (unsigned long)getpid())
        return 0;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

/* What to do with one set-aside folder of a cache base.
 *   0  leave it: its owner is still running (it may be in the middle of an
 *      install and need the folder to put back);
 *   1  put it back: the folder it was taken from is missing, so an install
 *      was interrupted between the rename aside and the arrival of the new
 *      pack. The whole old pack is in it;
 *   2  remove it: the replacement is in place and nobody needs it. */
static int aside_entry_fate(const char* base, const char* entry, char* target, size_t cap) {
    unsigned long pid = 0;
    const char* name = NULL;
    if (!aside_entry_parse(entry, &pid, &name))
        return 0;
    if (aside_owner_is_running(pid))
        return 0;
    if (!join_path(target, cap, base, name))
        return 0;
    return path_exists_any(target) ? 2 : 1;
}

/* Put back, in every cache base, the folders an interrupted install left set
 * aside (their name is missing). Nothing reads a dot-name, so without this the
 * pack would be lost to every lookup and removed by the next prune. Called
 * before a check of the pointers and before a prune. */
static void toolchain_put_back_set_aside(void) {
    char bases[12][1400];
    int n = collect_toolchain_cache_bases(bases, 12), i;
    for (i = 0; i < n; ++i) {
        char names[16][300];
        int count = 0, k;
#if defined(_WIN32)
        WIN32_FIND_DATAA fd;
        char pattern[1500];
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\" TOOLCHAIN_ASIDE_PREFIX "*", bases[i]);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < 16)
                snprintf(names[count++], sizeof(names[0]), "%s", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        DIR* d = opendir(bases[i]);
        struct dirent* ent;
        if (!d)
            continue;
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, TOOLCHAIN_ASIDE_PREFIX, sizeof(TOOLCHAIN_ASIDE_PREFIX) - 1) == 0 && count < 16)
                snprintf(names[count++], sizeof(names[0]), "%s", ent->d_name);
        }
        closedir(d);
#endif
        for (k = 0; k < count; ++k) {
            char aside[1800], target[1800];
            if (aside_entry_fate(bases[i], names[k], target, sizeof(target)) != 1)
                continue;
            if (!join_path(aside, sizeof(aside), bases[i], names[k]) || !pack_root_has_cmake_direct(aside))
                continue;   /* not a pack: nothing to put back */
            if (toolchain_readonly_skip("put back a pack an interrupted install left set aside", aside))
                continue;
            if (restore_dir_aside(aside, target))
                fprintf(stderr,
                        "psxrecomp-codegen: an install was interrupted; the toolchain pack "
                        "it had set aside is put back: %s\n", target);
        }
    }
}

/* Put a folder that was set aside back under its name. */
static int restore_dir_aside(const char* aside, const char* path) {
    if (!aside || !aside[0] || path_exists_any(path))
        return 0;
#if defined(_WIN32)
    return MoveFileExA(aside, path, 0) != 0;
#else
    return rename(aside, path) == 0;
#endif
}

/* Remove a folder that was set aside. 1 when it is gone whole. */
static int remove_dir_aside(const char* aside) {
    if (!aside || !aside[0])
        return 1;
    rmtree_path(aside);
    return !path_exists_any(aside);
}

#if defined(_WIN32)
static int run_cmdline_wait(const char* cmdline, DWORD* out_code) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char mutable_cmd[8192];
    DWORD code = 1;
    HANDLE hin, hout, herr;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    /* GUI launchers often have INVALID std handles. Passing those with
     * STARTF_USESTDHANDLES makes console children (cmake/curl) fail even when
     * the exe is fine — do not inherit broken handles. */
    hin = GetStdHandle(STD_INPUT_HANDLE);
    hout = GetStdHandle(STD_OUTPUT_HANDLE);
    herr = GetStdHandle(STD_ERROR_HANDLE);
    if (hin != INVALID_HANDLE_VALUE && hin != NULL &&
        hout != INVALID_HANDLE_VALUE && hout != NULL &&
        herr != INVALID_HANDLE_VALUE && herr != NULL) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hin;
        si.hStdOutput = hout;
        si.hStdError = herr;
    }
    snprintf(mutable_cmd, sizeof(mutable_cmd), "%s", cmdline);
    if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (out_code)
        *out_code = code;
    return 1;
}
#endif

static int shared_toolchain_latest_dir(char* out, size_t cap) {
    char cache[1400];
    if (!preferred_toolchain_cache_root(cache, sizeof(cache)))
        return 0;
    return join_path(out, cap, cache, "latest");
}

/* True when root/bin/cmake(.exe) exists (no nested unwrap). */
static int pack_root_has_cmake_direct(const char* root) {
    char bin[1400], cmake[1400];
    if (!root || !root[0])
        return 0;
    if (!join_path(bin, sizeof(bin), root, "bin"))
        return 0;
#if defined(_WIN32)
    return join_path(cmake, sizeof(cmake), bin, "cmake.exe") && path_is_file(cmake);
#else
    return join_path(cmake, sizeof(cmake), bin, "cmake") && path_is_file(cmake);
#endif
}

/* Resolve pack root with bin/cmake — flat or single nested child (zip layout). */
static int unwrap_toolchain_pack_root(const char* in, char* out, size_t cap) {
    char bin[1400];
    if (!in || !in[0] || !out || cap < 2)
        return 0;
    if (pack_root_has_cmake_direct(in)) {
        snprintf(out, cap, "%s", in);
        return 1;
    }
    if (resolve_toolchain_bin_under(in, bin, sizeof(bin)) &&
        dirname_copy(out, cap, bin))
        return pack_root_has_cmake_direct(out);
    return 0;
}

static int pack_root_has_cmake(const char* root) {
    char unwrapped[1400];
    return unwrap_toolchain_pack_root(root, unwrapped, sizeof(unwrapped));
}

/* Read retcomm-toolchain.json "version" (best-effort; empty if missing). */
static void read_toolchain_pack_version(const char* pack_root, char* out,
                                        size_t cap) {
    char path[1400], buf[2048];
    FILE* f;
    out[0] = '\0';
    if (!pack_root || !pack_root[0] || !out || cap < 2)
        return;
    if (!join_path(path, sizeof(path), pack_root, "retcomm-toolchain.json"))
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char* p = strstr(buf, "\"version\"");
    if (!p)
        return;
    p = strchr(p + 9, '"');
    if (!p)
        return;
    ++p;
    const char* end = strchr(p, '"');
    if (!end || end <= p)
        return;
    size_t len = (size_t)(end - p);
    if (len >= cap)
        len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static void sanitize_toolchain_tag(const char* ver, char* out, size_t cap) {
    size_t j = 0;
    if (!out || cap < 2) return;
    if (!ver || !ver[0]) {
        snprintf(out, cap, "offline");
        return;
    }
    for (size_t i = 0; ver[i] && j + 1 < cap; ++i) {
        unsigned char c = (unsigned char)ver[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out[j++] = (char)c;
        else
            out[j++] = '_';
    }
    out[j] = '\0';
    if (!out[0])
        snprintf(out, cap, "offline");
}

/* Point cache_root/latest at pack_root (symlink preferred; copy fallback).
 * Never install pack contents *into* latest/ — latest is pointer-only. */
static int set_toolchain_latest_pointer(const char* cache_root,
                                        const char* pack_root) {
    char latest[1400], resolved_pack[1400];
    if (!cache_root || !pack_root || !pack_root[0])
        return 0;
    if (!unwrap_toolchain_pack_root(pack_root, resolved_pack,
                                   sizeof(resolved_pack)))
        return 0;
    if (!join_path(latest, sizeof(latest), cache_root, "latest"))
        return 0;
    /* Already points at a usable pack (symlink or real dir with bin/). */
    if (pack_root_has_cmake_direct(latest)) {
#if !defined(_WIN32)
        char latest_res[1400], pack_res[1400];
        if (realpath(latest, latest_res) && realpath(resolved_pack, pack_res) &&
            strcmp(latest_res, pack_res) == 0)
            return 1;
#endif
        if (strcmp(latest, resolved_pack) == 0)
            return 1;
    }
    if (toolchain_readonly_skip("move the latest pointer", latest))
        return pack_root_has_cmake_direct(latest);
    {
        char aside[1500];
        int made = 0;
        aside[0] = '\0';
        if (path_is_link(latest)) {
            remove_pointer_only(latest);
        } else if (path_is_dir(latest)) {
            /* A real folder: an older layout's pack, or the copy made below
             * when a link could not be made. Out of use first, removed after
             * the new pointer is there. A folder in use stays as it is. */
            if (!set_dir_aside(latest, aside, sizeof(aside)))
                return pack_root_has_cmake_direct(latest);
        }
#if defined(_WIN32)
        if (junction_dir(latest, resolved_pack)) {
            made = 1;
        } else {
            char cmd[3200];
            DWORD code = 1;
            mkdir_p(latest);
            snprintf(cmd, sizeof(cmd),
                     "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                     "/ns /np",
                     resolved_pack, latest);
            if (run_cmdline_wait(cmd, &code) && code <= 7 &&
                pack_root_has_cmake_direct(latest))
                made = 1;
        }
#else
        if (symlink(resolved_pack, latest) == 0) {
            made = 1;
        } else {
            char cmd[3200];
            snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", resolved_pack, latest);
            if (system(cmd) == 0 && pack_root_has_cmake_direct(latest))
                made = 1;
        }
#endif
        if (made) {
            remove_dir_aside(aside);
            return 1;
        }
        /* No pointer could be made: the folder that was there goes back. */
        if (aside[0]) {
            if (path_is_link(latest))
                remove_pointer_only(latest);
            else if (path_is_dir(latest) && !pack_root_has_cmake_direct(latest))
                rmtree_path(latest);   /* the copy that did not finish; ours */
            restore_dir_aside(aside, latest);
        }
    }
    return pack_root_has_cmake_direct(latest);
}

static int write_project_toolchain_stamp(const char* bin_dir) {
    char tc_dir[1200], stamp[1200];
    FILE* f;
    if (!g_project_root[0] || !bin_dir || !bin_dir[0])
        return 0;
    if (!join_path(tc_dir, sizeof(tc_dir), g_project_root, "toolchain"))
        return 0;
    if (toolchain_readonly_skip("write the project's toolchain stamp", tc_dir))
        return 0;
    mkdir_p(tc_dir);
    if (!join_path(stamp, sizeof(stamp), tc_dir, ".psxrecomp-bin"))
        return 0;
    f = fopen(stamp, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", bin_dir);
    fclose(f);
    return 1;
}

static int activate_installed_pack_root(const char* pack_root) {
    char root[1400], bin[1400];
    if (!unwrap_toolchain_pack_root(pack_root, root, sizeof(root)))
        return 0;
    if (!join_path(bin, sizeof(bin), root, "bin"))
        return 0;
    snprintf(g_cli_toolchain_bin, sizeof(g_cli_toolchain_bin), "%s", bin);
#if defined(_WIN32)
    _putenv_s("RETCOMM_TOOLCHAIN_DIR", root);
    _putenv_s("PSXRECOMP_TOOLCHAIN_DIR", root);
#else
    setenv("RETCOMM_TOOLCHAIN_DIR", root, 1);
    setenv("PSXRECOMP_TOOLCHAIN_DIR", root, 1);
#endif
    write_project_toolchain_stamp(bin);
    activate_toolchain_path();
    /* File presence is not enough — cmake and clang/lld must actually run
     * (missing DLLs / libicu / MOTW used to pass find_cmake then fail later). */
    if (!find_cmake(g_cmake, sizeof(g_cmake)))
        return 0;
    if (!cmake_path_runs(g_cmake))
        return 0;
    return toolchain_bin_is_healthy(bin);
}

#if defined(_WIN32)
/* Point a real LocalAppData path at a Store-Python LocalCache pack (no copy). */
static int junction_dir(const char* link_path, const char* target_path) {
    char cmd[3200];
    DWORD code = 1;
    char parent[1400];
    if (!link_path || !target_path || !path_is_dir(target_path))
        return 0;
    if (path_is_dir(link_path) || path_is_file(link_path)) {
        /* Replace broken/empty dir; keep a usable pack. */
        if (pack_root_has_cmake(link_path))
            return 1;
        if (toolchain_readonly_skip("replace a toolchain folder by a link", link_path))
            return 0;
        if (path_is_link(link_path)) {
            remove_pointer_only(link_path);
        } else {
            /* A folder without a usable pack. Out of use first, then removed;
             * when a file in it is open it stays and no link is made. */
            char aside[1500];
            if (!set_dir_aside(link_path, aside, sizeof(aside)))
                return 0;
            remove_dir_aside(aside);
        }
    }
    if (toolchain_readonly_skip("make a toolchain link", link_path))
        return 0;
    if (!dirname_copy(parent, sizeof(parent), link_path))
        return 0;
    mkdir_p(parent);
    snprintf(cmd, sizeof(cmd), "cmd.exe /c mklink /J \"%s\" \"%s\"", link_path,
             target_path);
    if (!run_cmdline_wait(cmd, &code))
        return 0;
    return code == 0 && path_is_dir(link_path);
}

static int find_store_localcache_pack_root(char* out, size_t cap) {
    const char* local = getenv("LOCALAPPDATA");
    char packages[1100], pattern[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!local || !local[0])
        return 0;
    if (!join_path(packages, sizeof(packages), local, "Packages"))
        return 0;
    snprintf(pattern, sizeof(pattern),
             "%s\\PythonSoftwareFoundation.Python.*", packages);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char* suffixes[] = {
            "LocalCache\\Local\\retcomm\\toolchains\\cmake-clang-v1\\latest",
            "LocalCache\\Local\\retcomm\\toolchains\\cmake-clang-v1\\offline",
            "LocalCache\\Local\\psxrecomp\\toolchains\\cmake-clang-v1\\latest",
            "LocalCache\\Local\\psxrecomp\\toolchains\\cmake-clang-v1\\offline",
            NULL};
        int i;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        for (i = 0; suffixes[i]; ++i) {
            char cand[1400], nested[1400];
            snprintf(cand, sizeof(cand), "%s\\%s\\%s", packages, fd.cFileName,
                     suffixes[i]);
            if (pack_root_has_cmake(cand)) {
                snprintf(out, cap, "%s", cand);
                FindClose(h);
                return 1;
            }
            /* Nested single child with bin/cmake.exe */
            if (path_is_dir(cand)) {
                WIN32_FIND_DATAA kd;
                char kp[1400];
                HANDLE hk;
                snprintf(kp, sizeof(kp), "%s\\*", cand);
                hk = FindFirstFileA(kp, &kd);
                if (hk == INVALID_HANDLE_VALUE)
                    continue;
                do {
                    if (!(kd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        continue;
                    if (kd.cFileName[0] == '.')
                        continue;
                    if (!join_path(nested, sizeof(nested), cand, kd.cFileName))
                        continue;
                    if (pack_root_has_cmake(nested)) {
                        snprintf(out, cap, "%s", nested);
                        FindClose(hk);
                        FindClose(h);
                        return 1;
                    }
                } while (FindNextFileA(hk, &kd));
                FindClose(hk);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* allow_copy=0: point this process at LocalCache (cheap).
 * allow_copy=1: also junction/robocopy into real %LOCALAPPDATA% (ensure). */
static int harvest_store_python_toolchain(int allow_copy) {
    char cache_root[1400], real_latest[1400], proj_tc[1200];
    if (!find_store_localcache_pack_root(cache_root, sizeof(cache_root)))
        return 0;
    if (allow_copy && toolchain_readonly_skip("copy or link a pack into the shared cache", cache_root))
        allow_copy = 0;
    if (!allow_copy)
        return activate_installed_pack_root(cache_root);
    if (!shared_toolchain_latest_dir(real_latest, sizeof(real_latest)))
        return activate_installed_pack_root(cache_root);
    if (!junction_dir(real_latest, cache_root) &&
        !pack_root_has_cmake(real_latest)) {
        /* Junction failed — robocopy into the real tree. */
        char parent[1400], cmd[3200];
        DWORD code = 1;
        if (!dirname_copy(parent, sizeof(parent), real_latest))
            return activate_installed_pack_root(cache_root);
        mkdir_p(parent);
        /* junction_dir took an unusable folder out of the way. What is still
         * there is in use: it stays, and this process uses the pack where it
         * is. */
        if (path_exists_any(real_latest))
            return activate_installed_pack_root(cache_root);
        mkdir_p(real_latest);
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                 "/ns /np",
                 cache_root, real_latest);
        if (!run_cmdline_wait(cmd, &code) || code > 7 ||
            !pack_root_has_cmake(real_latest))
            return activate_installed_pack_root(cache_root);
    }
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc))
            junction_dir(proj_tc, pack_root_has_cmake(real_latest)
                                      ? real_latest
                                      : cache_root);
    }
    return activate_installed_pack_root(
        pack_root_has_cmake(real_latest) ? real_latest : cache_root);
}

static int host_download_url_to_file(const char* url, const char* dest,
                                     char* err_msg, size_t err_cap) {
    char cmd[4096];
    DWORD code = 1;
    char parent[1400];
    if (!dirname_copy(parent, sizeof(parent), dest)) {
        snprintf(err_msg, err_cap, "Bad download destination.");
        return 0;
    }
    mkdir_p(parent);
    DeleteFileA(dest);
    /* Windows 10+ ships curl.exe. -L follows GitHub release redirects. */
    snprintf(cmd, sizeof(cmd),
             "curl.exe -fsSL --retry 3 --retry-delay 2 -o \"%s\" \"%s\"", dest,
             url);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        if (code == 23)   /* curl's write error: nothing could be written */
            toolchain_text_download_failed(err_msg, err_cap,
                                           TOOLCHAIN_DOWNLOAD_NO_WRITE);
        else
            snprintf(err_msg, err_cap,
                     "Toolchain download failed (curl exit %lu). Check network / "
                     "curl.exe.",
                     (unsigned long)code);
        return 0;
    }
    if (!path_is_file(dest)) {   /* curl is done and the pack is not there */
        toolchain_text_download_failed(err_msg, err_cap,
                                       TOOLCHAIN_DOWNLOAD_NO_WRITE);
        return 0;
    }
    return 1;
}

static int host_extract_zip(const char* zip_path, const char* dest_dir,
                            char* err_msg, size_t err_cap) {
    char cmd[3200];
    DWORD code = 1;
    char parent[1400];
    if (!path_is_file(zip_path)) {
        snprintf(err_msg, err_cap, "Toolchain zip not found: %s", zip_path);
        return 0;
    }
    if (!dirname_copy(parent, sizeof(parent), dest_dir)) {
        snprintf(err_msg, err_cap, "Bad extract destination.");
        return 0;
    }
    mkdir_p(parent);
    rmtree_path(dest_dir);
    mkdir_p(dest_dir);
    /* tar.exe on Windows 10+ extracts .zip */
    snprintf(cmd, sizeof(cmd), "tar.exe -xf \"%s\" -C \"%s\"", zip_path,
             dest_dir);
    if (!run_cmdline_wait(cmd, &code) || code != 0) {
        snprintf(err_msg, err_cap, "Failed to extract toolchain zip (tar exit %lu).",
                 (unsigned long)code);
        return 0;
    }
    if (pack_root_has_cmake(dest_dir))
        return 1;
    /* Single nested directory layout — resolve_toolchain_bin_under handles it. */
    {
        WIN32_FIND_DATAA fd;
        char pattern[1400], child[1400];
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", dest_dir);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (fd.cFileName[0] == '.')
                    continue;
                if (!join_path(child, sizeof(child), dest_dir, fd.cFileName))
                    continue;
                if (pack_root_has_cmake(child)) {
                    FindClose(h);
                    return 1;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.exe.");
    return 0;
}
#else
static int host_download_url_to_file(const char* url, const char* dest,
                                     char* err_msg, size_t err_cap) {
    char cmd[4096];
    char parent[1400];
    int status;
    if (!dirname_copy(parent, sizeof(parent), dest)) {
        snprintf(err_msg, err_cap, "Bad download destination.");
        return 0;
    }
    mkdir_p(parent);
    unlink(dest);
    snprintf(cmd, sizeof(cmd),
             "curl -fsSL --retry 3 --retry-delay 2 -o \"%s\" \"%s\"", dest, url);
    status = system(cmd);
    if (status != 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 23)   /* curl's write error */
            toolchain_text_download_failed(err_msg, err_cap,
                                           TOOLCHAIN_DOWNLOAD_NO_WRITE);
        else
            snprintf(err_msg, err_cap, "Toolchain download failed (curl).");
        return 0;
    }
    if (!path_is_file(dest)) {   /* curl is done and the pack is not there */
        toolchain_text_download_failed(err_msg, err_cap,
                                       TOOLCHAIN_DOWNLOAD_NO_WRITE);
        return 0;
    }
    return 1;
}

static int host_extract_zip(const char* zip_path, const char* dest_dir,
                            char* err_msg, size_t err_cap) {
    char cmd[3200];
    char parent[1400];
    if (!path_is_file(zip_path)) {
        snprintf(err_msg, err_cap, "Toolchain zip not found: %s", zip_path);
        return 0;
    }
    if (!dirname_copy(parent, sizeof(parent), dest_dir)) {
        snprintf(err_msg, err_cap, "Bad extract destination.");
        return 0;
    }
    mkdir_p(parent);
    rmtree_path(dest_dir);
    mkdir_p(dest_dir);
    snprintf(cmd, sizeof(cmd), "unzip -q \"%s\" -d \"%s\"", zip_path, dest_dir);
    if (system(cmd) != 0) {
        snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\"", zip_path,
                 dest_dir);
        if (system(cmd) != 0) {
            snprintf(err_msg, err_cap, "Failed to extract toolchain zip.");
            return 0;
        }
    }
    if (pack_root_has_cmake(dest_dir))
        return 1;
    /* Nested child with bin/ is fine — resolve_toolchain_bin_under handles it. */
    {
        DIR* dir = opendir(dest_dir);
        struct dirent* ent;
        if (!dir) {
            snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.");
            return 0;
        }
        while ((ent = readdir(dir)) != NULL) {
            char child[1400];
            if (ent->d_name[0] == '.')
                continue;
            if (!join_path(child, sizeof(child), dest_dir, ent->d_name))
                continue;
            if (path_is_dir(child) && pack_root_has_cmake(child)) {
                closedir(dir);
                return 1;
            }
        }
        closedir(dir);
    }
    snprintf(err_msg, err_cap, "Toolchain zip missing bin/cmake.");
    return 0;
}
#endif

static int link_or_stamp_project_toolchain(const char* pack_root) {
    char root[1400], proj_tc[1200], bin[1400];
    if (!unwrap_toolchain_pack_root(pack_root, root, sizeof(root)))
        return 0;
    if (!join_path(bin, sizeof(bin), root, "bin"))
        return 0;
    if (toolchain_readonly_skip("link the pack into the project", g_project_root))
        return 0;
#if defined(_WIN32)
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc))
            junction_dir(proj_tc, root);
    }
#else
    if (g_project_root[0] &&
        join_path(proj_tc, sizeof(proj_tc), g_project_root, "toolchain")) {
        if (!pack_root_has_cmake(proj_tc) && !path_is_dir(proj_tc)) {
            char cmd[2800];
            snprintf(cmd, sizeof(cmd), "ln -s \"%s\" \"%s\"", root, proj_tc);
            (void)system(cmd);
        }
    }
#endif
    write_project_toolchain_stamp(bin);
    return 1;
}

/* True when *child* is *parent* or a path under *parent* (dir prefix). */
static int path_is_under_dir(const char* child, const char* parent) {
    char ca[1400], pa[1400];
    size_t n;
    if (!child || !child[0] || !parent || !parent[0])
        return 0;
#if defined(_WIN32)
    {
        DWORD nc = GetFullPathNameA(child, (DWORD)sizeof(ca), ca, NULL);
        DWORD np = GetFullPathNameA(parent, (DWORD)sizeof(pa), pa, NULL);
        if (nc == 0 || nc >= (DWORD)sizeof(ca) || np == 0 ||
            np >= (DWORD)sizeof(pa))
            return 0;
    }
#else
    {
        char* rc = realpath(child, NULL);
        char* rp = realpath(parent, NULL);
        if (!rc || !rp) {
            free(rc);
            free(rp);
            return 0;
        }
        snprintf(ca, sizeof(ca), "%s", rc);
        snprintf(pa, sizeof(pa), "%s", rp);
        free(rc);
        free(rp);
    }
#endif
    n = strlen(pa);
#if defined(_WIN32)
    if (_strnicmp(ca, pa, (int)n) != 0)
        return 0;
#else
    if (strncmp(ca, pa, n) != 0)
        return 0;
#endif
    if (ca[n] == '\0')
        return 1;
    return ca[n] == '/' || ca[n] == '\\';
}

/* One older install: a link is removed as a link; a folder is set aside and
 * then removed. A folder with an open file cannot be set aside on Windows: it
 * is in use and stays whole. */
static int prune_one_toolchain_child(const char* child) {
    char aside[1500];
    if (path_is_link(child))
        return remove_pointer_only(child);
    if (!set_dir_aside(child, aside, sizeof(aside))) {
        fprintf(stderr,
                "psxrecomp-codegen: older toolchain is in use by another program; "
                "kept: %s\n", child);
        return 0;
    }
    remove_dir_aside(aside);
    return 1;
}

/* After a successful install into the managed cache, drop other versioned
 * <tag>/ siblings (and *.broken quarantines). Keeps *keep_pack*, latest/, and
 * dot-directories (staging). Does not touch RETCOMM_TOOLCHAIN_DIR overrides
 * outside the preferred install root. A folder set aside (.old-*) is removed
 * only when its owner has ended and its replacement is in place. */
static void prune_old_toolchain_tags(const char* keep_pack) {
    char cache_root[1400];
    int removed = 0;
    if (!keep_pack || !keep_pack[0])
        return;
    if (!preferred_toolchain_cache_root(cache_root, sizeof(cache_root)))
        return;
    if (!path_is_dir(cache_root))
        return;
    if (!path_is_under_dir(keep_pack, cache_root) &&
        !host_paths_same_file(keep_pack, cache_root))
        return;
    if (toolchain_readonly_skip("prune older toolchain installs", cache_root))
        return;
    toolchain_put_back_set_aside();   /* a pack an interrupted install left aside is not pruned */

#if defined(_WIN32)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h;
        char pattern[1400];
        snprintf(pattern, sizeof(pattern), "%s\\*", cache_root);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            char child[1400];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (strncmp(fd.cFileName, TOOLCHAIN_ASIDE_PREFIX,
                        sizeof(TOOLCHAIN_ASIDE_PREFIX) - 1) == 0) {
                /* Only what nobody needs: its owner has ended and the folder
                 * it was taken from is there again. */
                char target[1800];
                if (aside_entry_fate(cache_root, fd.cFileName, target, sizeof(target)) == 2 &&
                    join_path(child, sizeof(child), cache_root, fd.cFileName))
                    remove_dir_aside(child);
                continue;
            }
            if (fd.cFileName[0] == '.')
                continue;
            if (_stricmp(fd.cFileName, "latest") == 0)
                continue;
            if (!join_path(child, sizeof(child), cache_root, fd.cFileName))
                continue;
            if (path_is_under_dir(keep_pack, child) ||
                host_paths_same_file(keep_pack, child) ||
                path_is_under_dir(child, keep_pack))
                continue;
            if (prune_one_toolchain_child(child))
                removed = 1;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* d = opendir(cache_root);
        struct dirent* ent;
        if (!d)
            return;
        while ((ent = readdir(d)) != NULL) {
            char child[1400];
            struct stat st;
            if (strncmp(ent->d_name, TOOLCHAIN_ASIDE_PREFIX,
                        sizeof(TOOLCHAIN_ASIDE_PREFIX) - 1) == 0) {
                char target[1800];
                if (aside_entry_fate(cache_root, ent->d_name, target, sizeof(target)) == 2 &&
                    join_path(child, sizeof(child), cache_root, ent->d_name))
                    remove_dir_aside(child);
                continue;
            }
            if (ent->d_name[0] == '.')
                continue;
            if (strcmp(ent->d_name, "latest") == 0)
                continue;
            if (!join_path(child, sizeof(child), cache_root, ent->d_name))
                continue;
            if (lstat(child, &st) != 0)
                continue;
            if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
                continue;
            if (path_is_under_dir(keep_pack, child) ||
                host_paths_same_file(keep_pack, child) ||
                path_is_under_dir(child, keep_pack))
                continue;
            if (prune_one_toolchain_child(child))
                removed = 1;
        }
        closedir(d);
    }
#endif
    (void)removed;
}

static int host_install_toolchain_from_zip(
    const char* zip_path, RecompLauncherCPrepareProgressFn on_progress,
    void* progress_ctx, char* err_msg, size_t err_cap) {
    /* Match retcomm-toolchains install.sh:
     *   …/cmake-clang-v1/<tag>/   (real pack with bin/)
     *   …/cmake-clang-v1/latest → <tag>   (pointer only)
     * Never extract the zip *into* latest/ — resolvers expect latest/bin. */
    char cache_root[1400], staging[1400], pack[1400], ver[64], tag[80];
    char dest[1400], latest[1400], aside[1500];
    aside[0] = '\0';
    if (!preferred_toolchain_cache_root(cache_root, sizeof(cache_root))) {
        snprintf(err_msg, err_cap, "Cannot resolve shared toolchain directory.");
        return 0;
    }
    if (toolchain_readonly_skip("install a toolchain pack", cache_root)) {
        snprintf(err_msg, err_cap,
                 "The toolchain is read-only for this start "
                 "(PSXRECOMP_TOOLCHAIN_READONLY). Nothing was installed.");
        return 0;
    }
    mkdir_p(cache_root);
    if (!join_path(staging, sizeof(staging), cache_root, ".staging-host")) {
        snprintf(err_msg, err_cap, "Staging path too long.");
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.45f, "Extracting portable cmake/clang…");
    if (!host_extract_zip(zip_path, staging, err_msg, err_cap))
        return 0;
    if (!unwrap_toolchain_pack_root(staging, pack, sizeof(pack))) {
        snprintf(err_msg, err_cap, "Extracted toolchain but cmake was not found.");
        rmtree_path(staging);
        return 0;
    }
    /* The new pack is checked where it was unpacked, before the installed one
     * is touched. */
    {
        char staged_bin[1500];
        g_tc_check_step = TC_STEP_CMAKE;   /* no bin folder to run cmake from */
        if (!join_path(staged_bin, sizeof(staged_bin), pack, "bin") ||
            !toolchain_bin_is_healthy(staged_bin)) {
            toolchain_text_new_pack_failed(err_msg, err_cap, g_tc_check_step);
            rmtree_path(staging);
            return 0;
        }
    }
    read_toolchain_pack_version(pack, ver, sizeof(ver));
    sanitize_toolchain_tag(ver[0] ? ver : "offline", tag, sizeof(tag));
    /* Never use "latest" as a tag directory name — reserved for the pointer. */
    if (strcmp(tag, "latest") == 0)
        snprintf(tag, sizeof(tag), "offline");
    if (!join_path(dest, sizeof(dest), cache_root, tag)) {
        snprintf(err_msg, err_cap, "Install path too long.");
        rmtree_path(staging);
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.7f, "Installing into shared toolchain cache…");
    /* The installed pack of this tag goes out of use by a rename, not by a
     * delete. A folder with an open file cannot be renamed on Windows: a build
     * is using it, so it stays whole and the new pack is dropped. */
    if (path_is_link(dest)) {
        remove_pointer_only(dest);
    } else if (path_exists_any(dest) && !set_dir_aside(dest, aside, sizeof(aside))) {
        toolchain_text_rename_failed(err_msg, err_cap, g_tc_aside_error);
        rmtree_path(staging);
        return 0;
    }
    mkdir_p(cache_root);
    /* Move usable pack to <tag>/ (flat bin/ at dest). */
    if (strcmp(pack, staging) == 0) {
#if defined(_WIN32)
        if (!MoveFileExA(staging, dest, MOVEFILE_COPY_ALLOWED |
                                            MOVEFILE_REPLACE_EXISTING)) {
            char cmd[3200];
            DWORD code = 1;
            mkdir_p(dest);
            snprintf(cmd, sizeof(cmd),
                     "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS "
                     "/nc /ns /np /MOVE",
                     staging, dest);
            (void)run_cmdline_wait(cmd, &code);
        }
#else
        if (rename(staging, dest) != 0) {
            char cmd[3200];
            snprintf(cmd, sizeof(cmd), "mv \"%s\" \"%s\"", staging, dest);
            if (system(cmd) != 0) {
                snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", staging, dest);
                (void)system(cmd);
                rmtree_path(staging);
            }
        }
#endif
    } else {
        /* Nested zip: move the child with bin/ up to dest. */
#if defined(_WIN32)
        if (!MoveFileExA(pack, dest, MOVEFILE_COPY_ALLOWED |
                                         MOVEFILE_REPLACE_EXISTING)) {
            char cmd[3200];
            DWORD code = 1;
            mkdir_p(dest);
            snprintf(cmd, sizeof(cmd),
                     "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS "
                     "/nc /ns /np /MOVE",
                     pack, dest);
            (void)run_cmdline_wait(cmd, &code);
        }
#else
        if (rename(pack, dest) != 0) {
            char cmd[3200];
            snprintf(cmd, sizeof(cmd), "mv \"%s\" \"%s\"", pack, dest);
            if (system(cmd) != 0) {
                snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", pack, dest);
                (void)system(cmd);
            }
        }
#endif
        rmtree_path(staging);
    }
    if (!unwrap_toolchain_pack_root(dest, pack, sizeof(pack))) {
        snprintf(err_msg, err_cap, "Installed toolchain missing bin/cmake.");
        /* The move did not arrive: the pack that was installed goes back. */
        if (aside[0]) {
            rmtree_path(dest);
            restore_dir_aside(aside, dest);
        }
        rmtree_path(staging);
        return 0;
    }
    toolchain_rejects_clear();   /* a new pack is at this path now */
    if (!set_toolchain_latest_pointer(cache_root, pack)) {
        /* Pointer is best-effort — pack at <tag>/ is still usable. */
    }
    if (on_progress)
        on_progress(progress_ctx, 0.85f, "Activating toolchain…");
    link_or_stamp_project_toolchain(pack);
    if (activate_installed_pack_root(pack) ||
        (join_path(latest, sizeof(latest), cache_root, "latest") &&
         activate_installed_pack_root(latest))) {
        /* Only now, with the new pack checked in its place, the old one and
         * the older installs are removed. What cannot be removed whole stays
         * under its dot-name and goes on a later pass. */
        remove_dir_aside(aside);
        if (on_progress)
            on_progress(progress_ctx, 0.92f,
                        "Removing older toolchain installs…");
        prune_old_toolchain_tags(pack);
        return 1;
    }
    /* The new pack passed its check where it was unpacked and fails it in its
     * place. The pack that was installed goes back under its name. */
    {
        int back = 1;   /* nothing was installed before, or it is back under its name */
        if (aside[0]) {
            rmtree_path(dest);
            back = restore_dir_aside(aside, dest);
            if (back && unwrap_toolchain_pack_root(dest, pack, sizeof(pack)))
                set_toolchain_latest_pointer(cache_root, pack);
        }
        /* The check's own sentence when the check named a step: the one that
         * says the installed pack is what it was, or the one that says it is
         * left under its dot-name. The older text otherwise. */
        if (g_tc_check_step[0] && back)
            toolchain_text_new_pack_failed(err_msg, err_cap, g_tc_check_step);
        else if (g_tc_check_step[0])
            toolchain_text_put_back_failed(err_msg, err_cap, g_tc_check_step);
        else
            snprintf(err_msg, err_cap,
                     "Extracted toolchain but cmake.exe is missing or will not run "
                     "(bin\\cmake.exe --version failed).");
    }
    return 0;
}

static int host_download_and_install_toolchain(
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx,
    char* err_msg, size_t err_cap) {
    char url[512], zip_path[1400], tmp_dir[1100];
    const char* asset = toolchain_zip_asset_name();
    if (toolchain_readonly_skip("download a toolchain pack", asset)) {
        snprintf(err_msg, err_cap,
                 "The toolchain is read-only for this start "
                 "(PSXRECOMP_TOOLCHAIN_READONLY). Nothing was downloaded.");
        return 0;
    }
    snprintf(url, sizeof(url),
             "https://github.com/%s/releases/latest/download/%s", k_tc_repo,
             asset);
#if defined(_WIN32)
    {
        char tmp[512];
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        if (n == 0 || n >= sizeof(tmp)) {
            snprintf(err_msg, err_cap, "GetTempPath failed.");
            return 0;
        }
        snprintf(tmp_dir, sizeof(tmp_dir), "%spsxrecomp-tc-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/psxrecomp-tc-%d", (int)getpid());
#endif
    mkdir_p(tmp_dir);
    if (!path_is_dir(tmp_dir)) {
        toolchain_text_download_failed(err_msg, err_cap,
                                       TOOLCHAIN_DOWNLOAD_NO_FOLDER);
        return 0;
    }
    if (!join_path(zip_path, sizeof(zip_path), tmp_dir, asset)) {
        snprintf(err_msg, err_cap, "Temp path too long.");
        return 0;
    }
    if (on_progress)
        on_progress(progress_ctx, 0.1f, "Downloading portable cmake/clang…");
    if (!host_download_url_to_file(url, zip_path, err_msg, err_cap)) {
        rmtree_path(tmp_dir);
        return 0;
    }
    if (!host_install_toolchain_from_zip(zip_path, on_progress, progress_ctx,
                                         err_msg, err_cap)) {
        rmtree_path(tmp_dir);
        return 0;
    }
    rmtree_path(tmp_dir);
    return 1;
}

/* Promote a usable legacy psxrecomp cache into the shared retcomm tree. */
static int migrate_legacy_psxrecomp_toolchain(void) {
    char legacy[1400], dest[1400], parent[1400];
#if defined(_WIN32)
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !local[0])
        return 0;
    if (!join_path(legacy, sizeof(legacy), local,
                   "psxrecomp/toolchains/cmake-clang-v1/latest"))
        return 0;
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0]) {
        if (!join_path(legacy, sizeof(legacy), xdg,
                       "psxrecomp/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else if (home && home[0]) {
        if (!join_path(legacy, sizeof(legacy), home,
                       ".local/share/psxrecomp/toolchains/cmake-clang-v1/latest"))
            return 0;
    } else {
        return 0;
    }
#endif
    if (!pack_root_has_cmake(legacy))
        return 0;
    if (!shared_toolchain_latest_dir(dest, sizeof(dest)))
        return 0;
    if (pack_root_has_cmake(dest))
        return 1;
    if (toolchain_readonly_skip("promote the legacy toolchain cache", dest))
        return 0;
    if (!dirname_copy(parent, sizeof(parent), dest))
        return 0;
    mkdir_p(parent);
#if defined(_WIN32)
    if (junction_dir(dest, legacy))
        return 1;
    {
        char cmd[3200];
        DWORD code = 1;
        mkdir_p(dest);
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /c robocopy \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /nc "
                 "/ns /np",
                 legacy, dest);
        if (run_cmdline_wait(cmd, &code) && code <= 7 && pack_root_has_cmake(dest))
            return 1;
    }
#else
    {
        char cmd[2800];
        snprintf(cmd, sizeof(cmd), "ln -sfn \"%s\" \"%s\"", legacy, dest);
        if (system(cmd) == 0 && pack_root_has_cmake(dest))
            return 1;
        snprintf(cmd, sizeof(cmd), "cp -a \"%s\" \"%s\"", legacy, dest);
        if (system(cmd) == 0 && pack_root_has_cmake(dest))
            return 1;
    }
#endif
    return 0;
}

static int pack_root_from_bin(const char* bin, char* out, size_t cap) {
    return bin && bin[0] && dirname_copy(out, cap, bin);
}

static int run_cmd_exit_zero(const char* cmdline) {
#if defined(_WIN32)
    DWORD code = 1;
    if (!run_cmdline_wait(cmdline, &code))
        return 0;
    return code == 0;
#else
    int st = system(cmdline);
    return st == 0;
#endif
}

/* True when cmake_path exists and ``cmake --version`` succeeds. */
static int cmake_path_runs(const char* cmake_path) {
    char cmd[1600];
    if (!cmake_path || !cmake_path[0] || !path_is_file(cmake_path))
        return 0;
#if defined(_WIN32)
    /* CreateProcess does not honor >NUL — use cmd /C. Nested quotes are the
     * documented form for a quoted exe path under cmd /C. */
    snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\" --version >NUL 2>&1\"",
             cmake_path);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" --version >/dev/null 2>&1", cmake_path);
#endif
    return run_cmd_exit_zero(cmd);
}

#if !defined(_WIN32)
/* Native release kits probe host tools; setup never installs system packages. */
static int posix_command_runs(const char* command) {
    char cmd[1600];
    if (!command || !command[0])
        return 0;
    snprintf(cmd, sizeof(cmd), "\"%s\" --version >/dev/null 2>&1", command);
    return run_cmd_exit_zero(cmd);
}

static int host_system_toolchain_ready(void) {
    char python[1200], tool[1200];
#if defined(__APPLE__)
    /* Avoid xcrun prompting to install tools during a readiness check. */
    if (!run_cmd_exit_zero("xcode-select -p >/dev/null 2>&1"))
        return 0;
#endif
    if (!find_on_path("cmake", g_cmake, sizeof(g_cmake)) ||
        !posix_command_runs(g_cmake))
        return 0;
    if (!(find_on_path("python3", python, sizeof(python)) ||
          find_on_path("python", python, sizeof(python))) ||
        !posix_command_runs(python))
        return 0;
    if (!find_on_path("ninja", tool, sizeof(tool)) || !posix_command_runs(tool))
        return 0;
    if (!(find_on_path("cc", tool, sizeof(tool)) ||
          find_on_path("gcc", tool, sizeof(tool)) ||
          find_on_path("clang", tool, sizeof(tool))) || !posix_command_runs(tool))
        return 0;
    if (!(find_on_path("c++", tool, sizeof(tool)) ||
          find_on_path("g++", tool, sizeof(tool)) ||
          find_on_path("clang++", tool, sizeof(tool))) || !posix_command_runs(tool))
        return 0;
    return 1;
}
#endif

/* True when pack bin/ can compile+link a tiny C program (catches broken
 * ld.lld / missing libicuuc.so.* etc. that cmake --version still passes). */
static int toolchain_bin_compiler_works(const char* bin) {
    char clang[1200], lld[1200], src[1400], exe[1400], cmd[4096];
    FILE* f;
    int ok;
    if (!bin || !bin[0])
        return 0;
#if defined(_WIN32)
    if (!join_path(clang, sizeof(clang), bin, "clang.exe") || !path_is_file(clang)) {
        g_tc_check_step = TC_STEP_MISSING;
        return 0;
    }
    if (!join_path(lld, sizeof(lld), bin, "ld.lld.exe") || !path_is_file(lld)) {
        /* Some Windows packs only ship lld.exe — accept either. */
        if (!join_path(lld, sizeof(lld), bin, "lld.exe") || !path_is_file(lld)) {
            g_tc_check_step = TC_STEP_MISSING;
            return 0;
        }
    }
#else
    if (!join_path(clang, sizeof(clang), bin, "clang") || !path_is_file(clang)) {
#  if defined(__APPLE__)
        /* The macOS pack (cmake-clang-v1-macos-universal) ships CMake, Ninja,
         * ccache and Python only; compiling uses the system toolchain from
         * Apple's Command Line Tools, checked by macos_system_compiler_works.
         * A pack without clang is the normal macOS layout, not a broken one. */
        return 1;
#  else
        g_tc_check_step = TC_STEP_MISSING;
        return 0;
#  endif
    }
    if (!join_path(lld, sizeof(lld), bin, "ld.lld") || !path_is_file(lld)) {
        g_tc_check_step = TC_STEP_MISSING;
        return 0;
    }
#endif
    if (!cmake_path_runs(clang)) {
        g_tc_check_step = TC_STEP_CLANG;
        return 0;
    }

#if defined(_WIN32)
    {
        char tdir[512];
        DWORD tn = GetTempPathA(sizeof(tdir), tdir);
        if (tn == 0 || tn >= sizeof(tdir)) {
            g_tc_check_step = TC_STEP_TEMP;
            return 0;
        }
        snprintf(src, sizeof(src), "%spsxrecomp-tc-probe-%lu.c", tdir,
                 (unsigned long)GetCurrentProcessId());
        snprintf(exe, sizeof(exe), "%spsxrecomp-tc-probe-%lu.exe", tdir,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    {
        /* TMPDIR when it is set, as other programs do; /tmp otherwise. */
        const char* tdir = getenv("TMPDIR");
        if (!tdir || !tdir[0])
            tdir = "/tmp";
        snprintf(src, sizeof(src), "%s/psxrecomp-tc-probe-%d.c", tdir, (int)getpid());
        snprintf(exe, sizeof(exe), "%s/psxrecomp-tc-probe-%d", tdir, (int)getpid());
    }
#endif

    f = fopen(src, "wb");
    if (!f) {
        g_tc_check_step = TC_STEP_TEMP;
        return 0;
    }
    fputs("int main(void){return 0;}\n", f);
    fclose(f);

#if defined(_WIN32)
    /* Pack bin in front of PATH so clang picks this tree's lld, not a system
     * linker. The child gets that PATH from this process's environment, for
     * the time of the check. It used to be written into the command
     * (set "PATH=<bin>;%PATH%" && ...): cmd expands %PATH% inside the line and
     * a cmd line holds 8,191 characters, so with a long PATH the line was
     * refused ("The input line is too long."), the compile "failed" and a
     * whole pack was judged unusable. */
    {
        const char* old = getenv("PATH");
        char* saved = old ? _strdup(old) : NULL;
        size_t need = strlen(bin) + 1 + (old ? strlen(old) : 0) + 1;
        char* with_bin = (char*)malloc(need);
        int in_front = 0;
        if (with_bin) {
            snprintf(with_bin, need, "%s%s%s", bin, (old && old[0]) ? ";" : "",
                     old ? old : "");
            in_front = (_putenv_s("PATH", with_bin) == 0);
        }
        if (!in_front)
            /* One variable holds 32,767 characters at most. clang still finds
             * its own linker beside itself; the check runs with PATH as it is. */
            fprintf(stderr,
                    "psxrecomp-codegen: PATH is too long to put the pack's bin "
                    "folder in front for the toolchain check; the check runs "
                    "with PATH as it is\n");
        snprintf(cmd, sizeof(cmd),
                 "cmd.exe /C \"\"%s\" \"%s\" -o \"%s\" >NUL 2>&1\"",
                 clang, src, exe);
        ok = run_cmd_exit_zero(cmd);
        if (in_front)
            _putenv_s("PATH", saved ? saved : "");
        free(with_bin);
        free(saved);
    }
#else
    /* Prefer the pack linker explicitly so PATH cannot hide a broken lld. */
    (void)lld; /* used via -fuse-ld when present; path already validated */
    snprintf(cmd, sizeof(cmd),
             "env PATH=\"%s:$PATH\" \"%s\" -fuse-ld=lld \"%s\" -o \"%s\" "
             ">/dev/null 2>&1",
             bin, clang, src, exe);
    ok = run_cmd_exit_zero(cmd);
#endif
#if defined(_WIN32)
    DeleteFileA(src);
    DeleteFileA(exe);
#else
    unlink(src);
    unlink(exe);
#endif
    if (!ok)
        g_tc_check_step = TC_STEP_LINK;
    return ok;
}

#if defined(__APPLE__)
/* True when Apple's Command Line Tools are installed and /usr/bin/clang can
 * compile and link a tiny program. xcode-select runs first so a machine
 * without the CLT gets a repair note instead of the xcrun install prompt on
 * every wizard refresh. */
static int macos_system_compiler_works(void) {
    char src[256], exe[256], cmd[1024];
    FILE* f;
    int ok;
    if (!run_cmd_exit_zero("xcode-select -p >/dev/null 2>&1"))
        return 0;
    snprintf(src, sizeof(src), "/tmp/psxrecomp-cc-probe-%d.c", (int)getpid());
    snprintf(exe, sizeof(exe), "/tmp/psxrecomp-cc-probe-%d", (int)getpid());
    f = fopen(src, "wb");
    if (!f)
        return 0;
    fputs("int main(void){return 0;}\n", f);
    fclose(f);
    snprintf(cmd, sizeof(cmd), "/usr/bin/clang \"%s\" -o \"%s\" >/dev/null 2>&1",
             src, exe);
    ok = run_cmd_exit_zero(cmd);
    unlink(src);
    unlink(exe);
    return ok;
}
#endif

/* Pack health says nothing about the macOS system compiler (see above), so
 * readiness checks it separately and explains how to fix it. The pack itself
 * is left alone: a missing compiler must never delete a good download. */
static int host_system_compiler_ready(void) {
#if defined(__APPLE__)
    if (macos_system_compiler_works())
        return 1;
    snprintf(g_tc_repair_note, sizeof(g_tc_repair_note),
             "Apple's Command Line Tools are required to build the game. "
             "Open Terminal, run: xcode-select --install  then reopen this "
             "app.");
    return 0;
#else
    return 1;
#endif
}

static int toolchain_bin_is_healthy(const char* bin) {
    char cmake[1200];
    g_tc_check_step = TC_STEP_CMAKE;   /* until cmake has run */
    if (!bin || !bin[0])
        return 0;
#if defined(_WIN32)
    if (!join_path(cmake, sizeof(cmake), bin, "cmake.exe") ||
        !cmake_path_runs(cmake))
        return 0;
#else
    if (!join_path(cmake, sizeof(cmake), bin, "cmake") || !cmake_path_runs(cmake))
        return 0;
#endif
    g_tc_check_step = "";
    return toolchain_bin_compiler_works(bin);
}

static void clear_project_toolchain_stamp(void) {
    char stamp[1200];
    if (!g_project_root[0])
        return;
    if (!join_path(stamp, sizeof(stamp), g_project_root,
                   "toolchain/.psxrecomp-bin"))
        return;
    if (path_is_file(stamp) &&
        toolchain_readonly_skip("remove the project's toolchain stamp", stamp))
        return;
#if defined(_WIN32)
    DeleteFileA(stamp);
#else
    unlink(stamp);
#endif
}

/* A check of the latest pointers before a re-download. Nothing that holds a
 * pack is removed here: one failed check does not prove that a pack is broken,
 * and a running build may be using it. A pack that fails is passed over for the
 * rest of this process and reported; the install that follows replaces it in
 * the safe order. Only a link that points at nothing is removed, and only the
 * link. A pack an interrupted install left set aside is put back first.
 * The check also fails packs where cmake runs but clang/ld.lld cannot link
 * (Linux ICU / missing libxml2). Versioned <tag>/ siblings are pruned after a
 * successful install/update (see prune_old_toolchain_tags). */
static void heal_broken_toolchain_pointers(void) {
    char bases[12][1400];
    int n = collect_toolchain_cache_bases(bases, 12);
    int failed = 0;
    const char* failed_step = "";
    toolchain_put_back_set_aside();
    for (int i = 0; i < n; ++i) {
        char latest[1400], root[1400], bin[1400];
        if (!join_path(latest, sizeof(latest), bases[i], "latest"))
            continue;
        if (!path_exists_any(latest))
            continue;
        if (!unwrap_toolchain_pack_root(latest, root, sizeof(root))) {
            /* No pack behind it. A link is removed as a link; a real folder
             * without a pack is not looked at by any lookup and stays. */
            if (path_is_link(latest) &&
                !toolchain_readonly_skip("remove a latest pointer that points at nothing",
                                         latest))
                remove_pointer_only(latest);
            continue;
        }
        if (!join_path(bin, sizeof(bin), root, "bin"))
            continue;
        if (!toolchain_bin_is_rejected(bin)) {
            if (toolchain_bin_is_healthy(bin))
                continue;
            toolchain_bin_reject(bin);
        }
        failed = 1;   /* said again at each check, as long as the pack is there */
        failed_step = toolchain_bin_rejected_step(bin);
        if (!failed_step[0])
            failed_step = g_tc_check_step;   /* the list was full: this check's own step */
    }
    if (failed) {
        clear_project_toolchain_stamp();
        g_toolchain_bin[0] = '\0';
        g_cli_toolchain_bin[0] = '\0';
        g_cmake[0] = '\0';
        toolchain_text_check_failed(g_tc_repair_note, sizeof(g_tc_repair_note), failed_step);
    }
}

/* If the active resolution points at a pack that fails its check, pass it over
 * for the rest of this process and drop the project's stamp, so that the
 * lookup can find another pack. The pack itself is not removed and not
 * renamed. */
static void discard_unhealthy_active_toolchain(void) {
    if (!g_toolchain_bin[0])
        return;
    if (toolchain_bin_is_healthy(g_toolchain_bin))
        return;
    toolchain_bin_reject(g_toolchain_bin);
    clear_project_toolchain_stamp();
    g_toolchain_bin[0] = '\0';
    g_cli_toolchain_bin[0] = '\0';
    g_cmake[0] = '\0';
    if (!g_tc_repair_note[0])
        toolchain_text_check_failed(g_tc_repair_note, sizeof(g_tc_repair_note), g_tc_check_step);
}

static int active_toolchain_meets_min(void) {
    char pack[1400];
    if (!g_toolchain_bin[0] && !resolve_toolchain_bin(g_toolchain_bin,
                                                      sizeof(g_toolchain_bin)))
        return 0;
    if (!pack_root_from_bin(g_toolchain_bin, pack, sizeof(pack)))
        return 0;
    return pack_meets_min_version(pack);
}

static int host_portable_cmake_ready(void) {
    if (!find_cmake(g_cmake, sizeof(g_cmake)))
        return 0;
    if (!cmake_path_runs(g_cmake))
        return 0;
    if (!active_toolchain_meets_min())
        return 0;
    if (!g_toolchain_bin[0] &&
        !resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return 0;
    return toolchain_bin_is_healthy(g_toolchain_bin);
}

static const char* host_toolchain_repair_note(void) {
    return g_tc_repair_note[0] ? g_tc_repair_note : NULL;
}

static int host_toolchain_is_ready(void) {
    int attempt;
    if (!g_project_root[0])
        return 0;
    g_tc_repair_note[0] = '\0';
#if !defined(_WIN32)
    return host_system_toolchain_ready();
#endif
    migrate_legacy_psxrecomp_toolchain();
    /* Wizard open: drop broken latest/ before treating the pack as ready. */
    heal_broken_toolchain_pointers();
    for (attempt = 0; attempt < 3; ++attempt) {
        activate_toolchain_path();
        if (host_portable_cmake_ready())
            return host_system_compiler_ready();
        if (!g_toolchain_bin[0])
            break;
        discard_unhealthy_active_toolchain();
    }
#if defined(_WIN32)
    /* Reuse a Store-Python LocalCache install without copying. */
    if (harvest_store_python_toolchain(0)) {
        char pack[1400];
        if (pack_root_from_bin(g_cli_toolchain_bin[0] ? g_cli_toolchain_bin
                                                     : g_toolchain_bin,
                               pack, sizeof(pack)) &&
            pack_root_has_cmake(pack) && pack_meets_min_version(pack) &&
            host_portable_cmake_ready())
            return 1;
    }
#endif
    return 0;
}

/* Fill *out with the installed pack's version string (retcomm-toolchain.json). */
static int host_local_toolchain_version(char* out, size_t cap) {
    char pack[1400];
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    activate_toolchain_path();
    if (!g_toolchain_bin[0] &&
        !resolve_toolchain_bin(g_toolchain_bin, sizeof(g_toolchain_bin)))
        return 0;
    if (!pack_root_from_bin(g_toolchain_bin, pack, sizeof(pack)))
        return 0;
    return read_pack_version(pack, out, cap);
}

/* Parse "tag_name":"…" or a …/releases/tag/<ver> URL into *out. */
static int parse_github_release_tag(const char* text, char* out, size_t cap) {
    const char* p;
    size_t i;
    if (!text || !out || cap < 2)
        return 0;
    out[0] = '\0';
    p = strstr(text, "\"tag_name\"");
    if (p) {
        p = strchr(p + 10, '"');
        if (!p)
            return 0;
        ++p;
        i = 0;
        while (*p && *p != '"' && i + 1 < cap)
            out[i++] = *p++;
        out[i] = '\0';
        return i > 0;
    }
    p = strstr(text, "/releases/tag/");
    if (!p)
        return 0;
    p += strlen("/releases/tag/");
    i = 0;
    while (*p && *p != '"' && *p != '\'' && *p != ' ' && *p != '\n' &&
           *p != '\r' && *p != '?' && *p != '#' && i + 1 < cap)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

/* Query GitHub for the latest cmake-clang-v1 release tag (short timeout). */
static int host_remote_toolchain_version(char* out, size_t cap) {
    char url[320], tmp[1400], buf[8192];
    FILE* f;
    size_t n;
    char err[256];
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/releases/latest", k_tc_repo);
#if defined(_WIN32)
    {
        char tdir[512];
        DWORD tn = GetTempPathA(sizeof(tdir), tdir);
        if (tn == 0 || tn >= sizeof(tdir))
            return 0;
        snprintf(tmp, sizeof(tmp), "%spsxrecomp-tc-latest-%lu.json", tdir,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(tmp, sizeof(tmp), "/tmp/psxrecomp-tc-latest-%d.json", (int)getpid());
#endif
    /* GitHub API wants a UA; keep the request short so wizard open stays snappy. */
#if defined(_WIN32)
    {
        char cmd[4096];
        DWORD code = 1;
        DeleteFileA(tmp);
        snprintf(cmd, sizeof(cmd),
                 "curl.exe -fsSL --connect-timeout 5 --max-time 15 "
                 "-A psxrecomp-codegen -H \"Accept: application/vnd.github+json\" "
                 "-o \"%s\" \"%s\"",
                 tmp, url);
        if (!run_cmdline_wait(cmd, &code) || code != 0 || !path_is_file(tmp)) {
            /* Fallback via cmd so stdout redirect works under CreateProcess. */
            DeleteFileA(tmp);
            snprintf(cmd, sizeof(cmd),
                     "cmd.exe /C \"curl.exe -fsSIL --connect-timeout 5 "
                     "--max-time 15 -A psxrecomp-codegen -o NUL "
                     "-w %%{url_effective} "
                     "https://github.com/%s/releases/latest > \"%s\"\"",
                     k_tc_repo, tmp);
            if (!run_cmdline_wait(cmd, &code) || code != 0 || !path_is_file(tmp))
                return 0;
        }
    }
#else
    {
        char cmd[4096];
        unlink(tmp);
        snprintf(cmd, sizeof(cmd),
                 "curl -fsSL --connect-timeout 5 --max-time 15 "
                 "-A psxrecomp-codegen -H 'Accept: application/vnd.github+json' "
                 "-o '%s' '%s'",
                 tmp, url);
        if (system(cmd) != 0 || !path_is_file(tmp)) {
            unlink(tmp);
            snprintf(cmd, sizeof(cmd),
                     "curl -fsSIL --connect-timeout 5 --max-time 15 "
                     "-A psxrecomp-codegen -o /dev/null -w '%%{url_effective}' "
                     "'https://github.com/%s/releases/latest' > '%s'",
                     k_tc_repo, tmp);
            if (system(cmd) != 0 || !path_is_file(tmp))
                return 0;
        }
    }
#endif
    (void)err;
    f = fopen(tmp, "rb");
    if (!f) {
#if defined(_WIN32)
        DeleteFileA(tmp);
#else
        unlink(tmp);
#endif
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
#if defined(_WIN32)
    DeleteFileA(tmp);
#else
    unlink(tmp);
#endif
    buf[n] = '\0';
    return parse_github_release_tag(buf, out, cap);
}

/* 1 = installed pack is older than GitHub /releases/latest (prompt to update).
 * 0 = up to date, not installed, offline, or RETCOMM_TOOLCHAIN_SKIP_UPDATE.
 * Always fills local/remote when discoverable. */
static int host_toolchain_update_available(char* local_ver, size_t local_cap,
                                           char* remote_ver, size_t remote_cap) {
    const char* skip = getenv("RETCOMM_TOOLCHAIN_SKIP_UPDATE");
    if (local_ver && local_cap)
        local_ver[0] = '\0';
    if (remote_ver && remote_cap)
        remote_ver[0] = '\0';
#if !defined(_WIN32)
    return 0;
#endif
    if (skip && skip[0] && skip[0] != '0')
        return 0;
    if (!g_project_root[0])
        return 0;
    migrate_legacy_psxrecomp_toolchain();
    (void)host_local_toolchain_version(local_ver, local_cap);
    if (!host_remote_toolchain_version(remote_ver, remote_cap))
        return 0;
    if (!local_ver || !local_ver[0])
        return 0; /* missing install → page 0 is "install", not "update" */
    if (!remote_ver || !remote_ver[0])
        return 0;
    return version_cmp(local_ver, remote_ver) < 0;
}

/* Download or offline-install cmake-clang-v1 (wizard page 0 / rebuild fallback).
 * Prefer host-native curl/tar so Microsoft Store Python cannot redirect the
 * unpack into Packages\\...\\LocalCache. Installs into the shared Retro
 * cache: %LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/…
 * Broken latest/ stamps are healed, then GitHub /releases/latest is fetched.
 *
 * download: 0 = zip/cache only, 1 = download if missing, 2 = force latest. */
static int host_ensure_toolchain_with_progress(
    int download, const char* zip_path, char* err_msg, size_t err_cap,
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx) {
    const int force = (download == 2);
    if (!g_project_root[0]) {
        snprintf(err_msg, err_cap, "Project root is not available.");
        return 0;
    }
#if !defined(_WIN32)
    if (on_progress)
        on_progress(progress_ctx, 0.02f, "Checking native build tools…");
    if (host_system_toolchain_ready())
        return 1;
    snprintf(err_msg, err_cap,
             "Native build tools are missing. Install CMake, Ninja, Python 3, "
             "and C/C++ compilers with your package manager, then check tools again.");
    return 0;
#endif
    migrate_legacy_psxrecomp_toolchain();
    activate_toolchain_path();
    if (!force && host_portable_cmake_ready())
        return 1;

#if defined(_WIN32)
    if (!force) {
        if (on_progress)
            on_progress(progress_ctx, 0.02f,
                        "Checking for an existing portable toolchain…");
        if (harvest_store_python_toolchain(1)) {
            activate_toolchain_path();
            if (host_portable_cmake_ready())
                return 1;
        }
    }
#endif

    if (on_progress)
        on_progress(progress_ctx, 0.03f,
                    force ? "Preparing toolchain update…"
                          : "Repairing broken toolchain cache if needed…");
    heal_broken_toolchain_pointers();
    clear_project_toolchain_stamp();
    g_toolchain_bin[0] = '\0';
    g_cli_toolchain_bin[0] = '\0';
    g_cmake[0] = '\0';
    activate_toolchain_path();
    if (!force && host_portable_cmake_ready())
        return 1;

    if (zip_path && zip_path[0]) {
        if (on_progress)
            on_progress(progress_ctx, 0.05f, "Installing toolchain from zip…");
        if (host_install_toolchain_from_zip(zip_path, on_progress, progress_ctx,
                                            err_msg, err_cap)) {
            if (host_portable_cmake_ready())
                return 1;
            snprintf(err_msg, err_cap,
                     "Toolchain zip installed but cmake will not run"
                     "%s. Try Unblock on the zip, or set "
                     "RETCOMM_TOOLCHAIN_DIR to a pack whose bin\\cmake.exe "
                     "passes --version.",
                     (toolchain_min_version()[0]
                          ? " (or is below RETCOMM_TOOLCHAIN_MIN_VERSION)"
                          : ""));
            return 0;
        }
        return 0;
    }

    if (download) {
        /* Fetch GitHub /releases/latest (no per-title version pin). */
        if (on_progress)
            on_progress(progress_ctx, 0.05f,
                        force ? "Downloading toolchain update…"
                              : "Downloading latest portable cmake/clang…");
        if (host_download_and_install_toolchain(on_progress, progress_ctx,
                                                err_msg, err_cap)) {
            if (host_portable_cmake_ready())
                return 1;
            /* Install saw bin/cmake.exe; ready check failed (won't run / min). */
            snprintf(err_msg, err_cap,
                     "Downloaded toolchain is present but cmake will not run"
                     "%s. Check "
                     "%%LOCALAPPDATA%%\\retcomm\\toolchains\\cmake-clang-v1\\"
                     "<tag>\\bin\\cmake.exe --version, or set "
                     "RETCOMM_TOOLCHAIN_DIR.",
                     (toolchain_min_version()[0]
                          ? " (or is below RETCOMM_TOOLCHAIN_MIN_VERSION)"
                          : ""));
            return 0;
        }
        return 0;
    }

    snprintf(err_msg, err_cap,
             "No portable toolchain found. Enable automatic download, pick a "
             "cmake-clang-v1 zip, or set RETCOMM_TOOLCHAIN_DIR.");
    return 0;
}

/* Rebuild-time fallback if the wizard step was skipped / cache pruned. */
static int host_ensure_toolchain(RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx, char* err_msg,
                                 size_t err_cap) {
    return host_ensure_toolchain_with_progress(1, NULL, err_msg, err_cap,
                                               on_progress, progress_ctx);
}

/* Record the located set in game.toml before Generate reads it.
 *
 * disc.cfg is the mounted-image cache and the runtime takes only its first
 * line; the hot-swap roster is built from game.toml [game] discs. So the
 * wizard's picks reach the roster only by being written there -- which is
 * exactly what the Retro path does by running probe_disc.py per image and
 * verify_disc_set.py over the results.
 *
 * update_disc_set.py performs the same probe/verify and then edits ONLY the
 * disc keys, leaving the project's hand-tuned sections and comments alone. It
 * is a no-op when game.toml already names these images, so a re-run of the
 * wizard does not churn the file.
 *
 * A failure here is reported and non-fatal: Generate itself only needs the
 * boot image, and refusing to build because disc 3 could not be probed would
 * be a worse outcome than building with a single-disc roster. */
static void host_update_disc_set(RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    char script[1200];
    char cmd[4096];
    const char* cut;
    size_t n;
    int i, off;

    if (g_wizard_disc_count < 2 || !g_python[0] || !g_cli_path[0] ||
        !g_game_toml[0])
        return;

    /* tools/ sits beside psxrecomp_cli.py, wherever the submodule lives. */
    cut = strrchr(g_cli_path, '/');
#if defined(_WIN32)
    {
        const char* b = strrchr(g_cli_path, '\\');
        if (b && (!cut || b > cut)) cut = b;
    }
#endif
    if (!cut) return;
    n = (size_t)(cut - g_cli_path);
    if (n >= sizeof(script)) return;
    memcpy(script, g_cli_path, n);
    script[n] = '\0';
    if ((size_t)snprintf(script + n, sizeof(script) - n,
                         "/tools/new_project_layout/update_disc_set.py") >=
        sizeof(script) - n)
        return;
    if (!path_is_file(script))
        return;   /* older SDK checkout: nothing to run, single-disc as before */

    off = snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" --game-toml \"%s\"",
                   g_python, script, g_game_toml);
    for (i = 0; i < g_wizard_disc_count; ++i) {
        if (!g_wizard_discs[i][0])
            return;   /* a gap in the middle would renumber the set */
        if (off < 0 || (size_t)off >= sizeof(cmd))
            return;
        off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " \"%s\"",
                        g_wizard_discs[i]);
    }
    if (off < 0 || (size_t)off >= sizeof(cmd))
        return;

    if (on_progress)
        on_progress(progress_ctx, 0.03f, "Recording the disc set…");
    if (!run_cmd_exit_zero(cmd) && on_progress)
        on_progress(progress_ctx, 0.03f,
                    "Could not record every disc — continuing with the boot disc.");
}

/* ---- self-build loop breaker + preflight ------------------------------- */

#define HOST_LAST_GENERATE_SIDECAR ".psxrecomp_last_generate"
/* A wizard loop iterates in minutes; a recent stamp with sources still
 * missing means "generate worked but nothing gates on its output" — a
 * config skew — not a user who wiped generated/ last week. */
#define HOST_LOOP_BREAKER_WINDOW_SECS (6 * 60 * 60)

static char g_loop_breaker_note[1024];

/* Comma-join the *_dispatch.c basenames under <root>/generated. */
static void list_generated_dispatch(char* out, size_t cap) {
    char dir[1100];
    size_t used = 0;
    out[0] = '\0';
    if (!join_path(dir, sizeof(dir), g_project_root, "generated"))
        return;
#if defined(_WIN32)
    {
        char pat[1200];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        if (!join_path(pat, sizeof(pat), dir, "*_dispatch.c"))
            return;
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            int n = snprintf(out + used, cap - used, "%s%s",
                             used ? ", " : "", fd.cFileName);
            if (n <= 0 || (size_t)n >= cap - used)
                break;
            used += (size_t)n;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        static const char suffix[] = "_dispatch.c";
        DIR* d = opendir(dir);
        struct dirent* e;
        if (!d)
            return;
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            int n;
            if (len <= sizeof(suffix) - 1 ||
                strcmp(e->d_name + len - (sizeof(suffix) - 1), suffix) != 0)
                continue;
            n = snprintf(out + used, cap - used, "%s%s",
                         used ? ", " : "", e->d_name);
            if (n <= 0 || (size_t)n >= cap - used)
                break;
            used += (size_t)n;
        }
        closedir(d);
    }
#endif
}

static void host_record_generate_ok(void) {
    char sidecar[1200], line[32];
    if (!join_path(sidecar, sizeof(sidecar), g_project_root,
                   HOST_LAST_GENERATE_SIDECAR))
        return;
    snprintf(line, sizeof(line), "%lld", (long long)time(NULL));
    write_line_file(sidecar, line);
}

static void host_clear_generate_stamp(void) {
    char sidecar[1200];
    if (join_path(sidecar, sizeof(sidecar), g_project_root,
                  HOST_LAST_GENERATE_SIDECAR))
        remove(sidecar);
}

/* apply() consults this when the wizard is about to reopen because sources
 * are missing. If a generate succeeded here recently, re-running it will
 * loop — say exactly what is missing and what exists instead of silently
 * re-prompting. Returns NULL when there is nothing to warn about. */
static const char* host_loop_breaker_note(void) {
    char sidecar[1200], line[64], found[512], marker_abs[1200];
    long long then, now;
    const char* marker_rel;
    const char* cause;
    int game_missing, bios_missing;
    if (!join_path(sidecar, sizeof(sidecar), g_project_root,
                   HOST_LAST_GENERATE_SIDECAR))
        return NULL;
    if (!read_line_file(sidecar, line, sizeof(line)))
        return NULL;
    then = strtoll(line, NULL, 10);
    now = (long long)time(NULL);
    if (then <= 0 || now < then || now - then > HOST_LOOP_BREAKER_WINDOW_SECS)
        return NULL;
    marker_rel = cfg_or(g_cfg->gen_marker_relpath,
                        "generated/SLUS_011.89_dispatch.c");
    game_missing =
        !join_path(marker_abs, sizeof(marker_abs), g_project_root,
                   marker_rel) ||
        !path_is_file(marker_abs);
    bios_missing = bios_backends_missing();
    if (!game_missing && !bios_missing)
        return NULL;
    list_generated_dispatch(found, sizeof(found));
    /* Each branch has its own cause, so do not assert a single one. A missing
     * game dispatch really does point at disagreeing boot-EXE names. Missing
     * BIOS backends do not: they mean Generate never emitted them, normally
     * because no retail BIOS was available to emit them from. Blaming
     * boot-EXE names for that sent players after the wrong thing. */
    if (game_missing)
        cause = "please report this to the port maintainer: the project's "
                "boot-EXE names disagree";
    else
        cause = "Generate produced the game code but no BIOS backend, which "
                "normally means it had no retail BIOS to work from. Select "
                "the PlayStation BIOS dump this port requires (named in the "
                "README; it must be exactly 512 KB) in the launcher, then "
                "run Generate again";
    snprintf(g_loop_breaker_note, sizeof(g_loop_breaker_note),
             "A Generate completed here recently, yet the launcher still "
             "cannot find %s%s%s. generated/ contains: %s. Running Generate "
             "again will very likely loop — %s.",
             game_missing ? marker_rel : "",
             (game_missing && bios_missing) ? " and " : "",
             bios_missing ? "the BIOS backends under psxrecomp/generated/"
                          : "",
             found[0] ? found : "no *_dispatch.c at all",
             cause);
    fprintf(stderr, "psxrecomp-codegen: %s\n", g_loop_breaker_note);
    return g_loop_breaker_note;
}

/* Generating into Explorer's temporary zip extraction (Windows) or an
 * unwritable folder always ends badly — the output evaporates or never
 * lands, and the wizard reopens forever. Refuse with instructions. */
static int host_preflight_project_root(char* err_msg, size_t err_cap) {
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
        if (n > 0 && n < (DWORD)sizeof(tmp) &&
            _strnicmp(g_project_root, tmp, strlen(tmp)) == 0) {
            snprintf(err_msg, err_cap,
                     "This game is running from a temporary folder (%s), "
                     "usually because the downloaded ZIP was opened without "
                     "extracting it. Extract the whole ZIP to a normal "
                     "folder, then run the game from there.",
                     g_project_root);
            return 0;
        }
    }
#endif
    {
        char probe[1200];
        FILE* f;
        if (!join_path(probe, sizeof(probe), g_project_root,
                       ".psxrecomp_write_probe"))
            return 1; /* path too long — let the CLI surface the real error */
        f = fopen(probe, "wb");
        if (!f) {
            snprintf(err_msg, err_cap,
                     "The install folder is not writable: %s. Move the game "
                     "folder somewhere writable (e.g. your user folder) or "
                     "fix its permissions, then retry.",
                     g_project_root);
            return 0;
        }
        fclose(f);
        remove(probe);
    }
    return 1;
}

static int host_prepare_generate(const char* source_path, char* out_path,
                                 size_t out_cap, char* err_msg, size_t err_cap,
                                 RecompLauncherCPrepareProgressFn on_progress,
                                 void* progress_ctx) {
    if (!g_ready) {
        snprintf(err_msg, err_cap, "Local codegen tools are not available.");
        return 0;
    }
    if (!source_path || !source_path[0]) {
        snprintf(err_msg, err_cap, "No disc selected.");
        return 0;
    }
    if (!host_preflight_project_root(err_msg, err_cap))
        return 0;
    activate_toolchain_path();
    if (!find_python(g_python, sizeof(g_python))) {
        /* First-run / pruned cache: page 0 normally installed the pack; heal. */
        if (!host_ensure_toolchain(on_progress, progress_ctx, err_msg, err_cap))
            return 0;
        activate_toolchain_path();
        if (!find_python(g_python, sizeof(g_python))) {
            snprintf(err_msg, err_cap,
                     "No usable Python. Install the portable toolchain step "
                     "(includes CPython under python/), or set RETCOMM_PYTHON.");
            return 0;
        }
    }
    if (on_progress)
        on_progress(progress_ctx, 0.02f, "Starting psxrecomp generate…");

    /* Must precede the CLI: generate reads game.toml. */
    host_update_disc_set(on_progress, progress_ctx);

    /* Hand the CLI the launcher's staged disc + BIOS. Empty g_wizard_bios means
     * OpenBIOS unless setup can adopt a retail dump beside the install. */
    {
        char abs_bios[1100];
        if (g_wizard_bios[0] &&
            absolutize_existing_file(abs_bios, sizeof(abs_bios), g_wizard_bios))
            snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", abs_bios);
        else if (!g_wizard_bios[0]) {
            /* Intentional OpenBIOS clear stays empty; otherwise adopt SCPH1001
             * from bios.cfg / discovery (resolve_bios_arg). */
            if (!g_wizard_bios_explicit) {
                char found[1100];
                if (resolve_bios_arg(found, sizeof(found)))
                    snprintf(g_wizard_bios, sizeof(g_wizard_bios), "%s", found);
            }
        } else {
            /* Staged path missing — fail clearly instead of silently OpenBIOS. */
            snprintf(err_msg, err_cap, "Staged BIOS not found: %s", g_wizard_bios);
            return 0;
        }
        host_persist_setup(NULL, source_path,
                           g_wizard_bios[0] ? g_wizard_bios : "");
    }

    char bios_path[1100];
    bios_path[0] = '\0';
    const int have_bios =
        g_wizard_bios[0] &&
        absolutize_existing_file(bios_path, sizeof(bios_path), g_wizard_bios);
    fprintf(stderr, "psxrecomp-codegen: generate disc=%s bios=%s\n", source_path,
            have_bios ? bios_path : "(OpenBIOS)");

    /* The exact dispatch filename this host (and CMakeLists GEN_MARKER) will
     * gate on later. Handing it to generate makes a boot-exe name skew fail
     * there, loudly, instead of surfacing as an endless setup-wizard loop. */
    const char* marker_rel =
        cfg_or(g_cfg->gen_marker_relpath, "generated/SLUS_011.89_dispatch.c");
    const char* marker_name = marker_rel;
    for (const char* p = marker_rel; *p; ++p)
        if (*p == '/' || *p == '\\')
            marker_name = p + 1;

#if defined(_WIN32)
    char cmdline[4096];
    if (have_bios) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --config \"%s\" "
                 "--disc \"%s\" --bios \"%s\" --gen-marker \"%s\" "
                 "--json-progress",
                 g_python, g_cli_path, g_project_root, g_game_toml, source_path,
                 bios_path, marker_name);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s\" \"%s\" generate --project-root \"%s\" --config \"%s\" "
                 "--disc \"%s\" --gen-marker \"%s\" --json-progress",
                 g_python, g_cli_path, g_project_root, g_game_toml, source_path,
                 marker_name);
    }
    if (!run_cli_win(cmdline, on_progress, progress_ctx, err_msg, err_cap,
                     "psxrecomp generate"))
        return 0;
#else
    char* argv[16];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "generate";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--config";
    argv[argc++] = g_game_toml;
    argv[argc++] = "--disc";
    argv[argc++] = (char*)source_path;
    if (have_bios) {
        argv[argc++] = "--bios";
        argv[argc++] = bios_path;
    }
    argv[argc++] = "--gen-marker";
    argv[argc++] = (char*)marker_name;
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;
    if (!run_cli_posix(argv, on_progress, progress_ctx, err_msg, err_cap,
                       "psxrecomp generate"))
        return 0;
#endif

    /* Stamp the success so apply() can tell "generate worked but the
     * launcher still cannot see its output" apart from a plain first run. */
    host_record_generate_ok();

    snprintf(out_path, out_cap, "%s", source_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f, "Generate complete");
    return 1;
}

#if defined(_WIN32)
static void bat_write_set(FILE* f, const char* name, const char* value) {
    fprintf(f, "set \"%s=", name);
    for (const char* p = value; *p; ++p) {
        if (*p == '%')
            fputc('%', f);
        fputc(*p, f);
    }
    fprintf(f, "\"\r\n");
}

/* force_pgo: Settings Optimize FMV (instrument → train → use). Else setup
 * rebuild with --no-pgo. disc_path required when force_pgo. */
static int write_windows_deferred_rebuild_helper(int force_pgo,
                                                 const char* disc_path,
                                                 char* err_msg,
                                                 size_t err_cap) {
    if (!join_path(g_helper_path, sizeof(g_helper_path), g_build_dir,
                   "recomp_deferred_rebuild.cmd")) {
        snprintf(err_msg, err_cap, "Failed to form helper path.");
        return 0;
    }
    /* Setup zips omit build-release/; create it before writing the .cmd. */
    if (!mkdir_p(g_build_dir)) {
        snprintf(err_msg, err_cap, "Failed to create build dir: %s",
                 g_build_dir);
        return 0;
    }
    FILE* f = fopen(g_helper_path, "wb");
    if (!f) {
        snprintf(err_msg, err_cap, "Failed to write rebuild helper: %s",
                 g_helper_path);
        return 0;
    }
    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "%lu",
             (unsigned long)GetCurrentProcessId());
    fprintf(f, "@echo off\r\n");
    fprintf(f, "setlocal EnableExtensions\r\n");
    fprintf(f, "title %s - %s\r\n", g_display,
            force_pgo ? "PGO optimize" : "rebuilding");
    bat_write_set(f, "PARENT_PID", pid_buf);
    bat_write_set(f, "PYTHON", g_python);
    bat_write_set(f, "CLI", g_cli_path);
    bat_write_set(f, "ROOT", g_project_root);
    bat_write_set(f, "CONFIG", g_game_toml);
    bat_write_set(f, "BUILD_DIR", g_build_dir);
    bat_write_set(f, "TARGET", g_cmake_target);
    bat_write_set(f, "EXE_BASE", g_exe_basename);
    bat_write_set(f, "EXE", g_exe_path);
    bat_write_set(f, "DISPLAY", g_display);
    {
        /* Post-build sanity: the dispatch file the launcher will gate on. */
        char marker_abs[1200];
        if (join_path(marker_abs, sizeof(marker_abs), g_project_root,
                      cfg_or(g_cfg->gen_marker_relpath,
                             "generated/SLUS_011.89_dispatch.c")))
            bat_write_set(f, "GEN_MARKER", marker_abs);
    }
    if (force_pgo && disc_path && disc_path[0])
        bat_write_set(f, "DISC", disc_path);
    if (g_toolchain_bin[0])
        bat_write_set(f, "TC_BIN", g_toolchain_bin);
    fprintf(f,
            "echo Waiting for %%DISPLAY%% to exit...\r\n"
            ":waitloop\r\n"
            "tasklist /FI \"PID eq %%PARENT_PID%%\" 2>NUL | "
            "findstr /I \"%%PARENT_PID%%\" >NUL\r\n"
            "if not errorlevel 1 (\r\n"
            "  ping -n 2 127.0.0.1 >NUL\r\n"
            "  goto waitloop\r\n"
            ")\r\n"
            "echo Ensuring toolchain...\r\n"
            "cd /d \"%%ROOT%%\"\r\n"
            "if defined TC_BIN set \"PATH=%%TC_BIN%%;%%PATH%%\"\r\n"
            "\"%%PYTHON%%\" \"%%CLI%%\" ensure-toolchain --project-root \"%%ROOT%%\"\r\n"
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Toolchain missing. Download cmake-clang-v1 or set\r\n"
            "  echo RETCOMM_TOOLCHAIN_DIR / pass --toolchain-zip on rebuild.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n");
    if (force_pgo) {
        fprintf(f,
                "echo PGO optimize (instrument + train + rebuild)...\r\n"
                "\"%%PYTHON%%\" \"%%CLI%%\" rebuild --project-root \"%%ROOT%%\" "
                "--config \"%%CONFIG%%\" --build-dir \"%%BUILD_DIR%%\" "
                "--target \"%%TARGET%%\" --exe-basename \"%%EXE_BASE%%\" "
                "--disc \"%%DISC%%\" --force-pgo --pgo-video\r\n");
    } else {
        fprintf(f,
                "echo Building...\r\n"
                "\"%%PYTHON%%\" \"%%CLI%%\" rebuild --project-root \"%%ROOT%%\" "
                "--config \"%%CONFIG%%\" --build-dir \"%%BUILD_DIR%%\" "
                "--target \"%%TARGET%%\" --exe-basename \"%%EXE_BASE%%\" "
                "--no-pgo --prune-after build-intermediates\r\n");
    }
    fprintf(f,
            "if errorlevel 1 (\r\n"
            "  echo.\r\n"
            "  echo Build failed. Fix the errors above, then rebuild manually.\r\n"
            "  pause\r\n"
            "  exit /b 1\r\n"
            ")\r\n"
            /* %EXE% was guessed before the build; runtime.cmake publishes the
             * OUTPUT_NAME it really used, which wins the moment a title is
             * renamed. Then verify game code + exe exist before relaunching,
             * so a setup-host build stops here with a message instead of
             * reopening the wizard in a loop. */
            "set \"EXE_FINAL=%%EXE%%\"\r\n"
            "set \"PUBLISHED=\"\r\n"
            "if exist \"%%BUILD_DIR%%\\psxrecomp_exe_name-%%TARGET%%.txt\" "
            "set /p PUBLISHED=<\"%%BUILD_DIR%%\\psxrecomp_exe_name-%%TARGET%%.txt\"\r\n"
            "if defined PUBLISHED if exist \"%%BUILD_DIR%%\\%%PUBLISHED%%.exe\" "
            "set \"EXE_FINAL=%%BUILD_DIR%%\\%%PUBLISHED%%.exe\"\r\n"
            /* Path-bearing messages sit outside ( ) blocks: cmd expands
             * %VAR% when it parses a block, so a ")" in an install folder
             * such as "... (1)" would close the block early. */
            "if defined GEN_MARKER if not exist \"%%GEN_MARKER%%\" goto no_gen\r\n"
            "if not exist \"%%EXE_FINAL%%\" goto no_exe\r\n"
            "echo Starting %%DISPLAY%%...\r\n"
            "start \"\" /D \"%%ROOT%%\" \"%%EXE_FINAL%%\" --launcher\r\n"
            "endlocal\r\n"
            "exit /b 0\r\n"
            ":no_gen\r\n"
            "echo.\r\n"
            "echo Build finished but the generated game code is missing:\r\n"
            "echo   %%GEN_MARKER%%\r\n"
            "echo Launching now would reopen setup in a loop. Please report\r\n"
            "echo this to the port maintainer: the boot-EXE name in\r\n"
            "echo game.toml disagrees with GEN_MARKER in CMakeLists.txt.\r\n"
            "pause\r\n"
            "exit /b 1\r\n"
            ":no_exe\r\n"
            "echo.\r\n"
            "echo Build finished but the game executable is missing:\r\n"
            "echo   %%EXE_FINAL%%\r\n"
            "pause\r\n"
            "exit /b 1\r\n");
    fclose(f);
    return 1;
}
#endif

static int host_rebuild_game_ex(const char* disc_path, int force_pgo,
                                char* out_exe_path, size_t out_cap,
                                char* err_msg, size_t err_cap,
                                RecompLauncherCPrepareProgressFn on_progress,
                                void* progress_ctx) {
    g_relaunch_is_helper = 0;
    if (!g_ready || !g_build_dir[0]) {
        snprintf(err_msg, err_cap, "CMake build environment is not available.");
        return 0;
    }
    if (force_pgo) {
        if (psxrecomp_codegen_host_sources_missing(g_cfg)) {
            snprintf(err_msg, err_cap,
                     "Generated game C is missing — finish Generate & rebuild "
                     "in the setup wizard first.");
            return 0;
        }
        if (!disc_path || !disc_path[0] || !path_is_file(disc_path)) {
            snprintf(err_msg, err_cap,
                     "Select a playable disc image before optimizing FMV.");
            return 0;
        }
    }

    if (!host_ensure_toolchain(on_progress, progress_ctx, err_msg, err_cap))
        return 0;

    activate_toolchain_path();
    if (!find_python(g_python, sizeof(g_python))) {
        snprintf(err_msg, err_cap,
                 "No usable Python. Install the portable toolchain step "
                 "(includes CPython under python/), or set RETCOMM_PYTHON.");
        return 0;
    }

#if defined(_WIN32)
    if (on_progress)
        on_progress(progress_ctx, 0.4f,
                    force_pgo ? "Scheduling Windows PGO optimize after exit…"
                              : "Scheduling Windows rebuild after exit…");
    if (!write_windows_deferred_rebuild_helper(force_pgo, disc_path, err_msg,
                                               err_cap))
        return 0;
    g_relaunch_is_helper = 1;
    snprintf(out_exe_path, out_cap, "%s", g_helper_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f,
                    force_pgo
                        ? "Exiting so Windows can run PGO train + rebuild…"
                        : "Exiting so Windows can rebuild safely…");
    return 1;
#else
    if (on_progress)
        on_progress(progress_ctx, 0.05f,
                    force_pgo ? "Starting FMV PGO optimize…"
                              : "Starting rebuild (cmake)…");

    char disc_arg_storage[1100];
    char* argv[40];
    int argc = 0;
    argv[argc++] = g_python;
    argv[argc++] = g_cli_path;
    argv[argc++] = "rebuild";
    argv[argc++] = "--project-root";
    argv[argc++] = g_project_root;
    argv[argc++] = "--config";
    argv[argc++] = g_game_toml;
    argv[argc++] = "--build-dir";
    argv[argc++] = g_build_dir;
    argv[argc++] = "--target";
    argv[argc++] = g_cmake_target;
    argv[argc++] = "--exe-basename";
    argv[argc++] = g_exe_basename;
    if (disc_path && disc_path[0]) {
        snprintf(disc_arg_storage, sizeof(disc_arg_storage), "%s", disc_path);
        argv[argc++] = "--disc";
        argv[argc++] = disc_arg_storage;
    }
    if (force_pgo) {
        argv[argc++] = "--force-pgo";
        argv[argc++] = "--pgo-video";
    } else {
        argv[argc++] = "--no-pgo";
        argv[argc++] = "--prune-after";
        argv[argc++] = "build-intermediates";
    }
    argv[argc++] = "--json-progress";
    argv[argc] = NULL;

    if (!run_cli_posix(argv, on_progress, progress_ctx, err_msg, err_cap,
                       force_pgo ? "psxrecomp PGO optimize"
                                 : "psxrecomp rebuild"))
        return 0;
    if (!path_is_file(g_exe_path)) {
        snprintf(err_msg, err_cap, "Build succeeded but binary missing: %s",
                 g_exe_path);
        return 0;
    }
    snprintf(out_exe_path, out_cap, "%s", g_exe_path);
    if (on_progress)
        on_progress(progress_ctx, 1.0f,
                    force_pgo ? "FMV optimize complete" : "Build complete");
    return 1;
#endif
}

static int host_rebuild_game(const char* disc_path, char* out_exe_path,
                             size_t out_cap, char* err_msg, size_t err_cap,
                             RecompLauncherCPrepareProgressFn on_progress,
                             void* progress_ctx) {
    return host_rebuild_game_ex(disc_path, 0, out_exe_path, out_cap, err_msg,
                                err_cap, on_progress, progress_ctx);
}

static int host_pgo_optimize(const char* disc_path, char* out_exe_path,
                             size_t out_cap, char* err_msg, size_t err_cap,
                             RecompLauncherCPrepareProgressFn on_progress,
                             void* progress_ctx) {
    return host_rebuild_game_ex(disc_path, 1, out_exe_path, out_cap, err_msg,
                                err_cap, on_progress, progress_ctx);
}

/* Settings → VIDEO → Apply FMV Timing Opt: regenerate C (picks up
 * load_charge_batch) then rebuild without PGO training. */
static int host_fmv_timing_optimize(const char* disc_path, char* out_exe_path,
                                    size_t out_cap, char* err_msg,
                                    size_t err_cap,
                                    RecompLauncherCPrepareProgressFn on_progress,
                                    void* progress_ctx) {
    char gen_out[512];
    if (!host_prepare_generate(disc_path, gen_out, sizeof(gen_out), err_msg,
                               err_cap, on_progress, progress_ctx))
        return 0;
    return host_rebuild_game_ex(disc_path, 0, out_exe_path, out_cap, err_msg,
                                err_cap, on_progress, progress_ctx);
}

static int host_self_exe_path(char* out, size_t cap) {
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
        return n > 0 && n < (DWORD)cap;
    }
#else
    {
        const char* appimg = getenv("APPIMAGE");
        if (appimg && appimg[0] && path_is_file(appimg)) {
            snprintf(out, cap, "%s", appimg);
            return 1;
        }
        return host_posix_exe_path(out, cap);
    }
#endif
}

static int host_paths_same_file(const char* a, const char* b) {
    char fa[1100], fb[1100];
    if (!a || !a[0] || !b || !b[0])
        return 0;
#if defined(_WIN32)
    {
        DWORD na = GetFullPathNameA(a, (DWORD)sizeof(fa), fa, NULL);
        DWORD nb = GetFullPathNameA(b, (DWORD)sizeof(fb), fb, NULL);
        if (na == 0 || na >= (DWORD)sizeof(fa) || nb == 0 ||
            nb >= (DWORD)sizeof(fb))
            return 0;
        return _stricmp(fa, fb) == 0;
    }
#else
    {
        char* ra = realpath(a, NULL);
        char* rb = realpath(b, NULL);
        int same = ra && rb && strcmp(ra, rb) == 0;
        free(ra);
        free(rb);
        return same;
    }
#endif
}

/* --setup-selfcheck: report what the setup host believes about this tree, as
 * JSON on stdout, then exit. 0 = generated sources complete, 2 = the wizard
 * would reopen, 1 = could not tell (no project root).
 *
 * This exists because the decision layer -- "are the generated sources
 * present?" -- had no headless entry point. Disc selection, BIOS selection and
 * generation were already scriptable via psxrecomp_cli.py; the verdict on
 * whether setup is DONE was reachable only by clicking through the wizard, so
 * nothing in CI could assert it. A stem mismatch there shipped a first-run
 * loop on 26 titles before anyone noticed.
 *
 * Deliberately ahead of the PSX_HAS_GAME_DISPATCH early return below, so a
 * product build answers too. Every title already calls this function with
 * argc/argv, so no per-title change is needed to gain the flag. */
static void host_json_str(const char* s) {
    putchar('"');
    for (; s && *s; ++s) {
        if (*s == '\\' || *s == '"')
            putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

static void host_selfcheck_or_return(const PsxrecompCodegenHostConfig* cfg,
                                     int argc, char** argv) {
    const PsxKnownBiosImage* want;
    const char* marker_rel;
    char marker_abs[1200];
    int i, missing, game_ok, bios_ok;

    for (i = 1; i < argc; ++i)
        if (argv[i] && strcmp(argv[i], "--setup-selfcheck") == 0)
            break;
    if (i >= argc)
        return;

    if (!cfg || !cfg->cmake_target || !cfg->exe_basename) {
        printf("{\"error\": \"no codegen host config linked\"}\n");
        exit(1);
    }
    /* Sets g_cfg and g_project_root as a side effect. */
    missing = psxrecomp_codegen_host_sources_missing(cfg);
    if (!g_project_root[0]) {
        printf("{\"error\": \"project root not found\"}\n");
        exit(1);
    }

    marker_rel = cfg_or(cfg->gen_marker_relpath,
                        "generated/SLUS_011.89_dispatch.c");
    game_ok = join_path(marker_abs, sizeof(marker_abs), g_project_root,
                        marker_rel) && path_is_file(marker_abs);
    bios_ok = !bios_backends_missing();
    want = psx_expected_bios();

    printf("{\n");
    printf("  \"display_name\": ");
    host_json_str(cfg_or(cfg->display_name, "Game"));
    printf(",\n  \"project_root\": ");
    host_json_str(g_project_root);
    printf(",\n  \"expected_bios_stem\": ");
    host_json_str(PSX_EXPECTED_BIOS_STEM);
    printf(",\n  \"expected_bios_id\": ");
    host_json_str(want ? want->id : "");
    printf(",\n  \"expected_bios_crc32\": ");
    if (want) {
        char crcbuf[16];
        snprintf(crcbuf, sizeof(crcbuf), "0x%08X", want->crc32);
        host_json_str(crcbuf);
    } else {
        printf("null");
    }
    printf(",\n  \"game_dispatch\": ");
    host_json_str(marker_rel);
    printf(",\n  \"game_dispatch_present\": %s", game_ok ? "true" : "false");
    printf(",\n  \"bios_backends_present\": %s", bios_ok ? "true" : "false");
    printf(",\n  \"sources_missing\": %s", missing ? "true" : "false");
    /* Wave-5 F4: overlay_cache is the switch that makes the runtime initialise the
     * overlay loader at all; without it every streamed overlay is interpreted. CI
     * asserts this field so a title cannot ship silently without it again. */
    {
        char toml_abs[1200];
        int overlay_cache = 0;
        if (join_path(toml_abs, sizeof(toml_abs), g_project_root, "game.toml")) {
            FILE* tf = fopen(toml_abs, "rb");
            if (tf) {
                char line[512];
                while (fgets(line, sizeof(line), tf)) {
                    const char* p = line;
                    while (*p == ' ' || *p == '\t') ++p;
                    if (strncmp(p, "overlay_cache", 13) == 0) {
                        p += 13;
                        while (*p == ' ' || *p == '\t') ++p;
                        if (*p == '=') {
                            ++p;
                            while (*p == ' ' || *p == '\t') ++p;
                            overlay_cache = strncmp(p, "true", 4) == 0;
                        }
                    }
                }
                fclose(tf);
            }
        }
        printf(",\n  \"overlay_cache_configured\": %s", overlay_cache ? "true" : "false");
    }
    /* Build-tools readiness as wizard page 0 judges it, minus its cache
     * healing: nothing is deleted or renamed here. toolchain_note carries the
     * repair hint the wizard would show (e.g. missing Command Line Tools). */
    {
        int tc_ready;
        g_tc_repair_note[0] = '\0';
        activate_toolchain_path();
        tc_ready = host_portable_cmake_ready() && host_system_compiler_ready();
        printf(",\n  \"toolchain_ready\": %s", tc_ready ? "true" : "false");
        printf(",\n  \"toolchain_note\": ");
        host_json_str(g_tc_repair_note);
    }
    printf("\n}\n");
    fflush(stdout);
    exit(missing ? 2 : 0);
}

#if !defined(PSX_HAS_GAME_DISPATCH)
/* Does the hand-over to the product add --launcher?
 *
 * It does for a player's start: the product's persisted skip_launcher setting
 * must not hide the launcher on a hand-over. It does not when the caller asked
 * for a start without the launcher, by one of the signals the product honours
 * itself, read by the product's own rules (runtime/src/main.cpp):
 *   --no-launcher, --headless, --headless-opengl, --hidden-window;
 *   PSX_NO_LAUNCHER present, whatever its value;
 *   PSX_HEADLESS set to something other than empty or a value that starts
 *   with 0.
 * In the product --launcher wins over all of them, so adding it made every
 * scripted headless start of the setup exe wait in the launcher and show no
 * frame. A --launcher the caller passed is forwarded as it is and not
 * doubled. */
static int handover_adds_launcher(int argc, char** argv) {
    static const char* const keep_args_only[] = {
        "--launcher", "--no-launcher", "--headless", "--headless-opengl",
        "--hidden-window",
    };
    const char* headless = getenv("PSX_HEADLESS");
    int i;
    size_t k;
    for (i = 1; i < argc && argv && argv[i]; ++i) {
        for (k = 0; k < sizeof(keep_args_only) / sizeof(keep_args_only[0]); ++k) {
            if (strcmp(argv[i], keep_args_only[k]) == 0)
                return 0;
        }
    }
    if (getenv("PSX_NO_LAUNCHER"))
        return 0;
    if (headless && headless[0] && headless[0] != '0')
        return 0;
    return 1;
}
#endif

/* Setup-host zip-root exe → build-release product (bios/mods/assets/settings). */
void psxrecomp_codegen_host_forward_if_built(
    const PsxrecompCodegenHostConfig* cfg, int argc, char** argv) {
    host_selfcheck_or_return(cfg, argc, argv); /* exits when requested */
#if defined(PSX_HAS_GAME_DISPATCH)
    /* Full game binary — already the product tree. */
    (void)cfg;
    (void)argc;
    (void)argv;
    return;
#else
    char self[1100];
    const char* no_fwd;
    const char* force_env;
    const char* force;

    if (!cfg || !cfg->cmake_target || !cfg->exe_basename)
        return;

    no_fwd = getenv("PSXRECOMP_NO_FORWARD");
    if (no_fwd && no_fwd[0] && no_fwd[0] != '0')
        return;

    g_cfg = cfg;
    snprintf(g_display, sizeof(g_display), "%s",
             cfg_or(cfg->display_name, "Game"));
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%s", cfg->cmake_target);
    snprintf(g_exe_basename, sizeof(g_exe_basename), "%s", cfg->exe_basename);

    force_env = cfg_or(cfg->force_setup_env, "PSXRECOMP_FORCE_SETUP");
    force = getenv(force_env);
    if (force && force[0] && force[0] != '0')
        return;

    /* Still need Generate — stay on the setup host. */
    if (psxrecomp_codegen_host_sources_missing(cfg))
        return;

    if (!discover_project_root(g_project_root, sizeof(g_project_root)))
        return;
    if (!resolve_build_paths())
        return;
    if (!g_exe_path[0] || !path_is_file(g_exe_path))
        return;
    if (!host_self_exe_path(self, sizeof(self)))
        return;
    if (host_paths_same_file(self, g_exe_path))
        return;

    fprintf(stderr,
            "psxrecomp-codegen: setup host forwarding to product build:\n  %s\n",
            g_exe_path);
    sync_project_disc_cfg_to_product(g_exe_path);
    sync_project_settings_to_product(g_exe_path);

#if defined(_WIN32)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[4096];
        size_t pos = 0;
        int i;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, "\"%s\"",
                                g_exe_path);
        /* Preserve caller argv (skip argv[0]); ensure --launcher so skip_launcher
         * in the product tree cannot hide the UI on first handoff, unless the
         * caller asked for a start without it (handover_adds_launcher). */
        {
            for (i = 1; i < argc && argv && argv[i]; ++i) {
                int n;
                n = snprintf(cmd + pos, sizeof(cmd) - pos, " \"%s\"", argv[i]);
                if (n <= 0 || (size_t)n >= sizeof(cmd) - pos)
                    break;
                pos += (size_t)n;
            }
            if (handover_adds_launcher(argc, argv) && pos + 12 < sizeof(cmd))
                snprintf(cmd + pos, sizeof(cmd) - pos, " --launcher");
        }
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL,
                            g_project_root[0] ? g_project_root : NULL, &si,
                            &pi)) {
            fprintf(stderr,
                    "psxrecomp-codegen: failed to start product exe (error %lu)\n",
                    (unsigned long)GetLastError());
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    {
        char** args;
        int i;
        int narg = (argc > 0) ? argc : 1;
        args = (char**)calloc((size_t)narg + 2, sizeof(char*));
        if (!args)
            return;
        args[0] = g_exe_path;
        for (i = 1; i < argc && argv && argv[i]; ++i)
            args[i] = argv[i];
        if (handover_adds_launcher(argc, argv))
            args[i++] = "--launcher";
        args[i] = NULL;
        if (g_project_root[0] && chdir(g_project_root) != 0) {
            fprintf(stderr, "psxrecomp-codegen: chdir(%s) failed: %s\n",
                    g_project_root, strerror(errno));
        }
        execv(g_exe_path, args);
        perror("psxrecomp-codegen: execv product exe failed");
        free(args);
    }
#endif
#endif /* !PSX_HAS_GAME_DISPATCH */
}

void psxrecomp_codegen_host_relaunch_or_exit(const char* disc_path) {
    char exe[512];
    const char* near_exe;
    if (!recomp_launcher_relaunch_exe(exe, sizeof(exe)) || !exe[0]) {
        fprintf(stderr, "psxrecomp-codegen: relaunch requested but no path\n");
        exit(1);
    }
    /* Prefer the final game binary (build/<exe>) over a Windows helper bat. */
    near_exe = g_exe_path[0] ? g_exe_path : exe;
    persist_relaunch_sidecars(near_exe, disc_path);

#if defined(_WIN32)
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[1536];
        DWORD flags = 0;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        if (g_relaunch_is_helper) {
            fprintf(stderr,
                    "psxrecomp-codegen: starting deferred rebuild helper\n");
            /* cmd /C strips the first and last quote of the command line
             * when it holds special characters such as the parentheses in
             * "r4-1.0-windows-x64 (1)"; the doubled outer pair keeps the
             * path's own quotes intact. */
            snprintf(cmd, sizeof(cmd), "cmd.exe /C \"\"%s\"\"", exe);
            flags = CREATE_NEW_CONSOLE;
        } else {
            fprintf(stderr, "psxrecomp-codegen: relaunching %s\n", exe);
            snprintf(cmd, sizeof(cmd), "\"%s\" --launcher", exe);
        }
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flags, NULL,
                            g_project_root, &si, &pi)) {
            fprintf(stderr, "psxrecomp-codegen: CreateProcess failed\n");
            exit(1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    {
        if (g_project_root[0] && chdir(g_project_root) != 0) {
            fprintf(stderr, "psxrecomp-codegen: chdir(%s) failed: %s\n",
                    g_project_root, strerror(errno));
        }
        fprintf(stderr, "psxrecomp-codegen: relaunching %s\n", exe);
        char* args[] = {exe, "--launcher", NULL};
        execv(exe, args);
        perror("psxrecomp-codegen: execv failed");
        exit(1);
    }
#endif
}

void psxrecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                  const PsxrecompCodegenHostConfig* cfg) {
    if (!gi || !cfg || !cfg->cmake_target || !cfg->exe_basename)
        return;

#if !defined(PSX_HAS_SETUP_WIZARD)
    /* Build did not opt into the setup-wizard product surface
     * (-DPSX_SETUP_WIZARD=ON / ENABLE_SETUP_WIZARD). Leave GameInfo dark so
     * recomp-ui never opens first-run / Generate & rebuild. */
    (void)cfg;
    return;
#else

    g_cfg = cfg;
    g_ready = 0;
    g_relaunch_is_helper = 0;
    g_project_root[0] = '\0';
    g_cli_path[0] = '\0';
    g_game_toml[0] = '\0';
    g_python[0] = '\0';
    g_cmake[0] = '\0';
    g_build_dir[0] = '\0';
    g_exe_path[0] = '\0';
    g_helper_path[0] = '\0';
    g_toolchain_bin[0] = '\0';
    g_cli_toolchain_bin[0] = '\0';
    /* Keep g_wizard_bios across re-apply within the same process. */

    snprintf(g_display, sizeof(g_display), "%s",
             cfg_or(cfg->display_name, "Game"));
    snprintf(g_cmake_target, sizeof(g_cmake_target), "%s", cfg->cmake_target);
    snprintf(g_exe_basename, sizeof(g_exe_basename), "%s", cfg->exe_basename);

    const char* force_env =
        cfg_or(cfg->force_setup_env, "PSXRECOMP_FORCE_SETUP");
    const char* force = getenv(force_env);
    const int force_setup = force && force[0] && force[0] != '0';

    /* Master switch for recomp-ui: first-run wizard + Generate & rebuild. */
    gi->setup_wizard_supported = 1;
    /* has_bios is NOT forced here. A setup SDK links OpenBIOS only and needs
     * the retail picker so prepare can ingest a dump, but a shipped bundled
     * build has no CLI, sources or toolchain to compile one in, and forcing
     * the row there offered "Generate & rebuild" that could never run.
     * main.cpp decides after this returns, from whether prepare_with_progress
     * was actually wired below (or nothing is linked yet). */

    if (!discover_project_root(g_project_root, sizeof(g_project_root))) {
        /* Still force the wizard when generated/ is missing — discover may
         * fail if the process cwd is unrelated to the project tree. */
        if (force_setup) {
            gi->needs_setup = 1;
            gi->prepare_required_before_continue = 1;
        }
        return;
    }
    if (!resolve_cli_path(g_project_root, g_cli_path, sizeof(g_cli_path))) {
        if (psxrecomp_codegen_host_sources_missing(cfg) || force_setup) {
            gi->needs_setup = 1;
            gi->prepare_required_before_continue = 1;
        }
        return;
    }
    if (!join_path(g_game_toml, sizeof(g_game_toml), g_project_root,
                   cfg_or(cfg->game_toml_relpath, "game.toml")))
        return;
    if (!path_is_file(g_game_toml))
        return;

    /* Wire prepare/rebuild even when Python is not on PATH yet — wizard page 0
     * installs cmake-clang-v1 (1.0.6+ ships portable CPython under python/).
     * generate/rebuild re-resolve after activate_toolchain_path. */
    g_ready = 1;
    activate_toolchain_path();
    (void)find_python(g_python, sizeof(g_python));
    gi->persist_setup = host_persist_setup;
    gi->persist_setup_ctx = NULL;
    /* Multi-disc titles only: load_game_toml_disc_roster leaves g_num_discs 0
     * for a single image, so the wizard keeps its one-image layout and the
     * launcher keeps taking the single-path flush. */
    load_game_toml_disc_roster();
    if (g_num_discs > 1) {
        gi->discs = g_disc_roster;
        gi->num_discs = g_num_discs;
        gi->persist_setup_discs = host_persist_setup_discs;
    }
    gi->prepare_with_progress = host_prepare_generate;
    gi->prepare_use_selected_rom = 1;
    /* Number prefix is applied in the setup UI (BIOS adds a step). */
    gi->prepare_section_title = "Generate BIOS + game C & rebuild";
    gi->prepare_busy_status = "Generating BIOS + game sources…";
    gi->prepare_success_status = "Sources ready — building…";

    /* Rebuild is offered whenever the build tree can be formed; the wizard
     * installs cmake-clang-v1 on page 0 before Generate & rebuild. */
    const int can_rebuild = resolve_build_paths();
    if (can_rebuild) {
        gi->prepare_disc_label = "Generate & rebuild…";
#if defined(_WIN32)
        gi->prepare_disc_note =
            cfg->prepare_note_windows
                ? cfg->prepare_note_windows
                : "Uses your disc with the local psxrecomp SDK to regenerate "
                  "generated/, then quits and rebuilds via a helper so the "
                  "running .exe is not locked.";
        gi->rebuild_busy_status = "Scheduling rebuild…";
        gi->rebuild_success_status =
            "Exiting for Windows rebuild — a console will finish the build…";
#else
        gi->prepare_disc_note =
            cfg->prepare_note
                ? cfg->prepare_note
                : "Uses your disc with the local psxrecomp SDK to regenerate "
                  "generated/, then runs cmake --build and restarts.";
        gi->rebuild_busy_status = "Building…";
        gi->rebuild_success_status = "Build complete — restarting…";
#endif
        gi->rebuild_with_progress = host_rebuild_game;
        gi->rebuild_after_prepare = 1;
        gi->relaunch_after_rebuild = 1;
        gi->setup_needs_toolchain = 1;
        gi->toolchain_is_ready = host_toolchain_is_ready;
        gi->ensure_toolchain_with_progress = host_ensure_toolchain_with_progress;
        gi->toolchain_update_available = host_toolchain_update_available;
        gi->toolchain_repair_note = host_toolchain_repair_note;
        /* Settings → SYSTEM: PGO on existing generated C (no wizard). */
        gi->pgo_optimize_with_progress = host_pgo_optimize;
#if defined(_WIN32)
        gi->pgo_busy_status = "Scheduling FMV PGO optimize…";
        gi->pgo_success_status =
            "Exiting for Windows PGO train + rebuild…";
#else
        gi->pgo_busy_status =
            "Optimizing FMV (instrument → train → rebuild)…";
        gi->pgo_success_status = "FMV optimize complete — restarting…";
#endif
        /* FMV timing (emitter load_charge_batch) generate+rebuild: host kept
         * but not exposed in Settings for now. */
    } else {
        gi->prepare_disc_label = "Generate sources…";
        gi->prepare_disc_note =
            cfg->prepare_note_no_cmake
                ? cfg->prepare_note_no_cmake
                : "Regenerates generated/ with the local psxrecomp SDK. "
                  "Build dir could not be resolved — rebuild manually, then "
                  "relaunch.";
        gi->prepare_success_status =
            "Sources generated. Rebuild manually, then relaunch.";
    }
    gi->persist_setup = host_persist_setup;
    gi->persist_setup_ctx = NULL;

    if (psxrecomp_codegen_host_sources_missing(cfg) || force_setup) {
        gi->needs_setup = 1;
        gi->prepare_required_before_continue = 1;
        if (!force_setup) {
            /* Loop breaker: a recent successful generate with sources still
             * missing is a config skew, not a first run — say so in the
             * wizard instead of silently asking for another round. */
            const char* note = host_loop_breaker_note();
            if (note)
                gi->prepare_disc_note = note;
        }
    } else {
        /* Sources are visible again — the last generate resolved; drop the
         * stamp so a much later manual wipe of generated/ reads as a fresh
         * first run, not a loop. */
        host_clear_generate_stamp();
    }
#endif /* PSX_HAS_SETUP_WIZARD */
}
