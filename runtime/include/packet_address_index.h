#ifndef PSX_PACKET_ADDRESS_INDEX_H
#define PSX_PACKET_ADDRESS_INDEX_H
#include <stdint.h>
#include <string.h>

/* Fixed-size lookup scratch for packet guards. Value zero denotes an empty
 * slot; store array index + 1. First insertion wins, matching the old scan.
 * Capacity must be a power of two and at least twice the node count. */
typedef struct { uint32_t address,value; } PSXPacketAddressIndex;
static inline uint32_t psx_packet_address_bucket(uint32_t address,uint32_t capacity) {
    return ((address>>2)*2654435761u)&(capacity-1u);
}
static inline void psx_packet_address_index_clear(PSXPacketAddressIndex* slots,uint32_t capacity) {
    memset(slots,0,sizeof(*slots)*capacity);
}
static inline void psx_packet_address_index_add(PSXPacketAddressIndex* slots,uint32_t capacity,uint32_t address,uint32_t index) {
    uint32_t bucket=psx_packet_address_bucket(address,capacity);
    for(uint32_t n=0;n<capacity;++n,bucket=(bucket+1u)&(capacity-1u)) {
        if(slots[bucket].value && slots[bucket].address==address)return;
        if(!slots[bucket].value){slots[bucket].address=address;slots[bucket].value=index+1u;return;}
    }
}
static inline uint32_t psx_packet_address_index_find(const PSXPacketAddressIndex* slots,uint32_t capacity,uint32_t address) {
    uint32_t bucket=psx_packet_address_bucket(address,capacity);
    for(uint32_t n=0;n<capacity;++n,bucket=(bucket+1u)&(capacity-1u)) {
        if(!slots[bucket].value)return UINT32_MAX;
        if(slots[bucket].address==address)return slots[bucket].value-1u;
    }
    return UINT32_MAX;
}
#endif
