#include "mod_plugins.h"
#include "mod_memory.h"
#include "mod_texture_banks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static uint32_t memory[256], used, reads, resolves;
static uint32_t forget_calls, forgot_first, forgot_count;
int g_psx_render_pass_active;
void gl_renderer_forget_texture_bank_range(uint16_t first,uint32_t count) {
    ++forget_calls;forgot_first=first;forgot_count=count;
}
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a) {
    (void)a; if(n>sizeof memory-used)return 0;
    uint32_t address=PSX_MOD_GPU_DMA_GUEST_BASE+used;used+=n;return address;
}
uint32_t psx_mod_read_word(uint32_t address) {
    uint32_t offset=(address&0x1fffffffu)-PSX_MOD_GPU_DMA_APERTURE_BASE;
    if(offset>sizeof memory-4)abort(); ++reads;return memory[offset/4];
}
static void check(int ok,const char* text){if(!ok){fprintf(stderr,"FAIL %s\n",text);exit(1);}}
static int resolve(uint16_t id){uint16_t data=0x4567;++resolves;return psx_mod_define_texture_bank(id,1,1,&data);}
static uint32_t bits(float f){uint32_t n;memcpy(&n,&f,4);return n;}
int main(void){
    check(!mod_texture_bank_batchable(1,0,0),"bank batching defaults off");
    psx_mod_set_texture_bank_batching(1);
    for(int mode=-1;mode<=4;++mode) for(int bank=0;bank<=1;++bank) for(int mask=0;mask<=1;++mask)
        check(mod_texture_bank_batchable(bank,mask,mode)==
            (bank && !mask && (mode==0 || mode==1 || mode==3)),"single-pass opt-in scope");
    psx_mod_set_texture_bank_batching(0);
    check(!mod_texture_bank_batchable(1,0,0),"bank batching disable");
    uint16_t data[4]={1,2,3,4};uint32_t w,h;
    check(!psx_mod_define_texture_bank(0,2,2,data),"bank zero reserved");
    check(!psx_mod_define_texture_bank(1,1025,2,data),"dimensions bounded");
    check(psx_mod_define_texture_bank(1,2,2,data),"define");
    check(psx_mod_define_texture_bank(1,2,2,data),"identical definition idempotent");
    data[0]=99;
    check(!psx_mod_define_texture_bank(1,2,2,data),"immutable id cannot change");
    check(mod_texture_bank_pixels(1,&w,&h)[0]==1 && w==2 && h==2,"owned deep copy");
    check(!mod_texture_bank_pixels(2,&w,&h),"missing bank explicit");
    psx_mod_set_texture_bank_resolver(resolve);
    check(mod_texture_bank_pixels(2,&w,&h)[0]==0x4567 && resolves==1,"lazy restored-bank resolver");
    check(mod_texture_bank_pixels(2,&w,&h)!=NULL && resolves==1,"resident bank no reload");
    check(!psx_mod_forget_texture_bank_range(0,1) &&
          !psx_mod_forget_texture_bank_range(2,0) &&
          !psx_mod_forget_texture_bank_range(65535,2) && !forget_calls,
          "invalid forget cannot touch GL or other banks");
    check(psx_mod_forget_texture_bank_range(2,1) && forget_calls==1 &&
          forgot_first==2 && forgot_count==1,"owned GL range invalidated");
    check(mod_texture_bank_pixels(1,&w,&h)[0]==1,"other bank remains immutable");
    check(mod_texture_bank_pixels(2,&w,&h)[0]==0x4567 && resolves==2,
          "restored missing bank rebuilt once through resolver");
    check(psx_mod_forget_texture_bank_range(1,1),"forget old timeline");
    check(psx_mod_define_texture_bank(1,2,2,data) &&
          mod_texture_bank_pixels(1,&w,&h)[0]==99,"new timeline can reuse owned ID");
    const uint32_t arena=psx_mod_alloc_texture_packet_memory(80,16);
    uint32_t words[9]={0x34000000,0,0,0x23000000,0,0,0x01000000,0,0};
    check(!mod_texture_packet_bank(0x80001004,words,9),"stock packet cannot opt in");
    check(mod_texture_packet_bank(arena+4,words,9)==0x123,"bank id only in trusted arena");
    check(!mod_texture_packet_bank(arena+48,words,9),"whole command must fit");
    check(!mod_texture_packet_bank(arena+4,words,8),"command size checked");
    words[0]=0x30000000;
    check(!mod_texture_packet_bank(arena+4,words,9),"GT3 opcode checked");
    float q[3],xy[6];
    check(!mod_texture_packet_precision(0x80001004,q,xy) && reads==0,"stock precision does not read suffix");
    memory[10]=0x48545031;
    for(unsigned i=0;i<3;++i)memory[11+i]=bits(.01f);
    for(unsigned i=0;i<6;++i)memory[14+i]=bits(15.25f+i);
    check(mod_texture_packet_precision(arena+4,q,xy) && q[1]==.01f && xy[5]==20.25f,"fractional projection and reciprocal depth");
    check(!mod_texture_packet_precision(arena+8,q,xy),"suffix bounds");
    memory[11]=bits(NAN);
    check(!mod_texture_packet_precision(arena+4,q,xy),"NaN rejected");
    memory[11]=bits(0);
    check(!mod_texture_packet_precision(arena+4,q,xy),"zero reciprocal depth rejected");
    words[0]=0x34000000;g_psx_render_pass_active=1;
    check(!mod_texture_banks_reset_session() && mod_texture_packet_bank(arena+4,words,9)==0x123 &&
          mod_texture_bank_pixels(1,&w,&h)[0]==99,
          "active render pass cannot reset session");
    g_psx_render_pass_active=0;
    check(mod_texture_packet_bank(arena+4,words,9)==0x123,"old arena registered before reset");
    check(mod_texture_banks_reset_session(),"session resets owned resources");
    check(!mod_texture_packet_bank(arena+4,words,9),"old packet registration removed");
    check(!mod_texture_bank_pixels(1,&w,&h) && !mod_texture_bank_pixels(2,&w,&h),
          "old CPU banks and lazy resolver removed");
    check(!mod_texture_bank_batchable(1,0,0),"batching policy removed");
    used=0;memset(memory,0,sizeof memory);
    for(unsigned session=0;session<20;++session) {
        check(psx_mod_alloc_texture_packet_memory(80,16)==arena,"stable arena reuse after reset");
        check(psx_mod_define_texture_bank(1,2,2,data),"immutable ID reusable in new session");
        check(mod_texture_packet_bank(arena+4,words,9)==0x123,"new arena registered");
        check(mod_texture_banks_reset_session(),"repeated session reset");used=0;
    }
    puts("texture bank checks passed");return 0;
}
