/*
 * The overlay compiler child must not keep the runtime's files open.
 *
 * autocompile_request() starts the compiler with CreateProcess and handle
 * inheritance, because the child writes to a pipe. Without a handle list the
 * child got every inheritable handle of the process, and every fopen() handle
 * is inheritable. The snapshot writer thread is usually in the middle of a
 * commit at that instant, so cmd.exe kept the commit's temp file open until the
 * compile run ended. The runtime then closed its own handle and renamed the
 * file: MoveFileEx failed with ERROR_SHARING_VIOLATION (32), remove() failed
 * too, and every retry failed again while the compiler lived. In a cold start
 * that printed "additive capture history write failed" and "latest capture
 * write failed" a few times, delayed those snapshots by a whole compile run,
 * and left one overlay_captures.json.<pid>.<seq>.tmp in the game's folder per
 * "latest" line.
 *
 * The test, in two halves:
 *
 *   control  a child started the OLD way (inheritance on, no handle list)
 *            while a file is open: the rename of that file must FAIL with 32.
 *            This shows that the method sees the fault on this machine. A test
 *            that could not fail here would prove nothing.
 *   subject  the same file handling, the child started through
 *            autocompile_request(): the rename and the remove must SUCCEED
 *            while the child still runs, and the child's output must still
 *            arrive through the pipe.
 *
 * Windows only. On POSIX a file that a child holds open does not block a
 * rename or an unlink, so the fault cannot exist there; the test says so and
 * reports itself skipped (exit 77).
 *
 * Build/run: ctest -R autocompile_handle_inherit_test
 */
#ifndef _WIN32

#include <stdio.h>

int main(void) {
    printf("SKIP: Windows only. On POSIX a file held open by a child process "
           "does not block rename() or unlink(), so this fault cannot occur "
           "here.\n");
    return 77;
}

#else /* _WIN32 */

#include "autocompile.h"
#include "overlay_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

/* autocompile.c calls into the overlay loader when a child publishes a shard.
 * The child here publishes nothing; the symbols only have to resolve.
 * Signatures must match overlay_loader.h exactly. */
struct OverlayPreparedImage { unsigned id; };
OverlayPreparedImage *overlay_loader_prepare_published(const char *dll_path) {
    (void)dll_path; return NULL;
}
int  overlay_loader_commit_published(OverlayPreparedImage *image) { (void)image; return 0; }
void overlay_loader_discard_prepared(OverlayPreparedImage *image) { (void)image; }
void overlay_loader_rescan(void) { }

static int failures;

#define CHECK(cond, msg) do {                                                 \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; }        \
} while (0)

/* About three seconds, no console needed, present on every Windows. */
#define CHILD_COMMAND "ping -n 4 127.0.0.1"

static char s_dir[MAX_PATH];

static void path_in_dir(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s\\%s", s_dir, name);
}

/* The way overlay_capture.c writes a snapshot temp file: fopen, fwrite. The
 * caller closes it AFTER the child has been started. */
static FILE *open_as_the_runtime_does(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (f && mode[0] == 'w') {
        fputs("{\"snapshot\": 1}\n", f);
        fflush(f);
    }
    return f;
}

/* overlay_capture.c's atomic_replace_file(). Returns 0 or the Windows error. */
static DWORD move_into_place(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    return GetLastError();
}

/* The old spawn: inheritance on, no handle list. */
static HANDLE start_child_the_old_way(void) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[] = "cmd.exe /C \"" CHILD_COMMAND " >nul\"";
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int status_has(const char *needle) {
    char json[4096];
    int n = autocompile_status_json(json, (int)sizeof json);
    return n > 0 && strstr(json, needle) != NULL;
}

