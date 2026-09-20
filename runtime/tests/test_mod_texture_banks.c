#include "mod_plugins.h"
#include "mod_memory.h"
#include "mod_texture_banks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static uint32_t memory[256], used, reads, resolves;
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
    puts("texture bank checks passed");return 0;
}
