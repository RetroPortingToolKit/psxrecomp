/* Production CPU memory map and real SPU: paired registers, upper voices and
 * rendered stereo. Unrelated tracing/shadow sinks use the established fixture. */
#include "cpu_state.h"
#include "psx_memory.h"
#include "psx_direct_memory.h"
#include "spu.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t cycles;
uint64_t psx_get_cycle_count(void) { return cycles; }
void audio_trace_pcm(int tap, const int16_t *p, int n) { (void)tap; (void)p; (void)n; }
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) { (void)kind; (void)a; (void)b; }
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *p, int n) { (void)p; (void)n; }
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)bit; (void)detail; abort(); /* These cases leave SPU IRQ disabled. */
}

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    if (failures <= 10) fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
} } while (0)

static void packed_pair(uint32_t alias, uint32_t address, uint32_t value) {
    psx_write_word(alias | address, value);
    CHECK(psx_read_half(alias | address) == (uint16_t)value);
    CHECK(psx_read_half(alias | (address + 2u)) == (uint16_t)(value >> 16));
    CHECK(psx_read_word(alias | address) == value);
}

int main(void) {
    const uint32_t aliases[] = { 0u, 0x80000000u, 0xA0000000u };
    for (unsigned a = 0; a < 3; ++a) {
        uint32_t alias = aliases[a];
        spu_init();
        for (unsigned voice = 0; voice < 24; ++voice) {
            uint32_t base = 0x1F801C00u + voice * 16u;
            SpuVoiceState state;
            packed_pair(alias, base, 0x23451234u + voice);
            spu_get_voice_state((int)voice, &state);
            CHECK(state.vol_cur_r == 0x468A);
            CHECK(psx_read_word(alias | (0x1F801E00u + voice * 4u)) ==
                  (0x468A0000u | (uint32_t)((0x1234u + voice) << 1)));
            packed_pair(alias, base + 4u, 0x12341000u + voice);
            packed_pair(alias, base + 8u, 0x4567000Fu + voice);
            /* A zero high half clears the right channel, rather than omitting
             * the second write. A halfword still updates just its own register. */
            packed_pair(alias, base, 0x00001234u);
            psx_write_half(alias | (base + 2u), 0x3210u);
            CHECK(psx_read_half(alias | base) == 0x1234u);
            CHECK(psx_read_half(alias | (base + 2u)) == 0x3210u);
        }
        packed_pair(alias, 0x1F801DC0u, 0xFEDCBA98u);

        /* Ordinary CPU words visit adjacent registers: FIFO then SPUCNT.
         * DMA words append two halfwords to SPU RAM, leaving SPUCNT alone. */
        uint8_t bytes[6];
        spu_init();
        psx_write_half(alias | 0x1F801DA6u, 0x0200u);
        psx_write_word(alias | 0x1F801DA8u, 0xC00055AAu);
        CHECK(spu_ctrl_read() == 0xC000u);
        CHECK(spu_ram_peek(0x1000u, bytes, 4u) == 4u);
        CHECK(bytes[0] == 0xAA && bytes[1] == 0x55 && !bytes[2] && !bytes[3]);
        spu_dma_write(0x44332211u);
        CHECK(spu_ctrl_read() == 0xC000u);
        CHECK(spu_ram_peek(0x1000u, bytes, 6u) == 6u);
        CHECK(bytes[0] == 0xAA && bytes[1] == 0x55 && bytes[2] == 0x11 &&
              bytes[3] == 0x22 && bytes[4] == 0x33 && bytes[5] == 0x44);

        /* A looping ADPCM voice23 must key on via the upper half, render
         * both channels, expose upper ENDX and receive upper KEYOFF. */
        spu_init();
        psx_write_half(alias | 0x1F801DAAu, 0xC000u);
        psx_write_word(alias | 0x1F801D80u, 0x3FFF3FFFu);
        psx_write_half(alias | 0x1F801DA6u, 0x0200u);
        spu_dma_write(0x77770300u);
        for (unsigned i = 0; i < 3; ++i) spu_dma_write(0x77777777u);
        uint32_t voice23 = 0x1F801C00u + 23u * 16u;
        psx_write_word(alias | voice23, 0x20002000u);
        psx_write_word(alias | (voice23 + 4u), 0x02001000u);
        psx_write_word(alias | (voice23 + 8u), 0u);
        psx_write_half(alias | (voice23 + 14u), 0x0200u);
        psx_write_word(alias | 0x1F801D88u, 0x00800000u);
        SpuGlobalState global;
        spu_get_global_state(&global);
        CHECK(global.active_mask == 0x00800000u);
        CHECK(global.kon_latch == 0x00800000u);
        int16_t pcm[256 * 2];
        cycles += 768u * 256u;
        spu_catch_up();
        CHECK(spu_output_pop(pcm, 256) == 256);
        uint64_t left = 0, right = 0;
        for (unsigned i = 0; i < 256; ++i) {
            left += (uint64_t)abs(pcm[i * 2]);
            right += (uint64_t)abs(pcm[i * 2 + 1]);
            CHECK(pcm[i * 2] == pcm[i * 2 + 1]);
        }
        CHECK(left > 0 && right > 0);
        CHECK(psx_read_word(alias | 0x1F801D9Cu) & 0x00800000u);
        psx_write_word(alias | 0x1F801D8Cu, 0x00800000u);
        spu_get_global_state(&global);
        CHECK(global.koff_latch == 0x00800000u);
        CHECK(psx_read_word(alias | 0x1F801D8Cu) == 0x00800000u);
    }
    if (failures) {
        fprintf(stderr, "%u/%u SPU bus checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS: %u real-memory/SPU checks,24 voices, three aliases and stereo PCM\n", checks);
    return 0;
}
