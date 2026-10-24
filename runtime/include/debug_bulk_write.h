#ifndef PSXRECOMP_DEBUG_BULK_WRITE_H
#define PSXRECOMP_DEBUG_BULK_WRITE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fits comfortably in the server's 8192-byte request envelope. */
#define PSX_DEBUG_BULK_MAX_BYTES 1024u

/* Archipelago wire format: {"addr":"0x80010000","hex":"0011aaff"}.
 * Validate the complete payload and RAM span before the first host store.
 * Called on the emulation thread; NULL means success, otherwise an error. */
const char *debug_bulk_write(const char *json);

#ifdef __cplusplus
}
#endif
#endif
