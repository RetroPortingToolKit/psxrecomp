#ifndef PSX_DMA_HOST_DRAW_H
#define PSX_DMA_HOST_DRAW_H
#include "dma_gpu_ll.h"
typedef struct DMAHostDrawRange { uint32_t start,bytes; } DMAHostDrawRange;
/* Trusted host rendering may produce complete draw packets in an allocated
 * aperture. Native RAM and nodes crossing a declared span are never eligible.
 * Only polygons and one-word drawing environment commands are accepted. */
static inline int dma_host_draw_drain(DMAGPULinkedList *s,
        const DMAHostDrawRange ranges[2],const DMAGPULinkedListOps *ops,void *user) {
    if(!s->active || !ops || !ops->read_word ||
       (s->phase!=DMA_GPU_LL_PHASE_HEADER && s->phase!=DMA_GPU_LL_PHASE_SETUP &&
        s->phase!=DMA_GPU_LL_PHASE_PAYLOAD))return 0;
    uint32_t address=ops->resolve_address?ops->resolve_address(user,s->current_addr):s->current_addr;
    unsigned slot;
    for(slot=0;slot<2;++slot)
        if(ranges[slot].bytes>=4 && address>=ranges[slot].start &&
           address-ranges[slot].start<=ranges[slot].bytes-4)break;
    if(slot==2)return 0;
    uint32_t header=ops->read_word(user,address),words=header>>24;
    if(4u*(words+1u)>ranges[slot].bytes-(address-ranges[slot].start))return 0;
    for(uint32_t i=0;i<words;) {
        uint32_t op=ops->read_word(user,address+4u+4u*i)>>24,n;
        if(op>=0xE1u && op<=0xE6u)n=1;
        else if(op>=0x20u && op<=0x3Fu) {
            uint32_t vertices=(op&8u)?4u:3u;
            n=1u+vertices+((op&16u)?vertices-1u:0u)+((op&4u)?vertices:0u);
        }else return 0;
        if(n>words-i)return 0;
        i+=n;
    }
    /* A whole immutable node can use one walker call. Stop exactly before
     * the next header, even when its link points back to this same node. */
    if(s->phase!=DMA_GPU_LL_PHASE_HEADER && s->payload_index>=words)return 0;
    uint32_t budget=s->phase==DMA_GPU_LL_PHASE_HEADER?words+1u:words-s->payload_index;
    dma_gpu_ll_advance(s,budget,ops,user);
    return 1;
}
#endif
