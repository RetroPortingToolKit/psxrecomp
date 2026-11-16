/* Private assets share the native voice DSP without touching hardware RAM.
 * Exercise concurrent banks, delayed KEYON, driver slot reuse and rollback. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/spu.c"
uint64_t s_frame_count;
static uint64_t guest_cycles;
uint64_t psx_get_cycle_count(void) {return guest_cycles;}
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
/* Released SDK private inspection contract, without importing unrelated
 * sample-bank debug endpoints. Owed guest time must remain owed after reads. */
static void test_private_inspection(void) {
    spu_init();uint8_t asset[32];block(asset,0x22);
    CHECK(spu_register_sample_bank(1,asset,sizeof asset));
    const uint16_t regs[8]={0x1000,0x2000,0x0478,2,0x80FF,0x1FC0,0,4};
    CHECK(spu_private_voice_play(23,1,regs,3));
    const uint32_t n=spu_snapshot_bytes();uint8_t *before=malloc(n),*after=malloc(n);
    CHECK(before && after);spu_snapshot_write(before);
    const uint64_t events=spu_event_total(),renders=render_frames,clock=s_clock;
    const uint32_t epoch=spu_sample_bank_epoch(),keyons=key_on_count,queued=spu_output_available();
    guest_cycles=1000000;
    SpuVoiceState state;
    CHECK(spu_get_private_voice_state(23,&state) && state.active && state.sample_bank==1 && !state.pending_bank && state.mode==3);
    CHECK(state.pitch==regs[2] && state.start_lo==regs[3] && state.loop_lo==regs[7]);
    CHECK(state.adsr_lo==regs[4] && state.adsr_hi==regs[5] && state.cur_addr==16);
    CHECK(state.sample_idx==28 && state.env_level==0 && state.adsr_phase==ADSR_ATTACK);
    CHECK(spu_get_private_voice_state(0,&state) && !state.active && !state.sample_bank);
    CHECK(!spu_get_private_voice_state(24,&state) && !state.active);
    CHECK(!spu_get_private_voice_state(UINT32_MAX,&state) && !spu_get_private_voice_state(0,NULL));
    spu_get_voice_state(47,&state);CHECK(!state.active && !state.sample_bank);
    CHECK(events==spu_event_total() && renders==render_frames && clock==s_clock);
    CHECK(epoch==spu_sample_bank_epoch() && keyons==key_on_count && queued==spu_output_available());
    guest_cycles=0;spu_snapshot_write(after);CHECK(!memcmp(before,after,n));
    free(before);free(after);spu_init();
    CHECK(spu_get_private_voice_state(23,&state) && !state.active && !private_enabled);
}
static void test_private_owners(void) {
    const uint64_t a=UINT64_C(0x4D4D583650320001),b=UINT64_C(0x464F524549474E02);
    uint8_t asset[32];block(asset,0x22);spu_init();
    CHECK(spu_register_sample_bank(1,asset,sizeof asset));block(asset,0x44);
    CHECK(spu_register_sample_bank(2,asset,sizeof asset));
    const uint16_t regs[8]={0x1000,0x2000,0x1000,0,0xF,0,0,0};
    uint32_t mask=(1u<<20)|(1u<<21);
    CHECK(!spu_private_voice_claim(0,mask) && !spu_private_voice_claim(a,0));
    CHECK(!spu_private_voice_claim(a,1u<<24) && !spu_private_voice_claim(a,UINT32_MAX));
    CHECK(!private_enabled && !spu_private_voice_owner(20));
    CHECK(spu_private_voice_claim(a,mask) && private_enabled);
    CHECK(spu_private_voice_owner(20)==a && spu_private_voice_owner(21)==a);
    CHECK(!spu_private_voice_owner(24) && !spu_private_voice_owner(UINT32_MAX));
    CHECK(spu_private_voice_play_owned(a,20,1,regs,0));
    CHECK(spu_private_voice_play_owned(a,20,2,regs,2) && voices[44].sample_bank==2);
    CHECK(spu_private_voice_claim(a,mask)); /* Own active reservation is stable. */
    CHECK(spu_private_voice_claim(b,(1u<<22)|(1u<<23)));
    CHECK(spu_private_voice_play_owned(b,22,2,regs,0)); /* Same bank is not ownership. */
    CHECK(spu_private_voice_play_owned(b,23,2,regs,0) && spu_private_voice_owner(23)==b);
    uint32_t bytes=spu_snapshot_bytes();uint8_t *before=malloc(bytes),*after=malloc(bytes);
    CHECK(before && after);spu_snapshot_write(before);
    CHECK(!spu_private_voice_claim(b,(1u<<4)|(1u<<20)) && !spu_private_voice_owner(4));
    CHECK(!spu_private_voice_release(a,mask) && spu_private_voice_owner(21)==a);
    CHECK(!spu_private_voice_play_owned(b,20,2,regs,0));
    CHECK(!spu_private_voice_volume_owned(b,20,0,0));
    CHECK(!spu_private_voice_stop_owned(a,(1u<<20)|(1u<<22)));
    CHECK(!spu_private_voice_play_owned(0,20,2,regs,0));
    CHECK(!spu_private_voice_stop_owned(0,mask) && !spu_private_voice_stop_owned(a,0));
    CHECK(!spu_private_voice_play(20,2,regs,0) && !spu_private_voice_volume(20,0,0));
    spu_private_voice_stop(UINT32_MAX); /* Legacy cannot stop either owner. */
    spu_snapshot_write(after);CHECK(!memcmp(before,after,bytes));
    CHECK(spu_private_voice_volume_owned(a,20,0,0) && spu_private_voice_stop_owned(a,mask));
    voice_next_sample(44,0);CHECK(!voices[44].active);
    CHECK(spu_private_voice_release(a,mask) && !spu_private_voice_owner(20));
    CHECK(!spu_private_voice_release(a,1u<<22) && spu_private_voice_owner(22)==b);
    CHECK(spu_private_voice_play(20,2,regs,0));
    CHECK(!spu_private_voice_claim(a,(1u<<4)|(1u<<20)) && !spu_private_voice_owner(4));
    spu_init();CHECK(!spu_private_voice_owner(22) && !private_enabled);
    block(asset,0x22);CHECK(spu_register_sample_bank(1,asset,sizeof asset));
    block(asset,0x44);CHECK(spu_register_sample_bank(2,asset,sizeof asset));
    CHECK(spu_snapshot_read(before,bytes));
    CHECK(spu_private_voice_owner(20)==a && spu_private_voice_owner(21)==a && spu_private_voice_owner(22)==b);
    CHECK(spu_private_voice_owner(23)==b);
    CHECK(!spu_private_voice_play(20,2,regs,0) && !spu_private_voice_play_owned(b,20,2,regs,0));
    CHECK(spu_private_voice_play_owned(a,20,1,regs,0) && voices[44].sample_bank==1);
    CHECK(!spu_snapshot_len_ok(bytes-24u*8u)); /* Unknown old private wire rejected. */
    free(before);free(after);spu_init();
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
    for(unsigned i=0;i<3;++i)voice_next_sample(i, 0);
    CHECK(voices[0].samples[0]==4096 && voices[1].samples[0]==12288 && voices[2].samples[0]==28672);
    CHECK(voices[0].repeat_addr==0 && voices[1].repeat_addr==0);
    spu_snapshot_write(snapshot);int16_t pcm[3][70];
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<70;++j)pcm[i][j]=voice_next_sample(i, 0);
    CHECK(spu_snapshot_read(snapshot,n));
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<70;++j)CHECK(pcm[i][j]==voice_next_sample(i, 0));
    /* KEYOFF retains the asset through release. The next ordinary KEYON
     * reuses the voice on hardware RAM; bindings cannot leak to music/SFX. */
    key_off(1);CHECK(voices[0].sample_bank==1 && voices[0].adsr_phase==ADSR_RELEASE);
    key_on(1);ready_voice(0);voice_next_sample(0, 0);
    CHECK(!voices[0].sample_bank && voices[0].samples[0]==28672);
    /* Invalid bank addresses terminate silently instead of reading stage RAM. */
    CHECK(spu_bind_next_voice_bank(0,1));spu_regs[3]=0x100;key_on(1);ready_voice(0);
    voice_next_sample(0, 0);CHECK(voices[0].flags==1 && voices[0].samples[0]==0);
    /* The same logical channel in two seats must mix independently. P2 is
     * not a second binding on P1's hardware KEYON register. */
    memset(voices,0,sizeof voices);memset(spu_regs,0,sizeof spu_regs);
    const uint16_t params[8]={0x1000,0x1000,0x1000,0,0x000F,0x0000,0,0};
    memcpy(spu_regs+20*8,params,sizeof params);
    CHECK(spu_bind_next_voice_bank(20,1));key_on(1u<<20);ready_voice(20);
    uint8_t hardware[sizeof spu_regs];memcpy(hardware,spu_regs,sizeof hardware);
    CHECK(!spu_private_voice_play(24,2,params,0));
    CHECK(!spu_private_voice_play(20,0,params,0));
    CHECK(spu_private_voice_play(20,2,params,0));
    CHECK(!memcmp(hardware,spu_regs,sizeof hardware));
    voices[44].env_level=0x6000;voices[44].adsr_phase=ADSR_SUSTAIN;
    CHECK(spu_private_voice_active()==1u<<20 && voices[20].sample_bank==1);
    spu_write(0x1F801DAAu,0x8000);spu_write(0x1F801D80u,0x3FFF);spu_write(0x1F801D82u,0x3FFF);
    uint32_t expanded=spu_snapshot_bytes();CHECK(expanded>n);
    uint8_t *full=malloc(expanded);CHECK(full);spu_snapshot_write(full);
    int16_t together[160],replayed[160],only_first[160];
    spu_render_block(together,80);
    CHECK(voices[20].samples[0]==4096 && voices[44].samples[0]==12288);
    CHECK(spu_snapshot_read(full,expanded));spu_render_block(replayed,80);
    CHECK(!memcmp(together,replayed,sizeof together));
    CHECK(spu_snapshot_read(full,expanded));voices[44].active=0;spu_render_block(only_first,80);
    CHECK(memcmp(together,only_first,sizeof together));
    CHECK(spu_snapshot_read(full,expanded));key_off(1u<<20);
    CHECK(voices[20].adsr_phase==ADSR_RELEASE && voices[44].adsr_phase==ADSR_SUSTAIN);
    CHECK(spu_snapshot_read(full,expanded));spu_private_voice_stop(1u<<20);
    CHECK(voices[20].adsr_phase==ADSR_SUSTAIN && voices[44].adsr_phase==ADSR_RELEASE);
    CHECK(spu_private_voice_volume(20,0,0) && spu_regs[20*8]==0x1000);
    CHECK(!spu_private_voice_volume(24,0,0));
    /* Restoring an ordinary snapshot disables private voices and restores
     * its unchanged wire size. Initialization clears them and their banks. */
    CHECK(spu_snapshot_read(snapshot,n) && !spu_private_voice_active() && spu_snapshot_bytes()==n);
    free(full);free(snapshot);spu_init();CHECK(!spu_bind_next_voice_bank(0,1));
    test_private_inspection();
    test_private_owners();
    puts("SPU private sample-bank checks passed");return 0;
}
