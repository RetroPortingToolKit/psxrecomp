/* boot_state_replace.inc - write a state file without losing the previous one.
 *
 * Included by boot_state.c, never built on its own. Kept in its own file so
 * test_boot_state_replace.c can include just this part and test the overwrite
 * and failure behaviour without the whole serializer.
 *
 * A save writes `<path>.tmp` first and moves it over `path` only when every
 * byte was written and the file closed cleanly. Any failure removes the
 * temporary and leaves the previous file at `path` exactly as it was.
 *
 * On Windows the CRT's rename() does not replace an existing target, and a
 * remove() followed by rename() loses the old file if the second step fails.
 * MoveFileExA with MOVEFILE_REPLACE_EXISTING replaces it in one step;
 * MOVEFILE_WRITE_THROUGH makes the call return only after the move is on disk.
 * The A form reads the path in the same code page as fopen() and remove(), so
 * all three name the same file. Elsewhere rename() already replaces atomically.
 *
 * A failed replace refuses the save (returns 0) instead of retrying: a scanner
 * briefly holding the new file open is a real failure the caller should see. */
#include "boot_state.h"

#include <stdio.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

int boot_state_replace_file(const char* from, const char* to) {
    if (!from || !to) return 0;
#if defined(_WIN32)
    return MoveFileExA(from, to,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 1 : 0;
#else
    return rename(from, to) == 0;
#endif
}

int boot_state_write_file_atomic(const char* path,
                                 int (*write)(FILE* f, void* ctx), void* ctx) {
    char tmp[1024];
    FILE* f;
    int ok;
    if (!path || !write) return 0;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    f = fopen(tmp, "wb");
    if (!f) return 0;
    ok = write(f, ctx);
    if (fclose(f) != 0) ok = 0;
    if (!ok || !boot_state_replace_file(tmp, path)) {
        remove(tmp);
        return 0;
    }
    return 1;
}