int main(void) {
    char cwd[MAX_PATH];
    char tmp[MAX_PATH], dst[MAX_PATH], held[MAX_PATH];
    DWORD err;

    GetCurrentDirectoryA(sizeof cwd, cwd);
    snprintf(s_dir, sizeof s_dir, "%s\\autocompile_handle_inherit_test.%lu",
             cwd, (unsigned long)GetCurrentProcessId());
    if (!CreateDirectoryA(s_dir, NULL)) {
        fprintf(stderr, "FAIL: cannot create %s (Windows error %lu)\n",
                s_dir, (unsigned long)GetLastError());
        return 1;
    }

    /* --- control: the old spawn holds the file, the rename fails with 32 --- */
    path_in_dir(tmp, sizeof tmp, "control.json.tmp");
    path_in_dir(dst, sizeof dst, "control.json");
    {
        FILE *f = open_as_the_runtime_does(tmp, "wb");
        CHECK(f != NULL, "control: temp file opens");
        HANDLE child = start_child_the_old_way();
        CHECK(child != NULL, "control: the old-way child starts");
        if (f) fclose(f);
        err = move_into_place(tmp, dst);
        printf("control: rename while an old-way child runs -> %s %lu\n",
               err ? "Windows error" : "ok", (unsigned long)err);
        CHECK(err == ERROR_SHARING_VIOLATION,
              "control: a child started without a handle list holds the file "
              "(the rename must fail with error 32, or this test cannot see the fault)");
        if (child) {
            TerminateProcess(child, 0);
            WaitForSingleObject(child, 10000);
            CloseHandle(child);
        }
        /* cmd.exe is gone; ping may outlive it for a moment with the handle. */
        for (int i = 0; i < 100 && (err = move_into_place(tmp, dst)) != 0; i++)
            Sleep(100);
        CHECK(err == 0, "control: the rename works once the child tree has ended");
    }

    /* --- subject: the real spawn must hold nothing but its pipe ------------ */
    path_in_dir(tmp, sizeof tmp, "subject.json.tmp");
    path_in_dir(dst, sizeof dst, "subject.json");
    path_in_dir(held, sizeof held, "subject-read.json.tmp");
    {
        FILE *w = open_as_the_runtime_does(tmp, "wb");
        FILE *seed = open_as_the_runtime_does(held, "wb");
        if (seed) fclose(seed);
        FILE *r = open_as_the_runtime_does(held, "rb");   /* a file being read back */
        CHECK(w != NULL && r != NULL, "subject: temp files open");

        autocompile_configure(CHILD_COMMAND, cwd);
        CHECK(autocompile_request() == 1, "subject: autocompile_request starts the child");
        if (w) fclose(w);
        if (r) fclose(r);

        err = move_into_place(tmp, dst);
        printf("subject: rename while the autocompile child runs -> %s %lu\n",
               err ? "Windows error" : "ok", (unsigned long)err);
        CHECK(err == 0, "subject: a file that was open when the compiler started "
                        "can be renamed while the compiler runs");
        CHECK(DeleteFileA(held) != 0, "subject: a file that was open for reading "
                                      "when the compiler started can be removed");
        CHECK(status_has("\"state\":\"running\""),
              "subject: the child was still running when the rename was made");

        /* The pipe is the one handle the child must still get. */
        for (int i = 0; i < 300 && autocompile_busy(); i++) {
            autocompile_poll_main();
            Sleep(50);
        }
        CHECK(!autocompile_busy(), "subject: the child ends and the run is reaped");
        CHECK(status_has("127.0.0.1"),
              "subject: the child's output still arrives through the pipe");
        CHECK(status_has("\"last_exit\":0"), "subject: the child exits 0");
        autocompile_shutdown();
    }

    path_in_dir(tmp, sizeof tmp, "control.json");          DeleteFileA(tmp);
    path_in_dir(tmp, sizeof tmp, "control.json.tmp");      DeleteFileA(tmp);
    path_in_dir(tmp, sizeof tmp, "subject.json");          DeleteFileA(tmp);
    path_in_dir(tmp, sizeof tmp, "subject.json.tmp");      DeleteFileA(tmp);
    path_in_dir(tmp, sizeof tmp, "subject-read.json.tmp"); DeleteFileA(tmp);
    RemoveDirectoryA(s_dir);

    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    printf("test_autocompile_handle_inherit: all checks passed\n");
    return 0;
}

#endif /* _WIN32 */
