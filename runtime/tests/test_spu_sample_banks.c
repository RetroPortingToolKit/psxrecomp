/* Private assets share the native voice DSP without touching hardware RAM.
 * Exercise concurrent banks, delayed KEYON, driver slot reuse and rollback. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/spu.c"
uint64_t s_frame_count;
uint64_t psx_get_cycle_count(void) {return 0;}
void audio_trace_pcm(int t,const int16_t *s,int n) {(void)t;(void)s;(void)n;}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;}
void psx_irq_raise(uint32_t b,uint32_t d) {(void)b;(void)d;}
uint32_t crc32_update(uint32_t c,const uint8_t *p,size_t n) {(void)p;(void)n;return c;}
bool spu_shadow_enabled(void) {return false;}
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *p,int n) {(void)p;(void)n;}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void block(uint8_t *p,uint8_t packed) {
    memset(p,0,32);p[1]=4;memset(p+2,packed,14);p[17]=3;memset(p+18,packed,14);
}
static void ready_voice(unsigned i) {
    voices[i].env_level=0x6000;voices[i].adsr_phase=ADSR_SUSTAIN;
    spu_regs[i*8+2]=0x1000;
}
int main(void) {
    spu_init();uint8_t a[32],b[32];block(a,0x11);block(b,0x33);
    CHECK(!spu_register_sample_bank(0,a,32));CHECK(!spu_register_sample_bank(32,a,32));
    CHECK(!spu_register_sample_bank(1,a,31));CHECK(!spu_bind_next_voice_bank(24,0));
    CHECK(!spu_bind_next_voice_bank(0,1));
    CHECK(spu_register_sample_bank(1,a,32));CHECK(spu_register_sample_bank(2,b,32));
    CHECK(spu_register_sample_bank(1,a,32));CHECK(!spu_register_sample_bank(1,b,32));
    /* Callers can release/overwrite their staging buffers after registration. */
    memset(a,0,sizeof a);memset(b,0,sizeof b);block(spu_ram,0x55);
    CHECK(spu_bind_next_voice_bank(0,1));CHECK(spu_bind_next_voice_bank(1,2));
    CHECK(!voices[0].sample_bank && voices[0].pending_bank==1);
    uint32_t n=spu_snapshot_bytes();uint8_t *snapshot=malloc(n);CHECK(snapshot);
    spu_snapshot_write(snapshot);
    CHECK(spu_bind_next_voice_bank(0,0));CHECK(spu_snapshot_read(snapshot,n));
    CHECK(voices[0].pending_bank==1 && voices[1].pending_bank==2);
    key_on(7);for(unsigned i=0;i<3;++i)ready_voice(i);
    CHECK(voices[0].sample_bank==1 && voices[1].sample_bank==2 && !voices[2].sample_bank);
    CHECK(!voices[0].pending_bank && !voices[1].pending_bank);
    /* A stage DMA can overwrite the same hardware address at any time. */
    uint8_t upload[32];block(upload,0x77);
    spu_write(0x1F801DA6u,0);
    for(unsigned at=0;at<sizeof upload;at+=4) {
        uint32_t word=(uint32_t)upload[at]|(uint32_t)upload[at+1]<<8|
            (uint32_t)upload[at+2]<<16|(uint32_t)upload[at+3]<<24;
        spu_dma_write(word);
    }
    for(unsigned i=0;i<3;++i)voice_next_sample(i);
    CHECK(voices[0].samples[0]==4096 && voices[1].samples[0]==12288 && voices[2].samples[0]==28672);
    CHECK(voices[0].repeat_addr==0 && voices[1].repeat_addr==0);
    spu_snapshot_write(snapshot);int16_t pcm[3][70];
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<70;++j)pcm[i][j]=voice_next_sample(i);
    CHECK(spu_snapshot_read(snapshot,n));
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<70;++j)CHECK(pcm[i][j]==voice_next_sample(i));
    /* KEYOFF retains the asset through release. The next ordinary KEYON
     * reuses the voice on hardware RAM; bindings cannot leak to music/SFX. */
    key_off(1);CHECK(voices[0].sample_bank==1 && voices[0].adsr_phase==ADSR_RELEASE);
    key_on(1);ready_voice(0);voice_next_sample(0);
    CHECK(!voices[0].sample_bank && voices[0].samples[0]==28672);
    /* Invalid bank addresses terminate silently instead of reading stage RAM. */
    CHECK(spu_bind_next_voice_bank(0,1));spu_regs[3]=0x100;key_on(1);ready_voice(0);
    voice_next_sample(0);CHECK(voices[0].flags==1 && voices[0].samples[0]==0);
    free(snapshot);spu_init();CHECK(!spu_bind_next_voice_bank(0,1));
    puts("SPU private sample-bank checks passed");return 0;
}
