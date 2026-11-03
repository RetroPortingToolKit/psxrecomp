#pragma once
/* These fixtures exercise native presentation. Decoder behavior has its own test. */
#include "gpu_fmv_hd.h"
int gpu_fmv_hd_lookup(const uint32_t *p,int w,int h,int cw,FmvHdTimeline timeline,FmvHdImage *out) {
    (void)p;(void)w;(void)h;(void)cw;(void)timeline;(void)out;return 0;
}
void gpu_fmv_hd_shutdown(void) {}
void gpu_fmv_hd_reset_tracking(void) {}
void gpu_fmv_hd_stats(uint64_t *h,uint64_t *q,uint64_t *m) {if(h)*h=0;if(q)*q=0;if(m)*m=0;}
int gpu_fmv_hd_debug_json(char *out,size_t cap) {if(out&&cap)out[0]=0;return 0;}
