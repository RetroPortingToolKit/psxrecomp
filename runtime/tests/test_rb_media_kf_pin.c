#include "netplay_rb_media_kf_pin.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

typedef struct FakeRing {
    uint32_t tick;
    const uint8_t *data;
    size_t size;
    int calls;
} FakeRing;

static const uint8_t *fake_ring_peek(void *opaque, uint32_t tick,
                                     size_t *out_size)
{
    FakeRing *ring = (FakeRing *)opaque;
    ring->calls++;
    if (ring->tick == tick && ring->data && ring->size) {
        if (out_size)
            *out_size = ring->size;
        return ring->data;
    }
    if (out_size)
        *out_size = 0u;
    return NULL;
}

static uint32_t checksum32(const uint8_t *data, size_t size)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

int main(void)
{
    static const uint8_t pin_payload[] = { 'p', 'i', 'n', '2', '3', '0', '4' };
    static const uint8_t ring_payload[] = { 'r', 'i', 'n', 'g' };
    NprbMediaKfPin pin;
    FakeRing ring;
    size_t sz = 0;
    const uint8_t *p;
    uint32_t host_crc;
    uint32_t probe_crc;
    int ready = 0;

    memset(&pin, 0, sizeof(pin));
    pin.valid = 1;
    pin.tick = 2304u;
    pin.data = pin_payload;
    pin.size = sizeof(pin_payload);

    memset(&ring, 0, sizeof(ring));
    ring.tick = 2288u; /* load=2304 was evicted by realign drop_after(2288). */
    ring.data = ring_payload;
    ring.size = sizeof(ring_payload);

    p = nprb_media_kf_peek_local(&pin, &ring, 2304u, &sz, fake_ring_peek);
    CHECK(p == pin_payload, "host payload uses matching pinned baseline when ring snap was evicted");
    CHECK(sz == sizeof(pin_payload), "host payload reports pinned baseline size");
    CHECK(ring.calls == 0, "matching pin avoids stale ring lookup");
    host_crc = checksum32(p, sz);

    p = nprb_media_kf_peek_local(&pin, &ring, 2304u, &sz, fake_ring_peek);
    probe_crc = checksum32(p, sz);
    CHECK(probe_crc == host_crc, "CRC probe reads the same pinned baseline payload");

    CHECK(nprb_media_kf_seal_ready_from_pin(&pin, 2304u, &ready),
          "seal succeeds from matching pinned baseline");
    CHECK(ready == 1, "seal marks MEDIA-KF ready");
    CHECK(pin.data == pin_payload && pin.size == sizeof(pin_payload),
          "seal from pin does not free or mutate pinned payload ownership");

    ready = 0;
    CHECK(!nprb_media_kf_seal_ready_from_pin(&pin, 2288u, &ready),
          "seal refuses non-matching pin tick");
    CHECK(ready == 0, "failed seal leaves ready unchanged");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
