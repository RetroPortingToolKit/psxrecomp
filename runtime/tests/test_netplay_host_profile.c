#include "netplay_host_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint8_t artifact[32]={1},session[32]={2};
static PSXHostProfileRule rules[3];
static PSXHostProfileCatalog catalog={rules,3};
static PSXHostCapabilities peers[2];
static PSXHostMatch match;
static void rule(unsigned at,const char *name,unsigned priority,unsigned flags){
    PSXHostProfileRule *r=rules+at;memset(r,0,sizeof *r);strcpy(r->profile,name);
    r->revision=1;r->priority=priority;r->flags=flags;r->qualification[0]=(uint8_t)(at+1);
    r->implementation_count=1;r->implementations[0].backend=PSX_HOST_BACKEND_ENHANCED;
    r->implementations[0].architectures=1u<<PSX_HOST_ARCH_X64;
}
static void setup(void){
    rule(0,"qualified-avx2",20,0);rules[0].implementations[0].required_features=PSX_HOST_AVX2;
    rule(1,"qualified-portable",10,PSX_HOST_PROFILE_PORTABLE);
    rules[1].implementations[0].architectures=(1u<<PSX_HOST_ARCH_COUNT)-1;
    rules[1].implementation_count=2;rules[1].implementations[1]=rules[1].implementations[0];
    rules[1].implementations[1].backend=PSX_HOST_BACKEND_REFERENCE;
    rule(2,"qualified-sse2",20,0);rules[2].implementations[0].required_features=PSX_HOST_SSE2;
    PSXHostCPU cpu={PSX_HOST_ARCH_X64,1,PSX_HOST_SSE2|PSX_HOST_AVX|PSX_HOST_AVX2};
    for(unsigned i=0;i<2;++i)CHECK(psx_host_caps_init(peers+i,&cpu,3,3,artifact));
    psx_host_match_init(&match);
}
static void offer_both(unsigned r,unsigned backend){for(unsigned i=0;i<2;++i)CHECK(psx_host_caps_offer(peers+i,rules+r,backend));}
static void selection(void){
    setup();offer_both(1,PSX_HOST_BACKEND_REFERENCE);offer_both(2,PSX_HOST_BACKEND_ENHANCED);offer_both(0,PSX_HOST_BACKEND_ENHANCED);
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_OK);
    CHECK(!strcmp(match.selected.profile,"qualified-avx2")); /* Stable lexical tie. */
    CHECK(!psx_host_match_ready(&match));PSXHostMatch sealed=match;
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_LOCKED && !memcmp(&match,&sealed,sizeof match));
    CHECK(psx_host_match_confirm(&match,0,match.selected.identity));CHECK(!psx_host_match_ready(&match));
    CHECK(psx_host_match_confirm(&match,0,match.selected.identity));CHECK(!psx_host_match_ready(&match));
    CHECK(psx_host_match_confirm(&match,1,match.selected.identity));CHECK(psx_host_match_ready(&match));
    uint8_t selected[32];memcpy(selected,match.selected.identity,32);
    PSXHostCapabilities swap=peers[0];peers[0]=peers[1];peers[1]=swap;
    PSXHostProfileRule r=rules[0];rules[0]=rules[2];rules[2]=r;
    psx_host_match_init(&match);CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==1);
    CHECK(!memcmp(selected,match.selected.identity,32));
    uint8_t wrong[32]={9};CHECK(!psx_host_match_confirm(&match,0,wrong));CHECK(!psx_host_match_ready(&match));
    CHECK(!psx_host_match_confirm(&match,1,match.selected.identity));
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_LOCKED);
}
static void asymmetry(void){
    setup();peers[1].cpu.features=PSX_HOST_SSE2;peers[1].available_backends=PSX_HOST_BACKEND_REFERENCE;
    CHECK(psx_host_caps_offer(peers,rules,PSX_HOST_BACKEND_ENHANCED));
    CHECK(!psx_host_caps_offer(peers+1,rules,PSX_HOST_BACKEND_ENHANCED));
    CHECK(psx_host_caps_offer(peers,rules+1,PSX_HOST_BACKEND_ENHANCED));
    CHECK(psx_host_caps_offer(peers+1,rules+1,PSX_HOST_BACKEND_REFERENCE));
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==1);
    CHECK(!strcmp(match.selected.profile,"qualified-portable"));
    setup();peers[1].cpu.features=PSX_HOST_SSE2;CHECK(psx_host_caps_offer(peers,rules,2));
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_NO_COMMON && !match.sealed);
    setup();peers[1].cpu.architecture=PSX_HOST_ARCH_ARM64;peers[1].cpu.features=PSX_HOST_NEON;
    offer_both(1,PSX_HOST_BACKEND_REFERENCE);CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==1);
    setup();peers[1].cpu.features_known=0;peers[1].cpu.features=0;
    CHECK(!psx_host_caps_offer(peers+1,rules,2));offer_both(1,1);
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==1);
    setup();offer_both(1,1);peers[1].build[0]^=2;
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_BUILD_MISMATCH);
    setup();peers[1].available_backends=0;CHECK(!psx_host_caps_offer(peers+1,rules+1,1));
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_NO_COMMON);
}
static void malformed(void){
    uint8_t wire[PSX_HOST_CAPS_MAX_BYTES+1],bad[sizeof wire];PSXHostCapabilities out;
    setup();offer_both(0,2);offer_both(1,1);size_t bytes=psx_host_caps_encode(peers,wire,sizeof wire);
    CHECK(bytes==200 && psx_host_caps_decode(&out,wire,bytes));CHECK(psx_host_caps_validate(&out,&catalog));
    for(size_t i=0;i<bytes;++i){memset(&out,0xA5,sizeof out);CHECK(!psx_host_caps_decode(&out,wire,i));
        PSXHostCapabilities zero={0};CHECK(!memcmp(&out,&zero,sizeof out));}
    CHECK(!psx_host_caps_decode(&out,wire,bytes+1));CHECK(!psx_host_caps_encode(peers,wire,bytes-1));
    memcpy(bad,wire,bytes);bad[4]=2;CHECK(!psx_host_caps_decode(&out,bad,bytes));
    memcpy(bad,wire,bytes);bad[7]=255;CHECK(!psx_host_caps_decode(&out,bad,bytes));
    memcpy(bad,wire,bytes);memcpy(bad+128,bad+56,32);CHECK(!psx_host_caps_decode(&out,bad,bytes));
    memcpy(bad,wire,bytes);bad[56+31]='x';CHECK(!psx_host_caps_decode(&out,bad,bytes));
    memcpy(bad,wire,bytes);bad[20]=4;CHECK(!psx_host_caps_decode(&out,bad,bytes));
    peers[0].offers[0].qualification[0]^=128;CHECK(!psx_host_caps_validate(peers,&catalog));
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_OK);
    CHECK(!strcmp(match.selected.profile,"qualified-portable"));
    setup();offer_both(0,2);memset(peers[1].offers[0].profile,0,32);strcpy(peers[1].offers[0].profile,"unqualified");
    CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_NO_COMMON);
    CHECK(!psx_host_caps_validate(peers+1,&catalog)); /* Local registration stays strict. */
    offer_both(1,1);CHECK(psx_host_match_select(&match,peers,2,&catalog,session)==PSX_HOST_OK);
    CHECK(!strcmp(match.selected.profile,"qualified-portable"));
    setup();CHECK(!psx_host_caps_offer(peers,rules,1));CHECK(psx_host_caps_offer(peers,rules,2));
    CHECK(!psx_host_caps_offer(peers,rules,2));
    rules[1].implementations[0].required_features=PSX_HOST_AVX2;CHECK(!psx_host_caps_offer(peers,rules+1,2));
    setup();rules[1]=rules[0];CHECK(!psx_host_caps_validate(peers,&catalog));
}
static void peer_bounds(void){
    setup();offer_both(1,1);PSXHostCapabilities all[PSX_HOST_MAX_PEERS];
    for(unsigned i=0;i<PSX_HOST_MAX_PEERS;++i)all[i]=peers[0];
    CHECK(psx_host_match_select(&match,all,PSX_HOST_MAX_PEERS,&catalog,session)==1);
    for(unsigned i=0;i<PSX_HOST_MAX_PEERS;++i){CHECK(!psx_host_match_ready(&match));CHECK(psx_host_match_confirm(&match,i,match.selected.identity));}
    CHECK(psx_host_match_ready(&match));psx_host_match_init(&match);
    CHECK(psx_host_match_select(&match,all,PSX_HOST_MAX_PEERS+1,&catalog,session)==PSX_HOST_INVALID);
    CHECK(psx_host_match_select(&match,all,1,&catalog,session)==PSX_HOST_INVALID);
    match.sealed=1;match.peer_count=32;CHECK(!psx_host_match_ready(&match));CHECK(!psx_host_match_confirm(&match,0,artifact));
}
int main(void){selection();asymmetry();malformed();peer_bounds();puts("qualified host profile codec, common fallback and immutable confirmation PASS");return 0;}
