/* Serializer binding only. File lifetime, synchronization, replacement and
 * failed-transaction cleanup belong to the shared persistence provider. */
#include "boot_state.h"
#include <recomp_persistence/atomic_file.h>

int boot_state_write_file_atomic(const char* path,
                                 int (*write)(RecompAtomicFile*, void* ctx), void* ctx) {
    if (!path || !write) return 0;
    /* Match the existing loader's file budget while streaming. */
    RecompAtomicFile* f = recomp_atomic_file_begin(path, 64u * 1024u * 1024u);
    if (!f) return 0;
    if (!write(f, ctx)) {
        recomp_atomic_file_abort(f);
        return 0;
    }
    return recomp_atomic_file_commit(f);
}
