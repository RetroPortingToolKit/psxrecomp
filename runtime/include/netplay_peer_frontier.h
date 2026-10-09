/* Per-seat peer RESOLVED frontiers for rollback episodes.
 *
 * A follower can only replay from a snapshot at or below the frontier it
 * advertised. With two seats there is one follower and one number. With more,
 * the initiator must open at or below the SLOWEST follower's frontier: taking
 * the highest advert (the old single watermark) let the fastest peer stand in
 * for the others, and on a 3-seat WAN race the host opened load=2400 against
 * followers at 2344 and 2319 -- every follower refused, the episodes aborted
 * and the session diverged before the race started.
 *
 * Header-only and free of runtime state so the bookkeeping is unit-tested
 * (tests/test_netplay_peer_frontier.c). */
#ifndef NETPLAY_PEER_FRONTIER_H
#define NETPLAY_PEER_FRONTIER_H

#include <stdint.h>
#include <string.h>

#define NETPLAY_PEER_FRONTIER_SEATS 16u

typedef struct NetplayPeerFrontier {
    uint32_t through[NETPLAY_PEER_FRONTIER_SEATS];
} NetplayPeerFrontier;

static inline void netplay_peer_frontier_reset(NetplayPeerFrontier *f)
{
    if (f)
        memset(f, 0, sizeof(*f));
}

/* A RESOLVED advert from `seat`. Watermark: never lowers. */
static inline void netplay_peer_frontier_note(NetplayPeerFrontier *f, int seat,
                                              uint32_t through)
{
    if (!f || seat < 0 || (uint32_t)seat >= NETPLAY_PEER_FRONTIER_SEATS)
        return;
    if (through > f->through[seat])
        f->through[seat] = through;
}

/* Evidence above `tick` was invalidated (realign) or a follower proved it
 * cannot follow above it (NACK): no seat may stay above it. */
static inline void netplay_peer_frontier_clamp(NetplayPeerFrontier *f, uint32_t tick)
{
    uint32_t i;
    if (!f)
        return;
    for (i = 0; i < NETPLAY_PEER_FRONTIER_SEATS; ++i)
        if (f->through[i] > tick)
            f->through[i] = tick;
}

/* Frontier every seat in `seats` has reached: the lowest, and 0 while any of
 * them has not advertised yet. */
static inline uint32_t netplay_peer_frontier_min(const NetplayPeerFrontier *f,
                                                 uint32_t seats)
{
    uint32_t i, lo = 0xffffffffu;
    int any = 0;
    if (!f)
        return 0u;
    for (i = 0; i < NETPLAY_PEER_FRONTIER_SEATS; ++i) {
        if (!(seats & (1u << i)))
            continue;
        any = 1;
        if (f->through[i] < lo)
            lo = f->through[i];
    }
    return any ? lo : 0u;
}

/* §55 bisect cap: after a baseline mismatch at load L the next load must be
 * below L. Once every peer has hash-confirmed a tick at or past L (agreed
 * ADVANCE from a live confirm) the fork is healed and the cap is stale; left
 * armed it refused every later episode, because the only loads it allowed had
 * long left the snapshot ring (4-seat race: cap 1928, agreed 3136, ring
 * 2656..3166 -> "no confirmed snap" and a permanent fork from 3168). */
static inline int netplay_fork_cap_retired(uint32_t fork_cap, uint32_t agreed_through)
{
    return fork_cap > 0u && agreed_through >= fork_cap;
}

#endif /* NETPLAY_PEER_FRONTIER_H */
