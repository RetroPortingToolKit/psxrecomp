#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_catchup.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 PsxAudioCatchup s={0};
 assert(!psx_audio_catchup_update(&s,1,0,0,90,100,20));
 assert(!psx_audio_catchup_update(&s,1,0,0,200,100,20)); /* late, but buffer absorbed it */
 assert(!psx_audio_catchup_update(&s,1,0,1,105,100,20)); /* small jitter, not catch-up */
 assert(psx_audio_catchup_update(&s,1,0,2,200,100,20));
 assert(psx_audio_catchup_update(&s,1,0,2,210,205,20)); /* retain until caught up */
 assert(!psx_audio_catchup_update(&s,1,0,2,220,230,20));
 assert(!psx_audio_catchup_update(&s,1,1,1000,400,300,20)); /* intended FF silence */
 assert(!psx_audio_catchup_update(&s,1,0,1000,410,300,20));
 assert(psx_audio_catchup_update(&s,1,0,1001,420,300,20));
 assert(!psx_audio_catchup_update(&s,0,0,1002,430,300,20));
 assert(psx_audio_catchup_update(&s,1,0,1003,435,300,20)); /* old stream active */
 assert(!psx_audio_catchup_update(&s,1,0,0,440,300,20)); /* new stream counter */
 assert(!psx_audio_catchup_update(&s,1,0,1,500,0,20));
 assert(!psx_audio_catchup_update(&s,1,0,2,500,100,0));
 assert(psx_audio_catchup_update(&s,1,0,3,500,480,17)); /* NTSC-sized period */
 /* A real dry callback can arm recovery, but an empty/healthy-low queue
  * must receive fresh PCM. Only accelerated excess may be discarded. */
 assert(!psx_audio_catchup_discard(1,0.0,80.0));
 assert(!psx_audio_catchup_discard(1,79.9,80.0));
 assert(psx_audio_catchup_discard(1,80.0,80.0));
 assert(psx_audio_catchup_discard(1,200.0,80.0));
 assert(!psx_audio_catchup_discard(0,200.0,80.0));
 assert(!psx_audio_catchup_discard(1,200.0,0.0));
 puts("PASS underrun + full late-period gate, hysteresis, absorbed stalls, FF/OFF/reset and rate-independent period");
}
