#ifndef NETPLAY_SIM_PAD_CACHE_H
#define NETPLAY_SIM_PAD_CACHE_H

#include "psx_netplay.h"
#include <string.h>

#ifndef PSX_MAX_PLAYERS
#define PSX_MAX_PLAYERS 2
#endif

/* Host-only snapshot of the exact normalized seat rows published for one
 * simulation tick. Rollback seal publication may overwrite an earlier
 * predicted row for that same tick. Never serialize this cache: resimulation
 * republishes all rows after loading the guest state. */
typedef struct PsxNetplaySimPadCache {
    PsxNetPad pads[PSX_MAX_PLAYERS + 1];
    uint32_t ticks[PSX_MAX_PLAYERS + 1];
    uint8_t valid[PSX_MAX_PLAYERS + 1];
} PsxNetplaySimPadCache;

static inline void psx_netplay_sim_pad_cache_reset(PsxNetplaySimPadCache *cache)
{
    if (cache) memset(cache->valid, 0, sizeof(cache->valid));
}

static inline void psx_netplay_sim_pad_cache_publish(
    PsxNetplaySimPadCache *cache, int seat, uint32_t tick,
    const PsxNetPad *pad)
{
    if (!cache || !pad || seat < 0 || seat > PSX_MAX_PLAYERS) return;
    cache->pads[seat] = *pad;
    cache->ticks[seat] = tick;
    cache->valid[seat] = 1;
}

static inline int psx_netplay_sim_pad_cache_read(
    const PsxNetplaySimPadCache *cache, int seat, int seat_count,
    uint32_t tick, PsxNetPad *out)
{
    if (!cache || !out || seat < 0 || seat >= seat_count ||
        seat > PSX_MAX_PLAYERS || !cache->valid[seat] ||
        cache->ticks[seat] != tick) return 0;
    *out = cache->pads[seat];
    return 1;
}

#endif
