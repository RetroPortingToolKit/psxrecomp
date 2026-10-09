#include "netplay_peer_frontier.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: %s\n", msg);                                         \
            failures++;                                                        \
        } else {                                                               \
            printf("ok:   %s\n", msg);                                         \
        }                                                                      \
    } while (0)

int main(void)
{
    NetplayPeerFrontier f;
    const uint32_t two = 1u << 1;                       /* host + seat 1 */
    const uint32_t three = (1u << 1) | (1u << 2);       /* host + seats 1, 2 */
    const uint32_t four = three | (1u << 3);

    netplay_peer_frontier_reset(&f);
    CHECK(netplay_peer_frontier_min(&f, three) == 0u, "nothing advertised -> 0");

    netplay_peer_frontier_note(&f, 1, 752u);
    CHECK(netplay_peer_frontier_min(&f, two) == 752u, "two seats: the single peer's frontier");
    CHECK(netplay_peer_frontier_min(&f, three) == 0u,
          "three seats: one advert does not speak for the silent seat");

    /* The 3-seat WAN failure: host had 2400 from the fast peer only. */
    netplay_peer_frontier_reset(&f);
    netplay_peer_frontier_note(&f, 1, 2344u);
    netplay_peer_frontier_note(&f, 2, 2319u);
    netplay_peer_frontier_note(&f, 1, 2400u);
    CHECK(netplay_peer_frontier_min(&f, three) == 2319u, "slowest follower bounds the load");

    netplay_peer_frontier_note(&f, 2, 2300u);
    CHECK(netplay_peer_frontier_min(&f, three) == 2319u, "reordered older advert never lowers");

    netplay_peer_frontier_note(&f, 3, 5000u);
    CHECK(netplay_peer_frontier_min(&f, four) == 2319u, "four seats: still the slowest");

    netplay_peer_frontier_clamp(&f, 2320u);
    CHECK(f.through[1] == 2320u && f.through[2] == 2319u && f.through[3] == 2320u,
          "clamp lowers every seat above the tick, leaves the rest");

    netplay_peer_frontier_note(&f, -1, 9u);
    netplay_peer_frontier_note(&f, 99, 9u);
    CHECK(netplay_peer_frontier_min(&f, four) == 2319u, "out-of-range seats ignored");
    CHECK(netplay_peer_frontier_min(&f, 0u) == 0u, "no peer seats -> 0");

    CHECK(!netplay_fork_cap_retired(0u, 3136u), "no cap: nothing to retire");
    CHECK(!netplay_fork_cap_retired(1928u, 1920u), "agreed below the cap keeps it");
    CHECK(netplay_fork_cap_retired(1928u, 1928u), "agreed at the cap retires it");
    CHECK(netplay_fork_cap_retired(1928u, 3136u), "agreed past the cap retires it (4-seat race)");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
