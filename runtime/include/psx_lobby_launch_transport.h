/* psx_lobby_launch_transport.h - how a lobby `launch` is carried.
 *
 * The lobby server picks the transport at `start` (recomp-net-server
 * start_transport, WS_LOBBY.md "Host relay"):
 *   "host" -- the host carries the match on host_endpoint; guests dial it.
 *   "ice"  -- a 2-player room the host relay cannot carry starts on ICE:
 *             peer-to-peer over STUN/TURN, signalled over the lobby socket.
 *             The endpoints are not dialled at all.
 *   a relay_endpoint, or equal host/guest endpoints from a server that
 *   rewrote both to its relay advertise address -- legacy server SFU.
 *
 * The last rule used to win over "ice": two peers behind one NAT advertise
 * the same public endpoint, so an ICE launch between them looked like a relay
 * and each peer dialled its own address -- the match never linked. */
#ifndef PSX_LOBBY_LAUNCH_TRANSPORT_H
#define PSX_LOBBY_LAUNCH_TRANSPORT_H

#include <stdlib.h>
#include <string.h>

enum {
    PSX_LAUNCH_DIRECT = 0, /* dial the peer endpoint (legacy p2p UDP) */
    PSX_LAUNCH_HOST = 1,   /* host relay: host binds, guests dial host_endpoint */
    PSX_LAUNCH_ICE = 2,    /* ICE peer-to-peer, signalled over the lobby */
    PSX_LAUNCH_RELAY = 3   /* server input relay (SFU) */
};

static inline int psx_lobby_endpoint_usable(const char *ep)
{
    const char *colon;
    if (!ep || !ep[0]) return 0;
    colon = strrchr(ep, ':');
    if (!colon || !colon[1]) return 0;
    return strtoul(colon + 1, NULL, 10) != 0;
}

/* transport: the launch's "transport" field ("" when absent).
 * relay_endpoint: the launch's relay_endpoint ("" when absent).
 * caps_force_relay: the host's match_caps asked for the server relay.
 * my_bind: this peer's own advertised bind ("" when unknown). */
static inline int psx_lobby_launch_transport(const char *transport,
                                             const char *relay_endpoint,
                                             int caps_force_relay,
                                             const char *host_ep,
                                             const char *guest_ep,
                                             const char *my_bind)
{
    if (transport && strcmp(transport, "host") == 0) return PSX_LAUNCH_HOST;
    if (transport && strcmp(transport, "ice") == 0) return PSX_LAUNCH_ICE;
    if (psx_lobby_endpoint_usable(relay_endpoint) || caps_force_relay)
        return PSX_LAUNCH_RELAY;
    if (psx_lobby_endpoint_usable(host_ep) && psx_lobby_endpoint_usable(guest_ep) &&
        strcmp(host_ep, guest_ep) == 0 &&
        (!my_bind || !my_bind[0] || strcmp(host_ep, my_bind) != 0))
        return PSX_LAUNCH_RELAY;
    return PSX_LAUNCH_DIRECT;
}

#endif
