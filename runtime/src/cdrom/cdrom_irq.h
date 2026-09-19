#ifndef PSXRECOMP_CDROM_IRQ_H
#define PSXRECOMP_CDROM_IRQ_H

#include <stdint.h>

/* HINTSTS bits 0..2 contain an encoded interrupt reason (INT1..INT5), not
 * independent one-hot flags. The controller nevertheless gates its IRQ line
 * by directly ANDing HINTSTS with HINTMSK. In particular, mask 0x07 enables
 * all five encoded reasons, including INT4 and INT5. */
static inline int cdrom_irq_mask_matches_reason(uint8_t irq_enable,
                                                uint8_t irq_reason) {
    return (irq_enable & irq_reason & 0x07u) != 0;
}

#endif /* PSXRECOMP_CDROM_IRQ_H */
