#ifndef PSX_DIGITAL_PAD_SNAPSHOT_H
#define PSX_DIGITAL_PAD_SNAPSHOT_H
#include <stdint.h>
#include <string.h>
#include "mod_plugins.h"

/* Host-only observations. Never serialized with the emulated SIO machine. */
typedef struct PSXResolvedDigitalPad {
    uint32_t held, generation, press[16];
} PSXResolvedDigitalPad;
static inline void digital_pad_history_reset(PSXResolvedDigitalPad *s,uint16_t active_low) {
    uint32_t generation=s->generation+1u;
    memset(s,0,sizeof *s);s->generation=generation?generation:1u;
    s->held=(uint16_t)~active_low;
}
static inline void digital_pad_history_record(PSXResolvedDigitalPad *s,uint16_t active_low) {
    uint32_t held=(uint16_t)~active_low,pressed=held & ~s->held;
    for(unsigned i=0;i<16;++i)if(pressed & (1u<<i))++s->press[i];
    s->held=held;
}
static inline void digital_pad_reader_reset(PSXModDigitalPadReader *r) {
    memset(r,0,sizeof *r);r->struct_size=sizeof *r;
}
static inline int digital_pad_reader_read(PSXModDigitalPadReader *r,const PSXResolvedDigitalPad *s,
                                           uint64_t frame,uint16_t legacy,uint32_t *out) {
    if(!r || !s || !out || r->struct_size!=sizeof *r)return 0;
    uint32_t held=0;
    if(!r->valid || r->generation!=s->generation || frame<r->frame || frame-r->frame>1)goto prime;
    if(frame==r->frame) {*out=r->held;return 1;}
    for(unsigned i=0;i<16;++i) {
        uint32_t delta=s->press[i]-r->last_press[i];
        if(delta>4 || r->pending[i]>4-delta)goto prime;
    }
    for(unsigned i=0;i<16;++i) {
        const uint32_t bit=1u<<i;
        r->pending[i]+=s->press[i]-r->last_press[i];r->last_press[i]=s->press[i];
        if(r->held & bit) {
            /* A second real press implies an intervening release. Deliver
             * that release first, not a duplicate held/press frame. */
            if((s->held & bit) && !r->pending[i])held|=bit;
        } else if(r->pending[i]) {
            held|=bit;--r->pending[i];
        } else if(s->held & bit)held|=bit;
    }
    r->held=held;r->frame=frame;*out=held;return 1;
prime:
    digital_pad_reader_reset(r);r->generation=s->generation;r->frame=frame;
    r->held=legacy;r->valid=1;memcpy(r->last_press,s->press,sizeof r->last_press);
    return 0;
}
#endif
