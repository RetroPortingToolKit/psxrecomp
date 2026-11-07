/* Register-driven pitch-counter and mixer checks against PSX-SPX's SPU
 * ADPCM Pitch algorithm. No game-specific switch or external assets. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/spu.c"

uint64_t s_frame_count;
uint64_t psx_get_cycle_count(void) { return 0; }
void audio_trace_pcm(int t, const int16_t *p, int n) {(void)t;(void)p;(void)n;}
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) {(void)k;(void)a;(void)b;}
void psx_irq_raise(uint32_t b, uint32_t d) {(void)b;(void)d;}
uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {(void)p;(void)n;return c;}
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *p, int n) {(void)p;(void)n;}
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}} while (0)

static void pitch(unsigned v, uint16_t value) {
    if (v < SPU_VOICE_COUNT) spu_regs[v * 8 + 2] = value;
    else private_regs[v - SPU_VOICE_COUNT][2] = value;
}

static void test_pitch_vectors(void) {
    spu_init();
    pitch(1, 0x1000);
    CHECK(voice_pitch_step(1, -32768) == 0x1000); /* PMON disabled */
    spu_regs[reg_index(0x1F801D90u)] = 3;
    CHECK(voice_pitch_step(1, -32768) == 0);
    CHECK(voice_pitch_step(1, -16384) == 0x0800);
    CHECK(voice_pitch_step(1, 0) == 0x1000);
    CHECK(voice_pitch_step(1, 16384) == 0x1800);
    CHECK(voice_pitch_step(1, 32767) == 0x1FFF);
    pitch(0, 0x1000);
    CHECK(voice_pitch_step(0, -32768) == 0x1000); /* bit zero unused */
    pitch(1, 0);
    CHECK(voice_pitch_step(1, 32767) == 0);
    pitch(1, 0x4000);
    CHECK(voice_pitch_step(1, -16384) == 0x2000); /* full register before clamp */
    CHECK(voice_pitch_step(1, 32767) == 0x4000);
    pitch(1, 0x8000);
    CHECK(voice_pitch_step(1, 32767) == 1); /* signed multiply, then wrap */
    pitch(1, 0xFFFF);
    CHECK(voice_pitch_step(1, 0) == 0x4000);
    spu_regs[reg_index(0x1F801D90u)] = 0;
    pitch(1, 0x4000);
    CHECK(voice_pitch_step(1, 0) == 0x4000);
    CHECK(voice_needs_clock(1));
    pitch(1, 0xFFFF);
    CHECK(voice_pitch_step(1, 0) == 0x4000);
    pitch(16, 0x1000); pitch(23, 0x1000);
    spu_regs[reg_index(0x1F801D92u)] = 0x81;
    CHECK(voice_pitch_step(16, -16384) == 0x0800);
    CHECK(voice_pitch_step(23, 16384) == 0x1800);
    pitch(17, 0x1000);
    CHECK(voice_pitch_step(17, -32768) == 0x1000);
    pitch(24, 0x1000);
    CHECK(voice_pitch_step(24, -32768) == 0x1000); /* private voices isolated */
}

static void stage_voice(unsigned i, uint16_t rate) {
    SpuVoice *v = &voices[i];
    v->active = 1;
    v->sample_idx = 7;
    v->env_level = 0x7FFF;
    v->adsr_phase = ADSR_SUSTAIN;
    v->cur_addr = 0x2000 + i * 16;
    for (unsigned j = 0; j < SPU_BLOCK_SAMPLES; ++j)
        v->samples[j] = (int16_t)((j & 1) ? 20000 : -20000);
    pitch(i, rate);
}

static void test_current_sample_before_channel_volume(unsigned source) {
    spu_init();
    stage_voice(source, 0);
    stage_voice(source + 1, 0x1000);
    noise_lfsr = 0x4000;
    spu_regs[reg_index(0x1F801D94u + (source >= 16 ? 2 : 0))] = 1u << (source & 15);
    spu_regs[reg_index(0x1F801D90u + (source + 1 >= 16 ? 2 : 0))] = 1u << ((source + 1) & 15);
    spu_regs[reg_index(0x1F801DAAu)] = 0x8000;
    int16_t out[2];
    spu_render_block(out, 1);
    /* Current source is 16384 * 32767 >> 15 = 16383, including when both
     * channel-volume registers are zero. Carrier step is exactly 6143. */
    CHECK(voices[source + 1].sample_idx == 8);
    CHECK(voices[source + 1].phase == 2047);
    CHECK(out[0] == 0 && out[1] == 0);
}

static void test_restore_and_block_boundaries(void) {
    spu_init();
    stage_voice(0, 0x0500); stage_voice(1, 0x1000); stage_voice(2, 0x0800);
    spu_regs[reg_index(0x1F801D90u)] = 6; /* chained current-sample modulation */
    spu_regs[reg_index(0x1F801DAAu)] = 0x8000;
    spu_regs[reg_index(0x1F801D80u)] = 0x3FFF;
    spu_regs[reg_index(0x1F801D82u)] = 0x3FFF;
    spu_regs[8] = spu_regs[9] = 0x3FFF;
    for (unsigned a = 0x2000; a < 0x2200; a += 16) {
        spu_ram[a + 1] = 7; memset(spu_ram + a + 2, 0x73, 14);
    }
    uint32_t bytes = spu_snapshot_bytes();
    uint8_t *initial = malloc(bytes), *whole = malloc(bytes), *split = malloc(bytes);
    CHECK(initial && whole && split);
    int16_t a[512], b[512], unmodulated[512];
    spu_snapshot_write(initial);
    spu_render_block(a, 256); spu_snapshot_write(whole);
    CHECK(spu_snapshot_read(initial, bytes));
    spu_render_block(b, 37); spu_render_block(b + 74, 219); spu_snapshot_write(split);
    CHECK(!memcmp(a, b, sizeof a));
    CHECK(!memcmp(whole, split, bytes));
    CHECK(spu_snapshot_read(initial, bytes));
    spu_regs[reg_index(0x1F801D90u)] = 0;
    spu_render_block(unmodulated, 256);
    CHECK(memcmp(a, unmodulated, sizeof a));
    free(initial); free(whole); free(split);
}

static void test_fast_pitch_crosses_adpcm_block(void) {
    spu_init();
    stage_voice(0, 0x4000);
    voices[0].sample_idx = 27;
    spu_ram[0x2001] = 7;
    memset(spu_ram + 0x2002, 0x11, 14);
    voice_next_sample(0, 0);
    CHECK(voices[0].sample_idx == 31 && voices[0].phase == 0);
    pitch(0, 0);
    voice_next_sample(0, 0);
    CHECK(voices[0].sample_idx == 3 && voices[0].phase == 0);
    CHECK(voices[0].cur_addr == 0x2010);
}

int main(void) {
    test_pitch_vectors();
    test_current_sample_before_channel_volume(0);
    test_current_sample_before_channel_volume(15);
    test_restore_and_block_boundaries();
    test_fast_pitch_crosses_adpcm_block();
    puts("SPU pitch modulation checks passed");
    return 0;
}
