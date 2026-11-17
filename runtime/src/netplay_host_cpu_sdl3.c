#include "netplay_host_profile.h"
#if defined(PSX_SDL3) && PSX_SDL3
#include <SDL3/SDL_cpuinfo.h>
#endif
void psx_host_cpu_detect_sdl3(PSXHostCPU *out){
    if(!out)return;
    psx_host_cpu_unknown(out);
#if defined(PSX_SDL3) && PSX_SDL3
    out->features_known=1;
#define FEATURE(query,bit) do{if(query())out->features|=bit;}while(0)
    FEATURE(SDL_HasSSE2,PSX_HOST_SSE2);FEATURE(SDL_HasSSE3,PSX_HOST_SSE3);
    FEATURE(SDL_HasSSE41,PSX_HOST_SSE41);FEATURE(SDL_HasSSE42,PSX_HOST_SSE42);
    FEATURE(SDL_HasAVX,PSX_HOST_AVX);FEATURE(SDL_HasAVX2,PSX_HOST_AVX2);
    FEATURE(SDL_HasAVX512F,PSX_HOST_AVX512F);FEATURE(SDL_HasNEON,PSX_HOST_NEON);
    FEATURE(SDL_HasAltiVec,PSX_HOST_ALTIVEC);FEATURE(SDL_HasLSX,PSX_HOST_LSX);FEATURE(SDL_HasLASX,PSX_HOST_LASX);
#undef FEATURE
#endif
}
