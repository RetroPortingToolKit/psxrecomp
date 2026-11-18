#ifndef PSX_HOST_EXECUTABLE_IDENTITY_H
#define PSX_HOST_EXECUTABLE_IDENTITY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Exact on-disk running image, obtained from the OS, never argv[0]/cwd.
 * Computed once per process. On failure out is untouched and callers must not
 * offer a same-image qualification. This does not attest loaded DLLs, JIT
 * output or mutable process memory, nor qualify different builds/hosts. */
int psx_running_executable_sha256(uint8_t out[32]);
#ifdef __cplusplus
}
#endif
#endif
