#include "psx_lobby_launch_transport.h"
#undef NDEBUG
#include <assert.h>

int main(void)
{
    /* ICE launch between two peers behind one NAT: same public endpoint on
     * both sides must stay ICE, not turn into a relay to ourselves. */
    assert(psx_lobby_launch_transport("ice", "", 0, "136.25.29.198:7778",
                                      "136.25.29.198:7778", "") == PSX_LAUNCH_ICE);
    assert(psx_lobby_launch_transport("ice", "", 0, "1.2.3.4:7777", "5.6.7.8:7777", "")
           == PSX_LAUNCH_ICE);
    /* Host relay is unchanged, even with equal endpoints. */
    assert(psx_lobby_launch_transport("host", "", 0, "1.2.3.4:7777", "1.2.3.4:7777", "")
           == PSX_LAUNCH_HOST);
    /* Legacy server relay: explicit relay_endpoint, caps, or equal rewritten
     * endpoints with no transport field. */
    assert(psx_lobby_launch_transport("", "9.9.9.9:8777", 0, "", "", "") == PSX_LAUNCH_RELAY);
    assert(psx_lobby_launch_transport("", "", 1, "1.2.3.4:7777", "5.6.7.8:7777", "")
           == PSX_LAUNCH_RELAY);
    assert(psx_lobby_launch_transport("", "", 0, "9.9.9.9:8777", "9.9.9.9:8777", "")
           == PSX_LAUNCH_RELAY);
    /* ...but not our own bind echoed back, and not unusable ports. */
    assert(psx_lobby_launch_transport("", "", 0, "1.2.3.4:7777", "1.2.3.4:7777",
                                      "1.2.3.4:7777") == PSX_LAUNCH_DIRECT);
    assert(psx_lobby_launch_transport("", "", 0, "1.2.3.4:0", "1.2.3.4:0", "")
           == PSX_LAUNCH_DIRECT);
    assert(psx_lobby_launch_transport("", "", 0, "1.2.3.4:7777", "5.6.7.8:7777", "")
           == PSX_LAUNCH_DIRECT);
    return 0;
}
