#ifndef PSX_NETPLAY_RB_MEDIA_KF_PIN_H
#define PSX_NETPLAY_RB_MEDIA_KF_PIN_H

#include <stddef.h>
#include <stdint.h>

typedef struct NprbMediaKfPin {
    int valid;
    uint32_t tick;
    const uint8_t *data;
    size_t size;
} NprbMediaKfPin;

typedef const uint8_t *(*NprbMediaKfRingPeek)(void *ring, uint32_t tick,
                                              size_t *out_size);

static inline const uint8_t *
nprb_media_kf_peek_local(const NprbMediaKfPin *pin, void *ring,
                         uint32_t tick, size_t *out_size,
                         NprbMediaKfRingPeek ring_peek)
{
    const uint8_t *p = NULL;
    size_t sz = 0;

    if (pin && pin->valid && pin->tick == tick && pin->data && pin->size) {
        if (out_size)
            *out_size = pin->size;
        return pin->data;
    }

    if (ring && ring_peek)
        p = ring_peek(ring, tick, &sz);
    if (out_size)
        *out_size = p ? sz : 0u;
    return p;
}

static inline int
nprb_media_kf_seal_ready_from_pin(const NprbMediaKfPin *pin, uint32_t tick,
                                  int *ready)
{
    if (!(pin && pin->valid && pin->tick == tick && pin->data && pin->size))
        return 0;
    if (ready)
        *ready = 1;
    return 1;
}

#endif /* PSX_NETPLAY_RB_MEDIA_KF_PIN_H */
