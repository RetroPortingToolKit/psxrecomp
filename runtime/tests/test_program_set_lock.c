/* test_program_set_lock.c — two programs of one set must not run at once.
 *
 * The parent takes the lock in a folder, then starts this same test binary as
 * a child that tries to take it. The child must be refused while the parent
 * lives. A child started on a folder nobody holds must get the lock.
 *
 *   test_program_set_lock            run the test (uses the working directory)
 *   test_program_set_lock child DIR  exit code = 10 + acquire result
 */
#include "program_set_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/wait.h>
#define make_dir(p) mkdir((p), 0755)
#endif

static int s_fails = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); s_fails++; } } while (0)

/* Exit code of `self child dir`, or -1. */
static int run_child(const char *self, const char *dir)
{
    char cmd[2600];
    int status;
#if defined(_WIN32)
    /* cmd.exe strips one outer pair of quotes; give it one to strip. */
    snprintf(cmd, sizeof(cmd), "\"\"%s\" child \"%s\"\"", self, dir);
    status = system(cmd);
    return status;
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" child \"%s\"", self, dir);
    status = system(cmd);
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

int main(int argc, char **argv)
{
    const char *held = "program-set-lock-test-held";
    const char *free_dir = "program-set-lock-test-free";

    if (argc == 3 && strcmp(argv[1], "child") == 0)
        return 10 + psx_program_set_lock_acquire(argv[2]);

    make_dir(held);
    make_dir(free_dir);

    /* The first program gets the lock; asking again in the same process is
     * still "held by us" (the runtime reaches this code on every session
     * reboot). */
    CHECK(psx_program_set_lock_acquire(held) == 1);
    CHECK(psx_program_set_lock_acquire(held) == 1);

    /* A second process on the same folder is refused: result 0. */
    CHECK(run_child(argv[0], held) == 10);

    /* A folder nobody holds: the child gets it (result 1), and releases it
     * when it exits, so the next child gets it again. */
    CHECK(run_child(argv[0], free_dir) == 11);
    CHECK(run_child(argv[0], free_dir) == 11);

    /* No folder, or a folder that does not exist: an error, not "held". */
    CHECK(run_child(argv[0], "program-set-lock-test-missing/nowhere") == 9);

    if (s_fails) {
        fprintf(stderr, "program_set_lock_test: %d check(s) failed\n", s_fails);
        return 1;
    }
    printf("program_set_lock_test: lock held, refused, released and error cases passed\n");
    return 0;
}
