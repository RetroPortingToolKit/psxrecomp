#include "mod_plugins.h"
#include <assert.h>
#include <stdio.h>
static int called;
static int draw(struct CPUState *cpu,void *user,uint32_t alpha) {
    (void)cpu;(void)user;(void)alpha;called++;return 1;
}
int main(void) {
    PSXModRenderPassFrame frame={0};frame.struct_size=sizeof frame;
    frame.period_vblanks=2;frame.w=384;frame.h=240;
    assert(!psx_mod_activate_render_pass_rate("old", "rate", "fps",0));
    assert(!psx_mod_render_pass_frame(NULL,&frame,draw,NULL));
    assert(!called);
    puts("retired render-pass frame API: rejected without guest callback PASS");
    return 0;
}
