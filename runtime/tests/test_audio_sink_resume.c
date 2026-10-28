#ifdef NDEBUG
#undef NDEBUG
#endif
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void test_live_and_depleted_resume(void) {
 rab_bridge b;rab_config c;rab_config_defaults(&c);c.source_rate=c.host_rate=44100;c.target_ms=80;
 assert(!rab_init(&b,&c));int16_t pcm[8820],out[9600];for(int i=0;i<8820;i++)pcm[i]=12000;
 rab_push(&b,pcm,4410);rab_pull(&b,out,1323);assert(b.primed&&rab_fill_ms(&b)<80);
 uint64_t count=b.in_count;double pos=b.out_pos;rab_resume_after_sink(&b);
 assert(b.primed&&b.in_count==count&&b.out_pos==pos);rab_pull(&b,out,441);
 for(int i=0;i<441;i++)assert(out[2*i]>11000);
 rab_free(&b);
 assert(!rab_init(&b,&c));rab_push(&b,pcm,4410);rab_pull(&b,out,441);rab_resume_after_sink(&b);
 for(int i=0;i<1764;i++)pcm[i]=-12000;rab_push(&b,pcm,882);rab_pull(&b,out,4800);
 for(int i=3900;i<4450;i++)assert(abs(out[2*i]-out[2*(i-1)])<350);
 rab_pull(&b,out,441);assert(!b.primed);uint64_t under=b.stats.underrun_events;assert(under>0);
 rab_pull(&b,out,441);assert(b.stats.underrun_events==under);
 for(int i=0;i<8820;i++)pcm[i]=12000;
 rab_push(&b,pcm,882);rab_pull(&b,out,441);assert(!b.primed);
 rab_push(&b,pcm,3528);rab_pull(&b,out,441);assert(b.primed);
 rab_free(&b);
}
int main(void) {
 test_live_and_depleted_resume();
 rab_bridge b;rab_config c;rab_config_defaults(&c);c.target_ms=80;
 assert(rab_init(&b,&c)==0);
 int16_t pcm[8820];for(int i=0;i<8820;i++)pcm[i]=(int16_t)(i%251*80);
 rab_push(&b,pcm,4410);int16_t out[882];rab_pull(&b,out,441);assert(b.primed);
 uint64_t count=b.in_count;double pos=b.out_pos;float *copy=malloc(b.cap*2*sizeof(float));memcpy(copy,b.ring,b.cap*2*sizeof(float));
 rab_resume_after_sink(&b);assert(b.primed);assert(b.in_count==count && b.out_pos==pos);assert(!memcmp(copy,b.ring,b.cap*2*sizeof(float)));
 for(int i=0;i<14;i++)rab_pull(&b,out,441);
 count=b.in_count;pos=b.out_pos;rab_resume_after_sink(&b);assert(!b.primed && b.in_count==count && b.out_pos==pos);
 assert(b.corr==0 && b.corr_i==0 && b.err_lp==0);
 rab_push(&b,pcm,4410);rab_pull(&b,out,441);assert(b.primed);free(copy);rab_free(&b);
 puts("PASS resume preserves PCM/read cursor, only re-primes depleted ring, resets stale rate integrator");
}
