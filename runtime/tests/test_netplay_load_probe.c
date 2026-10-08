#include "netplay_load_probe.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void)
{
    PsxNetplayLoadProbe probe;
    const uint32_t slot = 10u;
    const uint32_t size = 1456939u;
    const uint32_t crc = 0x89abcdefu;

    psx_netplay_load_probe_clear(&probe);
    CHECK(!psx_netplay_load_probe_can_reuse(&probe, (int)slot, size, crc,
                                            size, crc),
          "unmatched peer applies authoritative blob");

    psx_netplay_load_probe_record(&probe, (int)slot, size, crc);
    CHECK(!psx_netplay_load_probe_can_reuse(&probe, (int)slot, size, crc,
                                            size, crc),
          "matching probe must finish local apply before reuse");
    psx_netplay_load_probe_mark_applied(&probe, (int)slot);
    CHECK(psx_netplay_load_probe_can_reuse(&probe, (int)slot, size, crc,
                                           size, crc),
          "completed matching peer verifies and reuses identical blob");

    CHECK(!psx_netplay_load_probe_can_reuse(&probe, (int)slot, size, crc ^ 1u,
                                            size, crc),
          "different authoritative CRC still applies host blob");
    CHECK(!psx_netplay_load_probe_can_reuse(&probe, (int)slot, size - 1u, crc,
                                            size, crc),
          "different authoritative size still applies host blob");
    CHECK(!psx_netplay_load_probe_can_reuse(&probe, (int)slot + 1, size, crc,
                                            size, crc),
          "different slot still applies host blob");
    CHECK(!psx_netplay_load_probe_can_reuse(&probe, (int)slot, size, crc,
                                            size, crc ^ 1u),
          "changed local slot still applies host blob");
    CHECK(!psx_netplay_load_probe_can_reuse(&probe, (int)slot, size, crc,
                                            size - 1u, crc),
          "changed local slot size still applies host blob");

    psx_netplay_load_probe_clear(&probe);
    CHECK(!probe.valid && !probe.local_applied,
          "new load clears prior match and apply receipt");
    return failures ? 1 : 0;
}
