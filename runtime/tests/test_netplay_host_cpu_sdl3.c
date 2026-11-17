#include "netplay_host_profile.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
    PSXHostCPU cpu;psx_host_cpu_detect_sdl3(&cpu);
    CHECK(cpu.architecture<PSX_HOST_ARCH_COUNT);
#if defined(PSX_SDL3) && PSX_SDL3
    CHECK(cpu.features_known==1);
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(cpu.architecture==PSX_HOST_ARCH_X64 && (cpu.features&PSX_HOST_SSE2));
#endif
#else
    CHECK(!cpu.features_known&&!cpu.features);
#endif
    uint8_t artifact[32]={1};PSXHostCapabilities c;
    CHECK(psx_host_caps_init(&c,&cpu,psx_host_compiled_backend(),0,artifact));
    CHECK(!c.available_backends && !c.offer_count);
    printf("CPU probe architecture=%u features=%llx; no automatic backend/profile offers\n",
           cpu.architecture,(unsigned long long)cpu.features);
    return 0;
}
