/*
 * Guest-clocked SPU (beads-eio.3.321): every SPU access advances the SPU to
 * the current guest cycle, so a game that keys a voice on and then polls its
 * envelope (ENVX) sees it rise without any host audio pump. Crash Bandicoot's
 * AudioUpdate keys a voice off when it reads "key on, envelope 0"; with the
 * SPU advanced only at the per-frame pump, its spin sound was cut the frame
 * it started.
 *
 * The test includes spu.c so it can drive the guest clock directly.
 * Build/run: ctest -R spu_guest_clock_test
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/spu.c"

uint64_t s_frame_count;
#if defined(PSX_TEST_SCHEDULER)
#include "interrupts.h"
void psx_set_midframe_audio_pump(void (*fn)(void));
uint64_t psx_cycle_count;
#define g_cycle psx_cycle_count
#else
static uint64_t g_cycle;
#endif
uint64_t psx_get_cycle_count(void) { return g_cycle; }
void audio_trace_pcm(int tap, const int16_t *stereo, int frames) { (void)tap; (void)stereo; (void)frames; }
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) { (void)kind; (void)a; (void)b; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) { (void)data; (void)len; return crc; }
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) { (void)canon; (void)frames; }

static int failures;
#define CHECK(condition, message)                                               \
    do {                                                                        \
        if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
        else printf("ok:   %s\n", message);                                     \
    } while (0)

#define VOICE0(off) (0x1F801C00u + (off))
static int s_suppress;
static int suppressed(void) { return s_suppress; }

static void stage_voice0(void) {
    spu_init();
    g_cycle = 0;
    for (uint32_t b = 0; b < 8u; b++) {          /* nonzero ADPCM, last block loops */
        uint8_t *blk = spu_ram + 0x1000u + b * 16u;
        blk[0] = 0x00;
        blk[1] = b == 7u ? 0x03 : 0x00;
        memset(blk + 2, 0x77, 14);
    }
    spu_write(0x1F801DAAu, 0x8000u);             /* SPU on */
    spu_write(VOICE0(0x0), 0x3FFFu);             /* volume L/R */
    spu_write(VOICE0(0x2), 0x3FFFu);
    spu_write(VOICE0(0x4), 0x1000u);             /* pitch 1.0 */
    spu_write(VOICE0(0x6), 0x1000u >> 3);        /* start address */
    spu_write(VOICE0(0x8), 0x000Fu);             /* fastest linear attack, SL max */
    spu_write(VOICE0(0xA), 0x0000u);
    spu_write(VOICE0(0xE), 0x1070u >> 3);        /* repeat address */
}

/* Initialization is a guest-clock boundary even before the first register
 * access. A host pump must not silently discard that initial interval. */
static void test_initial_clock_boundary(void) {
    g_cycle = 0;
    spu_init();
    g_cycle = 767;
    spu_catch_up();
    CHECK(spu_output_available() == 0u, "initial sample is not produced early");
    g_cycle++;
    spu_catch_up();
    CHECK(spu_output_available() == 1u, "first sample is due 768 cycles after init");
    g_cycle = 768u * 37u + 123u;
    spu_init();
    CHECK(spu_output_available() == 0u, "reinitialization clears pending output");
    g_cycle += 768u;
    spu_catch_up();
    CHECK(spu_output_available() == 1u, "nonzero init anchors to its own guest epoch");
}

static void test_envelope_rises_between_accesses(void) {
    stage_voice0();
    g_cycle = 768u * 10u + 100u;
    spu_write(0x1F801D88u, 0x0001u);             /* KEYON voice 0 */
    CHECK(spu_output_available() == 10u, "the write first renders the 10 samples owed");
    CHECK(spu_read(VOICE0(0xC)) == 0u, "ENVX is 0 within the KEYON sample");
    g_cycle += 768u * 3u;
    const uint32_t env = spu_read(VOICE0(0xC));
    CHECK(env > 0u, "ENVX rises three samples after KEYON with no host pump");
    CHECK(spu_output_available() == 13u, "the read rendered exactly the 3 owed samples");
    int16_t out[16 * 2];
    CHECK(spu_output_pop(out, 16u) == 13u && spu_output_available() == 0u,
          "the host pump drains what the guest clock produced");
}

