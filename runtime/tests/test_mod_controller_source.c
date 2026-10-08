#include "mod_controller_source.h"
#include "sio.h"
#include <stdio.h>
static int mode;
static int calls;
static int source(PSXModControllerState *p) {
    calls++;
    p->buttons=0xfeff;p->lx=200;p->ly=5;
    if(mode==1)return 0;
    if(mode==2)p->rx=256;
    if(mode==3)p->analog=0;
    if(mode==4)p->struct_size=1;
    return 1;
}
/* Evaluate side effects and failures even when NDEBUG is defined. */
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)
int main(void) {
    PSXModControllerState p;
    int rel = -1;
    CHECK(!psx_mod_set_controller_source(PSX_MAX_PLAYERS,source));
    CHECK(!mod_controller_source_sample(0,&p,&rel) && rel==0);
    CHECK(psx_mod_set_controller_source(0,source));
    CHECK(mod_controller_source_sample(0,&p,&rel) && p.lx==200 && p.ly==5 && !rel);
    mod_controller_source_begin_frame();
    mode=1;CHECK(mod_controller_source_sample(0,&p,NULL) && p.buttons==0xffff && p.lx==128);
    mod_controller_source_begin_frame();
    mode=2;CHECK(mod_controller_source_sample(0,&p,NULL) && p.buttons==0xffff && p.rx==128);
    mod_controller_source_begin_frame();
    mode=3;CHECK(mod_controller_source_sample(0,&p,NULL) && !p.analog && p.lx==128 && p.ly==128);
    /* Wrong struct_size (ABI mismatch) is neutral (and logged to stderr). */
    mod_controller_source_begin_frame();
    mode=4;CHECK(mod_controller_source_sample(0,&p,NULL) && p.buttons==0xffff &&
                 p.struct_size==sizeof p && p.lx==128);
    /* Once per frame: repeated samples reuse the first result. */
    mode=0;mod_controller_source_begin_frame();calls=0;
    CHECK(mod_controller_source_sample(0,&p,NULL) && p.lx==200);
    mode=1; /* a second callback invocation would now decline */
    CHECK(mod_controller_source_sample(0,&p,NULL) && p.lx==200 && calls==1);
    mode=0;mod_controller_source_begin_frame();
    CHECK(mod_controller_source_sample(0,&p,NULL) && calls==2);
    /* Detach: one neutral release frame, shared by both passes of that frame. */
    CHECK(psx_mod_set_controller_source(0,0));
    CHECK(mod_controller_source_sample(0,&p,&rel) && rel==1 && p.buttons==0xffff && p.lx==128);
    CHECK(mod_controller_source_sample(0,&p,&rel) && rel==1);
    mod_controller_source_begin_frame();
    CHECK(!mod_controller_source_sample(0,&p,&rel) && rel==0);
    /* Reset on a held source still delivers the neutral release, for every
     * port that had a source. */
    CHECK(psx_mod_set_controller_source(0,source));
    CHECK(psx_mod_set_controller_source(1,source));
    mod_controller_source_begin_frame();
    mode=0;CHECK(mod_controller_source_sample(0,&p,NULL) && p.buttons==0xfeff);
    mod_controller_source_reset();
    CHECK(!mod_controller_source_present(0) && !mod_controller_source_present(1));
    CHECK(mod_controller_source_sample(0,&p,&rel) && rel==1 && p.buttons==0xffff && p.lx==128 && p.ly==128);
    CHECK(mod_controller_source_sample(1,&p,&rel) && rel==1 && p.buttons==0xffff);
    mod_controller_source_begin_frame();
    CHECK(!mod_controller_source_sample(0,&p,&rel) && !mod_controller_source_sample(1,&p,&rel));
    return 0;
}
