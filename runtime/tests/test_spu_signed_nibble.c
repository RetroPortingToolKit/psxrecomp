/* Authored block amplitudes through the actual production decoder. */
#define main upstream_spu_fidelity_main
#include "test_spu_fidelity.c"
#undef main

int main(void) {
    unsigned samples = 0;
    spu_init();
    for (unsigned nibble = 0; nibble < 16; nibble++) {
        for (unsigned shift = 0; shift <= 12; shift++) {
            memset(&voices[0], 0, sizeof voices[0]);
            memset(spu_ram, 0, 16);
            spu_ram[0] = (uint8_t)shift; /* filter zero */
            memset(spu_ram + 2, (int)(nibble | (nibble << 4)), 14);
            decode_block(&voices[0]);
            const int signed_nibble = nibble < 8 ? (int)nibble : (int)nibble - 16;
            const int expected = signed_nibble * (1 << (12 - shift));
            for (unsigned n = 0; n < SPU_BLOCK_SAMPLES; n++) {
                CHECK(voices[0].samples[n] == expected);
                samples++;
            }
        }
    }
    printf("SPU_SIGNED_NIBBLE_SAMPLES=%u checks=%d failures=%d\n", samples, g_checks, g_fails);
    return g_fails ? 1 : 0;
}
