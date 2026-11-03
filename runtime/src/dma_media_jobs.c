/* Build-selected MDEC transfer jobs. A 32-word job is one MDEC output FIFO
 * (448 clocks at ch1's rate, about13.2us). Live source reads and attributed
 * RAM writes still use dma.c's data path, including wraps, decrement, stalls,
 * cancellation and final IRQ. Only internal service/publication is coarser.
 * Other DMA channels retain their own policies. */
#define PSX_MDEC_WORDS_PER_JOB 32u
#include "dma.c"
