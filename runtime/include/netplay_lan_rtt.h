#ifndef NETPLAY_LAN_RTT_H
#define NETPLAY_LAN_RTT_H

#include <stdint.h>

/* Waiting-room probes share the lobby socket. Keep ownership and the echoed
 * timestamp together so a late reply cannot be attributed to a new occupant. */
typedef struct NetplayLanRtt {
    uint64_t peer;
    uint32_t sent_ms;
    uint32_t last_probe_ms;
    int has_probe;
    int pending;
    int latency_ms;
} NetplayLanRtt;

static inline void netplay_lan_rtt_bind(NetplayLanRtt *sample, uint64_t peer)
{
    if (sample->peer == peer) return;
    sample->peer = peer;
    sample->sent_ms = sample->last_probe_ms = 0;
    sample->has_probe = sample->pending = 0;
    sample->latency_ms = -1;
}

static inline int netplay_lan_rtt_due(const NetplayLanRtt *sample, uint32_t now)
{
    return sample->peer && (!sample->has_probe ||
        (uint32_t)(now - sample->last_probe_ms) >= 2500u);
}

static inline void netplay_lan_rtt_sent(NetplayLanRtt *sample, uint32_t now)
{
    sample->sent_ms = sample->last_probe_ms = now;
    sample->has_probe = sample->pending = 1;
}

static inline int netplay_lan_rtt_reply(NetplayLanRtt *sample, uint64_t peer,
                                       uint32_t echoed, uint32_t now)
{
    uint32_t elapsed = (uint32_t)(now - echoed);
    if (!sample->pending || sample->peer != peer ||
        sample->sent_ms != echoed || elapsed > 60000u) return 0;
    sample->pending = 0;
    /* Retain the slower sample while in the room, as the automatic delay
     * must cover the path's jitter rather than its fastest packet. */
    if (sample->latency_ms < 0 || (int)elapsed > sample->latency_ms)
        sample->latency_ms = (int)elapsed;
    return 1;
}

#endif
