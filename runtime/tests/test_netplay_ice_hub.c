/* Host relay over ICE: transport choice and lobby-seat -> session-slot map.
 * ROM-free, socket-free. Plain checks, not assert(): Release builds define
 * NDEBUG. */
#include "netplay_ice_hub.h"

#include <stdio.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

/* psx_transport_decide(cfg_transport, hub, host, force_input_relay,
 *                      in_online_room, has_peer, ice_built) */
static void test_transport(void)
{
    /* The hub is decided by the launch alone: neither force_input_relay, an
     * online room, nor an explicit transport=ice may move it to the SFU or the
     * single-agent ICE path. transport_host is ALSO 1 for a hub launch. */
    CHECK(psx_transport_decide(0, 1, 1, 0, 1, 0, 1) == PSX_TRANSPORT_ICE_HUB);
    CHECK(psx_transport_decide(0, 1, 1, 1, 1, 1, 1) == PSX_TRANSPORT_ICE_HUB);
    CHECK(psx_transport_decide(1, 1, 1, 0, 1, 0, 1) == PSX_TRANSPORT_ICE_HUB);
    CHECK(psx_transport_decide(0, 1, 1, 0, 0, 0, 1) == PSX_TRANSPORT_ICE_HUB);
    /* Forced LAN, or a build without ICE: refuse, never fall back to a UDP
     * path the launch has no endpoint for. */
    CHECK(psx_transport_decide(2, 1, 1, 0, 1, 0, 1) == PSX_TRANSPORT_ERROR_NO_PEER);
    CHECK(psx_transport_decide(0, 1, 1, 0, 1, 0, 0) == PSX_TRANSPORT_ERROR_NO_PEER);

    /* Legacy paths are unchanged. */
    CHECK(psx_transport_decide(0, 0, 1, 0, 1, 0, 1) == PSX_TRANSPORT_LAN_UDP);  /* host relay, host binds */
    CHECK(psx_transport_decide(0, 0, 1, 0, 1, 1, 1) == PSX_TRANSPORT_LAN_UDP);  /* host relay, guest dials */
    CHECK(psx_transport_decide(0, 0, 0, 1, 1, 1, 1) == PSX_TRANSPORT_LAN_UDP);  /* SFU */
    CHECK(psx_transport_decide(0, 0, 0, 1, 1, 0, 1) == PSX_TRANSPORT_ERROR_NO_PEER); /* SFU, no endpoint */
    CHECK(psx_transport_decide(0, 0, 0, 0, 1, 0, 1) == PSX_TRANSPORT_ERROR_NO_PEER); /* online, no endpoint */
    CHECK(psx_transport_decide(2, 0, 0, 1, 1, 0, 1) == PSX_TRANSPORT_LAN_UDP);  /* forced LAN */
    CHECK(psx_transport_decide(0, 0, 0, 0, 0, 1, 1) == PSX_TRANSPORT_LAN_UDP);  /* direct IP */
    CHECK(psx_transport_decide(1, 0, 0, 0, 0, 0, 1) == PSX_TRANSPORT_SINGLE_ICE);
    CHECK(psx_transport_decide(1, 0, 0, 0, 0, 0, 0) == PSX_TRANSPORT_LAN_UDP);
}

static int map(const int *port, int valid, int slots, int hs, const int *lobby, int n,
               int *out, PsxIceHubMapError *err)
{
    return psx_ice_hub_map_seats(port, valid, slots, hs, lobby, n, out, err);
}

