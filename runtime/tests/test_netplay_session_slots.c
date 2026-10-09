#include "netplay_session_slots.h"
#undef NDEBUG
#include <assert.h>

int main(void)
{
    /* Players: the lobby's player count wins; a seat past it widens. */
    assert(psx_netplay_session_slot_count(2, 2, 0, 0, 4, 4) == 2);
    assert(psx_netplay_session_slot_count(2, 2, 1, 0, 4, 4) == 2);
    assert(psx_netplay_session_slot_count(2, 4, 2, 0, 4, 4) == 3);
    assert(psx_netplay_session_slot_count(0, 0, 0, 0, 0, 4) == 2);
    assert(psx_netplay_session_slot_count(8, 8, 7, 0, 4, 4) == 4);
    /* Spectator: gallery seat 64 must not widen the count, so it matches
     * the players (2) and the relay base (2) sits above every player slot. */
    assert(psx_netplay_session_slot_count(2, 2, 64, 1, 4, 4) == 2);
    assert(psx_netplay_session_slot_count(3, 4, 65, 1, 4, 4) == 3);
    /* Host in the gallery: the cap grows by one and players shift up. */
    assert(psx_netplay_session_slot_count(3, 2, 2, 0, 4, 5) == 3);
    return 0;
}
