/* Build-selected ENHANCED scheduling. The existing SPU mixer owns sample math
 * and catches up at every register, DMA, CD-input and snapshot observation. */
#define PSX_BLOCK_AUDIO 1
int spu_irq_can_raise(void);
#include "interrupts.c"
