/* netplay_ice_hub.c -- see netplay_ice_hub.h. No recomp-net, no sockets. */
#include "netplay_ice_hub.h"

#include <stddef.h>

PsxTransportChoice psx_transport_decide(int cfg_transport, int transport_ice_hub,
                                        int transport_host, int force_input_relay,
                                        int in_online_room, int has_peer_endpoint,
                                        int ice_built)
{
    /* Hub first: it implies transport_host, and it is decided by the launch
     * alone. Neither force_input_relay nor an explicit transport=ice may
     * redirect it onto the SFU or the single-agent path. */
    if (transport_ice_hub) {
        if (cfg_transport == 2 || !ice_built) return PSX_TRANSPORT_ERROR_NO_PEER;
        return PSX_TRANSPORT_ICE_HUB;
    }
    if (cfg_transport == 2) return PSX_TRANSPORT_LAN_UDP;
    /* Legacy host relay (advertised UDP port): the LAN path, not the SFU. */
    if (transport_host) return PSX_TRANSPORT_LAN_UDP;
    if (force_input_relay || in_online_room)
        return has_peer_endpoint ? PSX_TRANSPORT_LAN_UDP : PSX_TRANSPORT_ERROR_NO_PEER;
    if (cfg_transport == 1 && ice_built) return PSX_TRANSPORT_SINGLE_ICE;
    return PSX_TRANSPORT_LAN_UDP;
}

const char *psx_ice_hub_map_error_text(PsxIceHubMapError e)
{
    switch (e) {
    case PSX_ICE_HUB_MAP_OK:             return "ok";
    case PSX_ICE_HUB_MAP_BAD_ARGS:       return "bad arguments";
    case PSX_ICE_HUB_MAP_SEAT_UNMAPPED:  return "a connected seat holds no session slot";
    case PSX_ICE_HUB_MAP_SEAT_DUPLICATE: return "two connections for one seat";
    case PSX_ICE_HUB_MAP_SEAT_MISSING:   return "a session slot has no connection";
    }
    return "unknown";
}

static int fail(PsxIceHubMapError *err_out, PsxIceHubMapError e)
{
    if (err_out) *err_out = e;
    return -1;
}

int psx_ice_hub_map_seats(const int *port_of_slot, int port_map_valid,
                          int slot_count, int host_spectates,
                          const int *lobby_slots, int n,
                          int *session_slots_out, PsxIceHubMapError *err_out)
{
    int i, s;
    unsigned used = 0;
    if (err_out) *err_out = PSX_ICE_HUB_MAP_OK;
    if (!lobby_slots || !session_slots_out || n < 1 || n > PSX_ICE_HUB_MAX_SEATS ||
        slot_count < 2 || slot_count > 32 || (port_map_valid && !port_of_slot))
        return fail(err_out, PSX_ICE_HUB_MAP_BAD_ARGS);
    for (i = 0; i < n; ++i) {
        int found = -1;
        if (lobby_slots[i] < 0) return fail(err_out, PSX_ICE_HUB_MAP_SEAT_UNMAPPED);
        for (s = 0; s < i; ++s)
            if (lobby_slots[s] == lobby_slots[i])
                return fail(err_out, PSX_ICE_HUB_MAP_SEAT_DUPLICATE);
        if (port_map_valid) {
            for (s = 1; s < slot_count; ++s)
                if (port_of_slot[s] == lobby_slots[i]) { found = s; break; }
        } else {
            found = lobby_slots[i] + (host_spectates ? 1 : 0);
            if (found < 1 || found >= slot_count) found = -1;
        }
        if (found < 0) return fail(err_out, PSX_ICE_HUB_MAP_SEAT_UNMAPPED);
        if (used & (1u << found)) return fail(err_out, PSX_ICE_HUB_MAP_SEAT_DUPLICATE);
        used |= 1u << found;
        session_slots_out[i] = found;
    }
    /* Every non-host session slot needs its agent. */
    for (s = 1; s < slot_count; ++s)
        if (!(used & (1u << s))) return fail(err_out, PSX_ICE_HUB_MAP_SEAT_MISSING);
    return 0;
}
