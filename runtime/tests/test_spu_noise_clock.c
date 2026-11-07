/* Dynamic SPUCNT frequency changes, qualified against reference SPU output.
 * The reference's measured constant 3/4 output gain is removed as a comparison
 * factor; the one-LSB tolerance covers that final integer gain rounding.
 * Golden PCM comes from black-box execution, not emulator implementation code. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/spu.c"
uint64_t s_frame_count;
uint64_t psx_get_cycle_count(void) {return 0;}
void audio_trace_pcm(int t,const int16_t *p,int n){(void)t;(void)p;(void)n;}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b){(void)k;(void)a;(void)b;}
void psx_irq_raise(uint32_t b,uint32_t d){(void)b;(void)d;}
uint32_t crc32_update(uint32_t c,const uint8_t *p,size_t n){(void)p;(void)n;return c;}
bool spu_shadow_enabled(void){return false;}
void spu_shadow_reset(void){}
void spu_shadow_process(int16_t *p,int n){(void)p;(void)n;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

static void setup(void) {
    spu_init();
    spu_write(0x1F801DAAu,0xC000);spu_write(0x1F801DACu,4);
    spu_write(0x1F801D80u,0x3FFF);spu_write(0x1F801D82u,0x3FFF);
    spu_write(0x1F801D94u,2);spu_write(0x1F801C10u,0x3FFF);spu_write(0x1F801C12u,0x3FFF);
    spu_write(0x1F801C14u,0);spu_write(0x1F801C16u,0x400);
    spu_write(0x1F801C18u,0xF);spu_write(0x1F801C1Au,0);spu_write(0x1F801D88u,2);
    int16_t warm[80];spu_render_block(warm,40);
}

static void test_slow_to_fast_has_no_stale_countdown(void) {
    setup();spu_write(0x1F801DAAu,0xFF00);
    int16_t out[128];spu_render_block(out,64);
    unsigned first=64;
    for(unsigned i=0;i<64;i++)if(out[i*2]){first=i;break;}
    CHECK(first==3); /* old reload-scaled phase held silence for 18,703 samples */
}

static void test_all_clock_controls_against_reference(void) {
    static const int16_t expected[]={
        0,-19950,-11533,12811,-21495,-21973,4529,-9420,-1084,15809,8860,-4224,
        8250,9238,3638,-22223,-21419,1895,21567,24239,18293,-20330,10320,154,
        698,-6514,7657,-17725,-4116,2445,-4256,19557,21491,22736,20588,8394,
        8649,-5227,-24493,-6747,-15810,23921,-14622,1349};
    setup();unsigned next=126,at=0;int16_t out[2];uint64_t controls=0;
    for(unsigned i=0;i<22050;i++) {
        if(i%127==0) {
            unsigned control=((i/127)*37+5)&63;
            controls|=(uint64_t)1<<control;
            spu_write(0x1F801DAAu,0xC000|(control<<8));
        }
        spu_render_block(out,1);
        if(i==next) {
            CHECK(at<sizeof(expected)/sizeof(expected[0]));
            int scaled=(int)out[0]*3/4;
            CHECK(abs(scaled-expected[at])<=1);
            at++;next+=509;
        }
    }
    CHECK(at==sizeof(expected)/sizeof(expected[0]));
    CHECK(controls==UINT64_MAX);
}

static void test_phase_restore_and_audio_block_partition(void) {
    setup();spu_write(0x1F801DAAu,0xE600);
    int16_t warm[254];spu_render_block(warm,127);
    uint32_t n=spu_snapshot_bytes();uint8_t *snapshot=malloc(n);CHECK(snapshot);
    spu_snapshot_write(snapshot);
    int16_t whole[514],split[514];
    spu_render_block(whole,257);
    CHECK(spu_snapshot_read(snapshot,n));
    for(unsigned i=0;i<257;i++)spu_render_block(split+i*2,1);
    CHECK(!memcmp(whole,split,sizeof whole));free(snapshot);
}

int main(void) {
    test_slow_to_fast_has_no_stale_countdown();
    test_all_clock_controls_against_reference();
    test_phase_restore_and_audio_block_partition();
    puts("SPU noise clock reference checks passed");return 0;
}
