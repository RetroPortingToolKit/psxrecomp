/* netplay_ice_hub.h -- host relay over ICE: the pure decisions.
 *
 * Two questions the engine must answer identically on every peer, kept free of
 * recomp-net and of the lobby client so they can be tested without a ROM, a
 * socket, or an ICE agent:
 *
 *   1. Which transport does a launch use?   psx_transport_decide
 *   2. Which SESSION slot does each connected guest agent serve?
 *                                           psx_ice_hub_map_seats
 *
 * Doctrine: a launch with transport_ice_hub binds no UDP port and dials
 * nothing. It must never fall through to the single-agent ICE path (that is a
 * different negotiation, signalled over the legacy ICE types) or to the
 * server's SFU, even when the config also carries force_input_relay or an
 * explicit transport=ice. A seat that cannot be mapped refuses the launch
 * rather than starting a smaller room.
 */
#ifndef NETPLAY_ICE_HUB_H
#define NETPLAY_ICE_HUB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PsxTransportChoice {
    PSX_TRANSPORT_ERROR_NO_PEER = -1, /* online / SFU launch with no endpoint */
    PSX_TRANSPORT_LAN_UDP       = 0,  /* bind + (hub | dial): LAN, host relay, SFU */
    PSX_TRANSPORT_SINGLE_ICE    = 1,  /* one negotiated agent (explicit ice only) */
    PSX_TRANSPORT_ICE_HUB       = 2   /* adopt the waiting room's connected agents */
} PsxTransportChoice;

/* cfg_transport: PsxNetplayConfig.transport (0 auto, 1 force ICE, 2 force LAN).
 * in_online_room: connected to the lobby and seated (a MotK online room).
 * ice_built: this build can run ICE agents.
 * An ICE hub launch under force-LAN is an error: the launch carries no
 * endpoint to bind or dial. */
PsxTransportChoice psx_transport_decide(int cfg_transport, int transport_ice_hub,
                                        int transport_host, int force_input_relay,
                                        int in_online_room, int has_peer_endpoint,
                                        int ice_built);

#define PSX_ICE_HUB_MAX_SEATS 8

/* Why a mapping was refused (for the log / the launch error). */
typedef enum PsxIceHubMapError {
    PSX_ICE_HUB_MAP_OK = 0,
    PSX_ICE_HUB_MAP_BAD_ARGS,
    PSX_ICE_HUB_MAP_SEAT_UNMAPPED,   /* a lobby seat holds no session slot */
    PSX_ICE_HUB_MAP_SEAT_DUPLICATE,  /* two agents for one lobby seat / slot */
    PSX_ICE_HUB_MAP_SEAT_MISSING     /* a non-host session slot has no agent */
} PsxIceHubMapError;

/* Map each connected guest's LOBBY seat to the SESSION slot it serves.
 *
 *   port_of_slot[s] (s in 0..slot_count-1): the lobby seat session slot s
 *     drives, -1 for none. Slot 0 is the host and is never an agent seat, so
 *     the table is searched from slot 1. This is the table the launch already
 *     carries (PsxNetplayConfig.port_of_slot): it folds in the host-in-gallery
 *     offset (host_spectates: slot 0 has port -1, players start at slot 1) and
 *     moved seats (slots follow ascending lobby seat, not lobby seat itself).
 *   port_map_valid == 0: no table; identity, or lobby seat + 1 with
 *     host_spectates (players sit one slot above their seat).
 *
 * Requires exactly slot_count - 1 distinct guest seats, every one mapped to a
 * distinct session slot in 1..slot_count-1: a session slot with no agent would
 * be a smaller room. Returns 0 and fills session_slots_out[i] (parallel to
 * lobby_slots[i]), else -1 with *err_out set. */
int psx_ice_hub_map_seats(const int *port_of_slot, int port_map_valid,
                          int slot_count, int host_spectates,
                          const int *lobby_slots, int n,
                          int *session_slots_out, PsxIceHubMapError *err_out);

const char *psx_ice_hub_map_error_text(PsxIceHubMapError e);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_ICE_HUB_H */
