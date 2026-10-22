/* Private execution buffers are prepared by their owner before publication.
 * Touch each host page here so immutable source capture never pays the first
 * write fault for a large replay image. This is not guest memory tracking. */
#ifndef PSX_EXECUTION_STORAGE_H
#define PSX_EXECUTION_STORAGE_H
#include <stdlib.h>
#include <stddef.h>
static inline void *execution_storage_calloc(size_t bytes) {
    void *storage = calloc(1, bytes);
    if (storage && bytes) {
        volatile unsigned char *pages = (volatile unsigned char *)storage;
        for (size_t offset = 0; offset < bytes; offset += 4096u) pages[offset] = 0;
        pages[bytes - 1] = 0;
    }
    return storage;
}
#endif