static void test_snapshot_keeps_clock_phase(void) {
    stage_voice0();
    g_cycle = 768u * 20u + 400u;
    spu_catch_up();
    const uint32_t n = spu_snapshot_bytes();
    uint8_t *snap = (uint8_t *)malloc(n);
    spu_snapshot_write(snap);
    CHECK(spu_snapshot_len_ok(n) && spu_snapshot_len_ok(n - 4u) && !spu_snapshot_len_ok(n - 1u),
          "current and pre-clock snapshot lengths validate, others do not");

    g_cycle = 768u * 5000u + 400u;               /* restored guest clock, same phase */
    CHECK(spu_snapshot_read(snap, n) == 1, "snapshot with clock phase loads");
    spu_catch_up();
    CHECK(spu_output_available() == 0u, "no samples owed right after restore");
    g_cycle += 367u;
    spu_catch_up();
    CHECK(spu_output_available() == 0u, "one cycle short of the next sample boundary");
    g_cycle += 1u;
    spu_catch_up();
    CHECK(spu_output_available() == 1u, "phase carried: next sample lands on the boundary");

    g_cycle = 768u * 9000u + 123u;
    CHECK(spu_snapshot_read(snap, n - 4u) == 1, "pre-clock snapshot loads (phase 0)");
    spu_catch_up();
    g_cycle += 767u;
    spu_catch_up();
    CHECK(spu_output_available() == 0u, "pre-clock snapshot anchors the clock at load");
    free(snap);
}

static void test_gate_and_rewind(void) {
    stage_voice0();
    spu_set_output_gate(suppressed);
    g_cycle = 768u * 4u;
    spu_write(0x1F801D88u, 0x0001u);
    spu_output_flush();
    s_suppress = 1;
    g_cycle += 768u * 6u;
    const uint32_t env = spu_read(VOICE0(0xC));
    CHECK(env > 0u && spu_output_available() == 0u,
          "suppressed output still advances SPU state");
    s_suppress = 0;
    spu_set_output_gate(NULL);
    const uint64_t clock = s_clock;
    g_cycle = 768u * 2u;                         /* guest clock rewound (state load) */
    spu_catch_up();
    CHECK(s_clock == g_cycle && s_clock < clock && spu_output_available() == 0u,
          "a rewound guest clock re-anchors without rendering");
}

static uint32_t pcm_frames;
static void pcm_overlay_test(int16_t* stereo, uint32_t frames) {
    pcm_frames += frames;
    for (uint32_t i = 0; i < frames * 2u; ++i) stereo[i] = 1234;
}

static void test_pcm_overlay_uses_guest_clock(void) {
    g_cycle = 0;
    spu_init();
    spu_set_pcm_overlay(pcm_overlay_test);
    spu_catch_up();
    g_cycle += 768u * 3u;
    spu_catch_up();
    CHECK(pcm_frames == 3u && spu_output_available() == 3u,
          "PCM overlay advances with guest samples while the native SPU is off");
    int16_t output[8];
    CHECK(spu_output_pop(output, 4u) == 3u && output[0] == 1234 &&
              output[5] == 1234 && pcm_frames == 3u,
          "host output consumption neither skips nor advances mod playback");
    spu_set_output_gate(suppressed);
    s_suppress = 1;
    g_cycle += 768u * 2u;
    spu_catch_up();
    CHECK(pcm_frames == 5u && spu_output_available() == 0u,
          "suppressed output still advances deterministic mod PCM state");
    s_suppress = 0;
    spu_set_output_gate(NULL);
    spu_write(0x1F801DAAu, 0x8000u);
    g_cycle += 768u * 4u;
    spu_catch_up();
    CHECK(pcm_frames == 9u && spu_output_available() == 4u,
          "native voice-free fast path advances the same mod PCM clock");
    spu_set_pcm_overlay(NULL);
    spu_output_flush();
    g_cycle += 768u;
    spu_catch_up();
    CHECK(pcm_frames == 9u && spu_output_pop(output, 4u) == 1u && output[0] == 0,
          "unbinding restores native silence without advancing mod playback");
}

