#include "ws_scene_latch.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
int main(void) {
 WsWorldScaleWitness s;ws_world_scale_reset(&s,100);
 assert(ws_backdrop_keeps_native_43(1,&s,100,1));
 for(unsigned f=100;f<120;f++)ws_world_scale_note(&s,f,424);
 assert(ws_backdrop_keeps_native_43(1,&s,119,1));
 ws_world_scale_note(&s,120,4000);
 assert(ws_backdrop_keeps_native_43(1,&s,120,1));
 ws_world_scale_note(&s,121,4000);
 assert(!ws_backdrop_keeps_native_43(1,&s,121,1));
 assert(ws_backdrop_keeps_native_43(1,&s,121,0));
 assert(!ws_backdrop_keeps_native_43(0,&s,124,0));
 assert(ws_backdrop_keeps_native_43(1,&s,124,1));
 ws_world_scale_reset(&s,121);
 assert(ws_backdrop_keeps_native_43(1,&s,121,1));
 ws_world_scale_reset(&s,0);
 ws_world_scale_note(&s,0xffffffffu,512);ws_world_scale_note(&s,0,512);
 assert(!ws_backdrop_keeps_native_43(1,&s,0,1));
 puts("PASS sustained mesh plus overhang, sparse UI, expiry, reset and wrap");
}
