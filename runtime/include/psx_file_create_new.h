#ifndef PSX_FILE_CREATE_NEW_H
#define PSX_FILE_CREATE_NEW_H

/* Create a file that must not exist yet and open it for writing.
 *
 * C11 gives fopen an exclusive-create flag ("x") for this. msvcrt.dll, the C
 * runtime of an MSYS2 MINGW64 build, rejects that flag: the call returns NULL
 * for every path. An exclusive open() followed by fdopen() does the same job
 * on msvcrt, UCRT and POSIX.
 *
 * `binary` chooses what the "b" of an fopen mode chooses. 0 is a text stream:
 * on Windows "\n" is written as "\r\n", as a text-mode fopen does. 1 writes the
 * bytes as given.
 *
 * Returns NULL when the file exists (errno EEXIST) or cannot be created. A file
 * that exists is never opened, cut short or changed. */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#ifndef __cplusplus
/* <stdio.h> declares fdopen only for a unit that asked for POSIX. A unit built
 * with -std=c11 and no feature macro did not. */
FILE *fdopen(int, const char *);
#endif
#endif

static inline FILE *psx_file_create_new(const char *path, int binary) {
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | (binary ? _O_BINARY : _O_TEXT),
                   _S_IREAD | _S_IWRITE);
    FILE *stream = fd < 0 ? NULL : _fdopen(fd, binary ? "wb" : "wt");
    if (!stream && fd >= 0) _close(fd);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    FILE *stream = fd < 0 ? NULL : fdopen(fd, binary ? "wb" : "w");
    if (!stream && fd >= 0) close(fd);
#endif
    return stream;
}

#endif
