#include "netplay_lan_rtt.h"
#include <assert.h>

int main(void)
{
    NetplayLanRtt peer = {0};
    netplay_lan_rtt_bind(&peer, 17);
    assert(peer.latency_ms == -1 && netplay_lan_rtt_due(&peer, 0));
    netplay_lan_rtt_sent(&peer, 100);
    assert(!netplay_lan_rtt_due(&peer, 2599));
    assert(!netplay_lan_rtt_reply(&peer, 18, 100, 160));
    assert(!netplay_lan_rtt_reply(&peer, 17, 99, 160));
    assert(netplay_lan_rtt_reply(&peer, 17, 100, 160));
    assert(peer.latency_ms == 60);
    assert(!netplay_lan_rtt_reply(&peer, 17, 100, 180));
    assert(netplay_lan_rtt_due(&peer, 2600));
    netplay_lan_rtt_sent(&peer, 2600);
    assert(netplay_lan_rtt_reply(&peer, 17, 2600, 2620));
    assert(peer.latency_ms == 60);
    /* An occupant's periodic JOIN must not discard the pending measurement. */
    netplay_lan_rtt_sent(&peer, 5100);
    netplay_lan_rtt_bind(&peer, 17);
    assert(netplay_lan_rtt_reply(&peer, 17, 5100, 5180));
    assert(peer.latency_ms == 80);
    netplay_lan_rtt_sent(&peer, 7600);
    netplay_lan_rtt_bind(&peer, 18);
    assert(peer.latency_ms == -1 && netplay_lan_rtt_due(&peer, 7601));
    assert(!netplay_lan_rtt_reply(&peer, 17, 7600, 7680));
    /* Millisecond wrap is ordinary subtraction, not a 49-day timeout. */
    netplay_lan_rtt_sent(&peer, UINT32_MAX - 20u);
    assert(netplay_lan_rtt_reply(&peer, 18, UINT32_MAX - 20u, 39));
    assert(peer.latency_ms == 60);
    netplay_lan_rtt_sent(&peer, 100);
    assert(!netplay_lan_rtt_reply(&peer, 18, 100, 60101));
    netplay_lan_rtt_bind(&peer, 0);
    assert(peer.latency_ms == -1 && !netplay_lan_rtt_due(&peer, 90000));
    return 0;
}
