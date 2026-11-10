#ifndef PSXRECOMP_TIMERS_H
#define PSXRECOMP_TIMERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIMER_BASE 0x1F801100

void timers_init(void);
/* Snapshot mode bits 0..12 are guest mode; bit 16 is the one-shot latch and
 * bits 17..18 are the post-target dwell. Pass complete words back on restore.
 * Guest mode reads never expose these snapshot-only bits. */
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                         uint16_t target[3], int32_t irq_line[3],
                         uint32_t frac[3]);
void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                         const uint16_t target[3], const int32_t irq_line[3],
                         const uint32_t frac[3]);

/* MMIO read/write for 0x1F801100..0x1F80112F.
 * Accepts 16-bit or 32-bit access (caller zero-extends). */
uint32_t timers_read(uint32_t addr);
void     timers_write(uint32_t addr, uint32_t value);

/* Advance all timers by guest CPU cycles. Used by native block-cycle builds. */
void timers_advance(uint32_t cycles);

/* Cycle-budgeted precise event slicing: guest CPU cycles until a timer raises a
 * DELIVERABLE IRQ (unmasked in i_mask + mode-armed). UINT32_MAX if none. */
uint32_t timers_cycles_to_irq(uint32_t i_mask);

/* Legacy coarse tick used by interpreter/oracle builds. */
void timers_tick(int cycles);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_TIMERS_H */
