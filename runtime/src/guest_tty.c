#include "guest_tty.h"

#include <stdatomic.h>
#include <stdio.h>

#define GUEST_TTY_RING_CAP 65536u
#define GUEST_TTY_RING_MASK (GUEST_TTY_RING_CAP - 1u)

static _Atomic unsigned char s_guest_tty_ring[GUEST_TTY_RING_CAP];
static _Atomic uint64_t s_guest_tty_total;
static char s_guest_tty_line[257];
static unsigned s_guest_tty_line_len;

void psx_guest_tty_putchar(uint8_t value)
{
    uint64_t seq = atomic_load_explicit(&s_guest_tty_total,
                                        memory_order_relaxed);
    atomic_store_explicit(&s_guest_tty_ring[seq & GUEST_TTY_RING_MASK], value,
                          memory_order_relaxed);
    atomic_store_explicit(&s_guest_tty_total, seq + 1u, memory_order_release);

    /* Preserve the existing human-readable runtime log alongside the
     * structured ring used by automation. */
    if (value == '\n' || s_guest_tty_line_len >= 256u) {
        s_guest_tty_line[s_guest_tty_line_len] = '\0';
        fprintf(stderr, "tty: %s\n", s_guest_tty_line);
        s_guest_tty_line_len = 0;
        if (value != '\n' && value != '\r')
            s_guest_tty_line[s_guest_tty_line_len++] = (char)value;
    } else if (value != '\r') {
        s_guest_tty_line[s_guest_tty_line_len++] = (char)value;
    }
}

size_t psx_guest_tty_snapshot(uint8_t *out, size_t capacity,
                              uint64_t *total_bytes)
{
    uint64_t total = atomic_load_explicit(&s_guest_tty_total,
                                          memory_order_acquire);
    size_t available = (total < GUEST_TTY_RING_CAP)
        ? (size_t)total : (size_t)GUEST_TTY_RING_CAP;
    size_t count = (capacity < available) ? capacity : available;
    uint64_t start = total - count;

    if (total_bytes) *total_bytes = total;
    if (!out) return count;

    for (size_t i = 0; i < count; ++i) {
        out[i] = atomic_load_explicit(
            &s_guest_tty_ring[(start + i) & GUEST_TTY_RING_MASK],
            memory_order_relaxed);
    }
    return count;
}

void psx_guest_tty_reset(void)
{
    atomic_store_explicit(&s_guest_tty_total, 0u, memory_order_release);
    s_guest_tty_line_len = 0;
}
