/* The shared owner used by main.cpp, with the PSX guest/mod profile. */
#include <recomp_input_effects.h>
#include <stdio.h>
static int failed;
#define CHECK(c,m) do {if(!(c)){fprintf(stderr,"FAIL %s\n",m);++failed;}}while(0)
int main(void) {
    ri_haptic_profile profile={100u,0u};
    ri_haptics*c=ri_haptics_create(8,profile);
    uint16_t low=0,high=0;
    CHECK(c,"owner allocation");
    ri_haptics_values(c,0,5,0,&low,&high);CHECK(!low&&!high,"unset neutral");
    ri_haptics_set(c,0,1,150u*257u,65535u,RI_HAPTIC_FRAME32,10,8);
    ri_haptics_values(c,0,10,0,&low,&high);CHECK(low==150u*257u&&high==65535u,"mod motors");
    ri_haptics_set(c,0,0,200u*257u,0,RI_HAPTIC_HELD,0,0);
    ri_haptics_values(c,0,12,0,&low,&high);CHECK(low==200u*257u&&high==65535u,"guest maximum");
    ri_haptics_values(c,0,17,0,&low,&high);CHECK(high==65535u,"before TTL");
    ri_haptics_values(c,0,18,0,&low,&high);CHECK(!high&&low==200u*257u,"at TTL guest remains");
    ri_haptics_set(c,0,1,65535u,0,RI_HAPTIC_FRAME32,0xfffffffeu,8);
    ri_haptics_values(c,0,1,0,&low,&high);CHECK(low==65535u&&!high,"VBlank wrap");
    ri_haptics_destroy(c);
    if(failed)return 1;
    puts("shared PSX host rumble tests passed");return 0;
}
