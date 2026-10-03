#include "mod_controller_source.h"
#include "sio.h"
#include <stdio.h>
static int mode;
static int source(PSXModControllerState *p) {
    p->buttons=0xfeff;p->lx=200;p->ly=5;
    if(mode==1)return 0;
    if(mode==2)p->rx=256;
    if(mode==3)p->analog=0;
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
    CHECK(!psx_mod_set_controller_source(PSX_MAX_PLAYERS,source));
    CHECK(!mod_controller_source_sample(0,&p));
    CHECK(psx_mod_set_controller_source(0,source));
    CHECK(mod_controller_source_sample(0,&p) && p.lx==200 && p.ly==5);
    mode=1;CHECK(mod_controller_source_sample(0,&p) && p.buttons==0xffff && p.lx==128);
    mode=2;CHECK(mod_controller_source_sample(0,&p) && p.buttons==0xffff && p.rx==128);
    mode=3;CHECK(mod_controller_source_sample(0,&p) && !p.analog && p.lx==128 && p.ly==128);
    CHECK(psx_mod_set_controller_source(0,0));
    CHECK(mod_controller_source_sample(0,&p) && p.buttons==0xffff && p.lx==128);
    CHECK(!mod_controller_source_sample(0,&p));
    CHECK(psx_mod_set_controller_source(0,source));
    mod_controller_source_reset();
    CHECK(!mod_controller_source_present(0) && !mod_controller_source_sample(0,&p));
    return 0;
}
