#include "netplay_sim_pad_cache.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    PsxNetplaySimPadCache cache = {0};
    PsxNetPad predicted = {0xFFFEu, 0x80, 0x80, 0x80, 0x80, 0, 1};
    PsxNetPad sealed = {0xFFF7u, 0x81, 0x80, 0x80, 0x80, 1, 1};
    PsxNetPad out;
    memset(&out, 0x5a, sizeof(out));
    assert(!psx_netplay_sim_pad_cache_read(&cache, 2, 4, 17, &out));
    psx_netplay_sim_pad_cache_publish(&cache, 2, 17, &predicted);
    assert(psx_netplay_sim_pad_cache_read(&cache, 2, 4, 17, &out));
    assert(memcmp(&out, &predicted, sizeof(out)) == 0);
    psx_netplay_sim_pad_cache_publish(&cache, 2, 17, &sealed);
    assert(psx_netplay_sim_pad_cache_read(&cache, 2, 4, 17, &out));
    assert(memcmp(&out, &sealed, sizeof(out)) == 0);
    assert(!psx_netplay_sim_pad_cache_read(&cache, 2, 4, 18, &out));
    assert(!psx_netplay_sim_pad_cache_read(&cache, 4, 4, 17, &out));
    psx_netplay_sim_pad_cache_reset(&cache);
    assert(!psx_netplay_sim_pad_cache_read(&cache, 2, 4, 17, &out));
    return 0;
}
