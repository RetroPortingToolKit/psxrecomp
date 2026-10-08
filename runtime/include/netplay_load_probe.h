#ifndef PSX_NETPLAY_LOAD_PROBE_H
#define PSX_NETPLAY_LOAD_PROBE_H

#include <stdint.h>

/* A guest can match the host's LOAD probe and start applying its local save
 * before the host has collected every seat's reply. If another seat misses,
 * the host later sends the authoritative blob to all guests. Remember the
 * verified local probe so a guest that already applied those exact bytes can
 * verify the transfer and avoid restoring the same machine state twice. */
typedef struct PsxNetplayLoadProbe {
    int valid;
    int local_applied;
    int slot;
    uint32_t size;
    uint32_t crc;
} PsxNetplayLoadProbe;

static inline void psx_netplay_load_probe_clear(PsxNetplayLoadProbe *probe)
{
    if (!probe) return;
    probe->valid = 0;
    probe->local_applied = 0;
    probe->slot = -1;
    probe->size = 0;
    probe->crc = 0;
}

static inline void psx_netplay_load_probe_record(PsxNetplayLoadProbe *probe,
                                                  int slot, uint32_t size,
                                                  uint32_t crc)
{
    if (!probe) return;
    probe->valid = size != 0u;
    probe->local_applied = 0;
    probe->slot = slot;
    probe->size = size;
    probe->crc = crc;
}

static inline void psx_netplay_load_probe_mark_applied(PsxNetplayLoadProbe *probe,
                                                        int slot)
{
    if (probe && probe->valid && probe->slot == slot)
        probe->local_applied = 1;
}

/* All four fingerprints are required: local probe, completed local apply,
 * authoritative wire blob, and a fresh read of the still-unmodified save
 * slot. The caller computes the blob CRC with the protocol checksum routine. */
static inline int psx_netplay_load_probe_can_reuse(
    const PsxNetplayLoadProbe *probe, int slot,
    uint32_t blob_size, uint32_t blob_crc,
    uint32_t local_size, uint32_t local_crc)
{
    return probe && probe->valid && probe->local_applied &&
           probe->slot == slot && probe->size == blob_size &&
           probe->crc == blob_crc && local_size == probe->size &&
           local_crc == probe->crc;
}

#endif /* PSX_NETPLAY_LOAD_PROBE_H */
