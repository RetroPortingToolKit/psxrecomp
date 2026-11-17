#ifdef NDEBUG
#undef NDEBUG
#endif
#include "latency_ring.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint64_t latency_test_ticks=1000;
int main(void) {
    assert(latency_ring_current_frame()==UINT64_MAX);
    latency_ring_frame_begin();const uint64_t first=latency_ring_current_frame();
    latency_test_ticks=1100;latency_ring_frame_begin();
    // Frame 0 swaps after frame 1's input. It must still belong to frame 0.
    latency_ring_replay_frame(first);latency_test_ticks=1200;latency_ring_mark(LAT_SWAP_BEGIN);
    latency_test_ticks=1250;latency_ring_mark(LAT_SWAP_END);
    latency_ring_replay_frame(UINT64_MAX);
    latency_test_ticks=1260;latency_ring_mark(LAT_SWAP_BEGIN);
    char json[4096];latency_ring_summary_json(json,sizeof json,2);
    assert(strstr(json,"\"input_to_swap\":{\"mean_us\":180.0"));
    latency_ring_dump_json(json,sizeof json,2);
    assert(strstr(json,"\"swap_begin\":200.0,\"swap_end\":250.0"));
    // The paced mark can precede a deliberately late input re-sample.
    latency_ring_mark_frame(latency_ring_current_frame(),LAT_PACED);
    latency_test_ticks=1270;latency_ring_restamp_input();
    latency_ring_dump_json(json,sizeof json,1);
    assert(strstr(json,"\"paced\":-10.0"));
    for(unsigned i=0;i<4096;++i) {++latency_test_ticks;latency_ring_frame_begin();}
    assert(!latency_ring_mark_frame(first,LAT_SWAP_BEGIN));
    puts("latency_ring: async frame ownership, late input and stale slots PASS");
    return 0;
}
