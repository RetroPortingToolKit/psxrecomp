#ifndef PSXRECOMP_GUEST_TTY_H
#define PSXRECOMP_GUEST_TTY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded observability sink for bytes emitted through the PS1 guest console.
 * Writers never block. A snapshot returns the newest complete byte window and
 * the monotonically increasing total number of bytes observed. */
void psx_guest_tty_putchar(uint8_t value);
size_t psx_guest_tty_snapshot(uint8_t *out, size_t capacity,
                              uint64_t *total_bytes);
void psx_guest_tty_reset(void);

#ifdef __cplusplus
}
#endif

#endif
