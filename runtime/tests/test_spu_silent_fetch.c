/* Released hardware slots still fetch ADPCM and can deliver stream IRQs.
 * Includes spu.c to verify both the mixer fast path and exact decoder state. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/spu.c"

/* Dependencies owned by the surrounding runtime are inert in this unit test. */
uint64_t s_frame_count;

uint64_t psx_get_cycle_count(void) { return 0; }

void audio_trace_pcm(int tap, const int16_t *stereo, int frames) {
    (void)tap;
    (void)stereo;
    (void)frames;
}

void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) {
    (void)kind;
    (void)a;
    (void)b;
}

void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return crc;
}

bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) {
    (void)canon;
    (void)frames;
}


#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void stage(unsigned index, uint16_t pitch, int active) {
    SpuVoice *v = &voices[index];
    memset(v, 0, sizeof *v);
    v->active = active;
    v->env_level = active ? 0x6000u : 0;
    v->adsr_phase = active ? ADSR_SUSTAIN : ADSR_RELEASE;
    v->cur_addr = 0x2000;
    v->repeat_addr = 0x2000;
    v->sample_idx = SPU_BLOCK_SAMPLES;
    spu_regs[index * 8u + 2u] = pitch;
    for (uint32_t a = 0x2000; a < 0x2100; a += 16) {
        spu_ram[a] = 0;
        spu_ram[a + 1] = 0;
        memset(spu_ram + a + 2, 0x11, 14);
    }
    spu_regs[reg_index(0x1F801DAA)] = 0xC040;
    spu_regs[reg_index(0x1F801DA4)] = 0x2040 / 8;
}
static void silent_irq(int with_playing_voice) {
    spu_init();
    stage(18, 0x0800, 0);
    if (with_playing_voice) {
        stage(0, 0x1000, 1);
        spu_regs[0] = spu_regs[1] = 0x3FFF;
        spu_regs[reg_index(0x1F801D80)] = 0x3FFF;
        spu_regs[reg_index(0x1F801D82)] = 0x3FFF;
    }
    int16_t pcm[512 * 2];
    spu_render_block(pcm, 512);
    CHECK(irq_flag && voices[18].cur_addr > 0x2040);
    CHECK(!voices[18].active && !voices[18].env_level);
    unsigned nonzero = 0;
    for (unsigned i = 0; i < sizeof pcm / sizeof *pcm; ++i) nonzero += pcm[i] != 0;
    CHECK(with_playing_voice ? nonzero > 0 : nonzero == 0);
    SpuEvent events[64];
    uint32_t count = spu_event_get(events, 64), irq = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].kind == SPU_EV_IRQ && events[i].addr == 0x2040) ++irq;
    CHECK(irq == 1);
}
int main(void) {
    silent_irq(0); /* exercises the previously all-silent fast path */
    silent_irq(1); /* a simultaneous audible slot must not hide the issue */
    spu_init(); stage(18, 0x0800, 1);
    voices[18].env_level = 1; voices[18].adsr_phase = ADSR_RELEASE;
    int16_t released[512 * 2]; spu_render_block(released, 512);
    CHECK(irq_flag && !voices[18].active && !voices[18].env_level);
    spu_init(); stage(18, 0, 0);
    int16_t pcm[512 * 2]; spu_render_block(pcm, 512);
    CHECK(!irq_flag && voices[18].cur_addr == 0x2010);
    CHECK(voices[18].sample_idx == 0); /* zero pitch still holds */
    spu_init(); stage(18, 0x1000, 0);
    spu_ram[0x2041] = 1; /* silent END still updates ENDX */
    spu_render_block(pcm, 512);
    CHECK(endx_latch & (1u << 18));
    CHECK(!voices[18].active && !voices[18].env_level);
    /* A private voice is explicitly stopped after release; it is not an
     * emulated hardware fetch slot and must not dereference an expired bank. */
    voices[SPU_VOICE_COUNT].active = 0;
    voices[SPU_VOICE_COUNT].cur_addr = 0x1234;
    voices[SPU_VOICE_COUNT].sample_idx = SPU_BLOCK_SAMPLES;
    CHECK(!voice_next_sample(SPU_VOICE_COUNT));
    CHECK(voices[SPU_VOICE_COUNT].cur_addr == 0x1234);
    spu_init(); stage(18, 0x1000, 0);
    spu_regs[reg_index(0x1F801DAA)] = 0xC000; /* silent clock runs with IRQ off */
    spu_render_block(pcm, 80);
    CHECK(voices[18].cur_addr > 0x2000 && !irq_flag);
    uint32_t next = voices[18].cur_addr;
    spu_regs[reg_index(0x1F801DA4)] = (uint16_t)(next / 8);
    spu_regs[reg_index(0x1F801DAA)] = 0xC040;
    spu_render_block(pcm, 100);
    CHECK(irq_flag); /* enabling IRQ sees the continuously advanced position */
    puts("spu_silent_fetch_test: pass");
    return 0;
}
