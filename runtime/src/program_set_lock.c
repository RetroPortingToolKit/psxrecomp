/* program_set_lock.c — see program_set_lock.h. */

#include "program_set_lock.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HANDLE s_lock = INVALID_HANDLE_VALUE;
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
static int s_lock = -1;
#endif

int psx_program_set_lock_acquire(const char *saves_dir)
{
    char path[1200];
    size_t n;

    if (!saves_dir || !saves_dir[0]) return -1;
    n = strlen(saves_dir);
    if (n + sizeof(PSX_PROGRAM_SET_LOCK_NAME) + 2u > sizeof(path)) return -1;
    snprintf(path, sizeof(path), "%s%s%s", saves_dir,
             (saves_dir[n - 1] == '/' || saves_dir[n - 1] == '\\') ? "" : "/",
             PSX_PROGRAM_SET_LOCK_NAME);

#if defined(_WIN32)
    if (s_lock != INVALID_HANDLE_VALUE) return 1;
    /* Share mode 0: a second open fails with a sharing violation while this
     * handle lives. DELETE_ON_CLOSE removes the file when the process ends. */
    s_lock = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                         NULL);
    if (s_lock != INVALID_HANDLE_VALUE) return 1;
    return GetLastError() == ERROR_SHARING_VIOLATION ? 0 : -1;
#else
    if (s_lock >= 0) return 1;
    s_lock = open(path, O_CREAT | O_RDWR, 0644);
    if (s_lock < 0) return -1;
    if (flock(s_lock, LOCK_EX | LOCK_NB) == 0) return 1;
    {
        const int held = (errno == EWOULDBLOCK);
        close(s_lock);
        s_lock = -1;
        return held ? 0 : -1;
    }
#endif
}