static void test_mapping(void)
{
    int out[PSX_ICE_HUB_MAX_SEATS];
    PsxIceHubMapError e;

    /* 2P: host seat 0, guest seat 1. */
    {
        const int port[] = { 0, 1 };
        const int lobby[] = { 1 };
        CHECK(map(port, 1, 2, 0, lobby, 1, out, &e) == 0 && out[0] == 1);
    }
    /* 3P in any arrival order: parallel output. */
    {
        const int port[] = { 0, 1, 2 };
        const int lobby[] = { 2, 1 };
        CHECK(map(port, 1, 3, 0, lobby, 2, out, &e) == 0);
        CHECK(out[0] == 2 && out[1] == 1);
    }
    /* Host holds lobby seat 2 (moved): it is still session slot 0, and the
     * guests follow in ascending seat order -- slot != seat. */
    {
        const int port[] = { 2, 0, 1 };
        const int lobby[] = { 0, 1 };
        CHECK(map(port, 1, 3, 0, lobby, 2, out, &e) == 0);
        CHECK(out[0] == 1 && out[1] == 2);
    }
    /* Host in the gallery (host_spectates): slot 0 drives no seat, players
     * start at slot 1. */
    {
        const int port[] = { -1, 0, 1 };
        const int lobby[] = { 0, 1 };
        CHECK(map(port, 1, 3, 1, lobby, 2, out, &e) == 0);
        CHECK(out[0] == 1 && out[1] == 2);
    }
    /* Sparse room (seats 0 and 3 occupied). */
    {
        const int port[] = { 0, 3 };
        const int lobby[] = { 3 };
        CHECK(map(port, 1, 2, 0, lobby, 1, out, &e) == 0 && out[0] == 1);
    }
    /* The host's own seat is never an agent seat (search starts at slot 1). */
    {
        const int port[] = { 0, 1 };
        const int lobby[] = { 0 };
        CHECK(map(port, 1, 2, 0, lobby, 1, out, &e) == -1);
        CHECK(e == PSX_ICE_HUB_MAP_SEAT_UNMAPPED);
    }
    /* A session slot with no agent is a smaller room: refused. */
    {
        const int port[] = { 0, 1, 2 };
        const int lobby[] = { 1 };
        CHECK(map(port, 1, 3, 0, lobby, 1, out, &e) == -1);
        CHECK(e == PSX_ICE_HUB_MAP_SEAT_MISSING);
    }
    /* A connected seat the launch does not seat. */
    {
        const int port[] = { 0, 1 };
        const int lobby[] = { 5 };
        CHECK(map(port, 1, 2, 0, lobby, 1, out, &e) == -1);
        CHECK(e == PSX_ICE_HUB_MAP_SEAT_UNMAPPED);
    }
    /* Two agents for one seat. */
    {
        const int port[] = { 0, 1, 2 };
        const int lobby[] = { 1, 1 };
        CHECK(map(port, 1, 3, 0, lobby, 2, out, &e) == -1);
        CHECK(e == PSX_ICE_HUB_MAP_SEAT_DUPLICATE);
    }
    /* Bad arguments never write a mapping the caller could adopt. */
    {
        const int lobby[] = { 1 };
        CHECK(map(NULL, 1, 2, 0, lobby, 1, out, &e) == -1);
        CHECK(e == PSX_ICE_HUB_MAP_BAD_ARGS);
        CHECK(map(NULL, 0, 2, 0, lobby, 0, out, &e) == -1);
        CHECK(map(NULL, 0, 1, 0, lobby, 1, out, &e) == -1);
    }
    /* No table: identity, or seat + 1 with the host in the gallery. */
    {
        const int lobby[] = { 1, 2 };
        CHECK(map(NULL, 0, 3, 0, lobby, 2, out, &e) == 0);
        CHECK(out[0] == 1 && out[1] == 2);
    }
    {
        const int lobby[] = { 0, 1 };
        CHECK(map(NULL, 0, 3, 1, lobby, 2, out, &e) == 0);
        CHECK(out[0] == 1 && out[1] == 2);
    }
    CHECK(psx_ice_hub_map_error_text(PSX_ICE_HUB_MAP_SEAT_MISSING) != NULL);
}

int main(void)
{
    test_transport();
    test_mapping();
    if (failures) {
        fprintf(stderr, "netplay_ice_hub_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("netplay_ice_hub_test: ok\n");
    return 0;
}
