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

/* Concurrent BEGIN while an episode is active. Wire epoch ids carry their
 * initiator in the low bits (`slot_mask`); the lower initiator slot wins.
 * Returns +1 when the incoming episode wins (yield ours if still in the
 * handshake, then follow it), -1 when ours wins (drop the incoming BEGIN),
 * 0 when the rule does not apply (same epoch, or a follower seeing another
 * epoch from the initiator it already follows -- an abort-and-reopen when it
 * is newer; an older one is dead and netplay_rb_begin_dead drops it first).
 *
 * A follower applies it too: with three or more seats two guests can both
 * BEGIN on the same mispredict, and a follower that followed whichever
 * arrived first split the seats across epochs (4-seat WAN: host following
 * slot 3's epoch while slot 1's won; every seat then waited on baselines
 * that never came). */
static inline int netplay_rb_concurrent_begin(uint32_t ours, uint32_t theirs,
                                              int following, uint32_t slot_mask)
{
    uint32_t our_slot = ours & slot_mask;
    uint32_t their_slot = theirs & slot_mask;
    if (ours == theirs)
        return 0;
    if (following && our_slot == their_slot)
        return 0;
    if (their_slot < our_slot)
        return 1;
    if (their_slot > our_slot)
        return -1;
    return 0;
}

/* Newest BEGIN epoch heard from each initiator slot, and whether this peer
 * refused it with a follow NACK.
 *
 * An initiator runs one episode at a time and numbers each new one above
 * every epoch it has seen, so a BEGIN below the newest one heard from the
 * same initiator is a delayed retransmit of an episode it already left. A
 * NACK ends the epoch it answers: every initiator NACK path aborts that
 * episode and reopens, if at all, under a new epoch. Following either kind
 * revives a dead episode: the follower waits for a baseline nobody sends
 * while the initiator, already back in Live, starves for its input (100 ms
 * latency soak: BEGIN 536 refused, 544 followed, a late 536 retransmit
 * stashed during 544 and unstashed after it; both peers stalled until the
 * match ended). */
#define NETPLAY_RB_BEGIN_SLOTS 8u

typedef struct NetplayRbBeginSeen {
    uint32_t newest[NETPLAY_RB_BEGIN_SLOTS];
    uint8_t  heard[NETPLAY_RB_BEGIN_SLOTS];
    uint8_t  refused[NETPLAY_RB_BEGIN_SLOTS];
} NetplayRbBeginSeen;

static inline void netplay_rb_begin_seen_reset(NetplayRbBeginSeen *s)
{
    if (s)
        memset(s, 0, sizeof(*s));
}

/* Record a BEGIN heard, or this peer's NACK for one (`refused`). An older
 * epoch never lowers the record; a newer one replaces it and its refusal. */
static inline void netplay_rb_begin_seen_note(NetplayRbBeginSeen *s, uint32_t epoch,
                                              uint32_t slot_mask, int refused)
{
    uint32_t slot = epoch & slot_mask;
    if (!s || slot >= NETPLAY_RB_BEGIN_SLOTS)
        return;
    if (!s->heard[slot] || epoch > s->newest[slot]) {
        s->heard[slot] = 1u;
        s->newest[slot] = epoch;
        s->refused[slot] = 0u;
    }
    if (refused && epoch == s->newest[slot])
        s->refused[slot] = 1u;
}

/* 1 when the BEGIN must be dropped, not followed or stashed: it is older
 * than the newest epoch heard from its initiator, or it is that epoch and
 * this peer refused it. A refusal NACK lost on the way is covered by the
 * follower's own NACK retransmit, not by answering the stale BEGIN. */
static inline int netplay_rb_begin_dead(const NetplayRbBeginSeen *s, uint32_t epoch,
                                        uint32_t slot_mask)
{
    uint32_t slot = epoch & slot_mask;
    if (!s || slot >= NETPLAY_RB_BEGIN_SLOTS || !s->heard[slot])
        return 0;
    return epoch < s->newest[slot] ||
           (epoch == s->newest[slot] && s->refused[slot]);
}

#endif /* NETPLAY_PEER_FRONTIER_H */