static void render_partition(int granular, int16_t *out, SpuVoiceState *state) {
    g_cycle = 0;
    stage_voice0();
    spu_write(0x1F801DAAu, 0xC001u); // output and CD input enabled
    spu_write(0x1F801D80u, 0x3FFFu);
    spu_write(0x1F801D82u, 0x3FFFu);
    spu_write(0x1F801DB0u, 0x7FFFu);
    spu_write(0x1F801DB2u, 0x7FFFu);
    spu_write(0x1F801D88u, 1u);
    int16_t cd[128];
    for (int i = 0; i < 128; ++i) cd[i] = (int16_t)(i * 97 - 6000);
    for (unsigned sample = 1; sample <= 735; ++sample) {
        if (granular || sample == 200 || sample == 500 || sample == 735) {
            g_cycle = sample * 768u;
            spu_catch_up();
        }
        if (sample == 200) spu_cd_audio_push(cd, 64);
        if (sample == 500) spu_write(0x1F801D8Cu, 1u);
    }
    CHECK(spu_output_pop(out, 735) == 735, "partition renders one frame of PCM");
    spu_get_voice_state(0, state);
}

static void test_block_pcm_partition(void) {
    int16_t sample_pcm[735 * 2], block_pcm[735 * 2];
    SpuVoiceState sample_state, block_state;
    render_partition(1, sample_pcm, &sample_state);
    render_partition(0, block_pcm, &block_state);
    CHECK(memcmp(sample_pcm, block_pcm, sizeof sample_pcm) == 0,
          "sample and observation-block PCM are exact across CD input and KEYOFF");
    CHECK(memcmp(&sample_state, &block_state, sizeof sample_state) == 0,
          "block partition preserves decoder, pitch and envelope state");
    int audible = 0;
    for (int i = 0; i < 735 * 2; ++i) audible |= sample_pcm[i] != 0;
    CHECK(audible, "partition comparison contains real nonzero audio");
}

#if defined(PSX_TEST_SCHEDULER)
static unsigned scheduler_pumps;
static void scheduler_pump(void) { ++scheduler_pumps; spu_catch_up(); }
static void test_scheduler_latch_rearm(void) {
    g_cycle = 0;
    stage_voice0();
    psx_set_midframe_audio_pump(scheduler_pump);
    spu_write(0x1F801DA4u, 0x2000u >> 3);
    spu_write(0x1F801DAAu, 0x8040u);
    CHECK(spu_irq_can_raise(), "unlatched armed SPU can raise a sample IRQ");
    CHECK(psx_spu_sample_event_cycles_to_next() == 768u,
          "real scheduler keeps the next possible IRQ sample");
    g_cycle += 768u;
    psx_spu_sample_event_service();
    CHECK(scheduler_pumps == 1u, "unlatched sample invokes the real pump");
    spu_irq_check(0x2000u, 2u);
    CHECK(!spu_irq_can_raise(), "a hardware RAM hit latches the IRQ");
#if defined(PSX_BLOCK_AUDIO) && PSX_BLOCK_AUDIO
    CHECK(psx_spu_sample_event_cycles_to_next() == UINT32_MAX,
          "ENHANCED batches a latched interval until a real observation");
    g_cycle += 768u;
    psx_spu_sample_event_service();
    CHECK(scheduler_pumps == 1u, "latched ENHANCED interval needs no sample pump");
#else
    CHECK(psx_spu_sample_event_cycles_to_next() == 768u,
          "REFERENCE retains the original per-sample deadline");
    g_cycle += 768u;
    psx_spu_sample_event_service();
    CHECK(scheduler_pumps == 2u, "REFERENCE retains the sample pump");
#endif
    spu_write(0x1F801DAAu, 0x8000u); // catches up before clearing latch
    spu_write(0x1F801DAAu, 0x8040u);
    CHECK(spu_irq_can_raise() && psx_spu_sample_event_cycles_to_next() == 768u,
          "guest acknowledgement/rearm restores the sample deadline");
    unsigned old_pumps = scheduler_pumps;
    g_cycle += 768u;
    psx_spu_sample_event_service();
    CHECK(scheduler_pumps == old_pumps + 1u, "rearmed IRQ pumps before the next observation");
    psx_set_midframe_audio_pump(NULL);
}
#endif

int main(void) {
    test_initial_clock_boundary();
    test_envelope_rises_between_accesses();
    test_snapshot_keeps_clock_phase();
    test_gate_and_rewind();
    test_pcm_overlay_uses_guest_clock();
    test_block_pcm_partition();
#if defined(PSX_TEST_SCHEDULER)
    test_scheduler_latch_rearm();
#endif
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("spu_guest_clock_test: ok");
    return 0;
}
