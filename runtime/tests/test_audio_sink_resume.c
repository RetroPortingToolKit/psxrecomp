#ifdef NDEBUG
#undef NDEBUG
#endif
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
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
