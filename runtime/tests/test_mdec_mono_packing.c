/* Authored monochrome commands: byte counts, pixel order and DMA drain.
 * The existing 8-bit decoder supplies values for a separate 4-bit packing
 * expectation; this does not establish IDCT or console timing accuracy. */
#define main mdec_busy_drain_control
#include "test_mdec_busy_drain.c"
#undef main

static void mono(unsigned depth, unsigned sign, int dc, unsigned padding) {
    mdec_write(MDEC_STATUS, 0xE0000000u);
    mdec_write(MDEC_DATA, (1u << 29) | (depth << 27) | (sign << 26) |
                           (1u + padding));
    mdec_dma_write_word(0xFE000000u | ((uint32_t)dc & 0x3FFu));
    if (padding) mdec_dma_write_word(0xFE00FE00u);
}

static int size_is(unsigned expected) {
    MDECDebugState state;
    mdec_debug_get_state(&state);
    CHECK(state.output_size == expected, "output bytes: %u, expected %u",
          state.output_size, expected);
    return state.output_size == expected;
}

int main(void) {
    mdec_init();
    for (unsigned depth = 0; depth < 2; ++depth) {
        for (unsigned sign = 0; sign < 2; ++sign) {
            for (unsigned padding = 0; padding < 2; ++padding) {
                mono(depth, sign, 0, padding);
                unsigned bytes = depth ? 64 : 32;
                if (!size_is(bytes)) continue;
                uint32_t expected = sign ? 0 : depth ? 0x80808080u : 0x88888888u;
                for (unsigned i = 0; i < bytes / 4; ++i) {
                    CHECK(mdec_dma_read_ready(), "DMA ready before word %u", i);
                    CHECK(mdec_dma_read_word() == expected, "zero DC word %u", i);
                }
                CHECK(!mdec_dma_read_ready(), "DMA retired after exact output");
                CHECK(mdec_read(MDEC_STATUS) & OUT_EMPTY, "empty after drain");
            }
        }
    }
    /* Nonzero DC values exercise rounding, clipping and both pixel nibbles.
     * A known scale matrix avoids relying on uninitialized command tables. */
    mdec_write(MDEC_STATUS, 0x80000000u);
    mdec_write(MDEC_DATA, 3u << 29);
    for (unsigned i = 0; i < 32; ++i) mdec_write(MDEC_DATA, 0x7FFF7FFFu);
    unsigned levels = 0;
    for (int dc = -512; dc < 512; ++dc) {
        uint8_t decoded[64];
        /* Reset preserves the scale matrix and makes each command independent. */
        mono(1, 0, dc, 0);
        for (unsigned i = 0; i < 64; i += 4) {
            uint32_t word = mdec_dma_read_word();
            for (unsigned b = 0; b < 4; ++b) decoded[i+b] = (uint8_t)(word >> (8*b));
        }
        for (unsigned sign = 0; sign < 2; ++sign) {
            mono(0, sign, dc, 0);
            MDECDebugState state;
            mdec_debug_get_state(&state);
            if (state.output_size != 32) continue; /* Earlier size control fails. */
            for (unsigned i = 0; i < 64; i += 8) {
                uint32_t word = mdec_dma_read_word();
                for (unsigned pixel = 0; pixel < 8; ++pixel) {
                    unsigned nibble = (decoded[i+pixel] + 8u) / 16u;
                    if (nibble > 15) nibble = 15;
                    levels |= 1u << nibble;
                    if (sign) nibble ^= 8;
                    CHECK(((word >> (4*pixel)) & 15u) == nibble,
                          "DC %d sign %u pixel %u packing", dc, sign, i+pixel);
                }
            }
            CHECK(!mdec_dma_read_ready(), "32-byte DC sweep drain");
        }
    }
    CHECK((levels & 0x8101u) == 0x8101u, "DC sweep covers low, mid and high output");
    /* Preserve the existing 24-bit output/BUSY boundary control. */
    int control = mdec_busy_drain_control();
    if (failures || control) return 1;
    puts("PASS: monochrome packing, padding, signed rounding and colour drain");
    return 0;
}
