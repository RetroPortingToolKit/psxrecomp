/*
 * MDEC status bit 29 (Command Busy) across output drain.
 *
 * Beetle's MDEC_Read reports InCommand as bit 29, and a decode command stays
 * InCommand until its last macroblock fits in the 32-word OutFIFO: the hardware
 * decodes progressively and stalls while OutFIFO is full. mdec.c decodes a whole
 * command at once, so bit 29 must be derived from the unread output.
 *
 * Regression: reporting ready right after the instant decode let libpress
 * DecDCTin (which spins on bit 29) issue the next frame while a DecDCTout column
 * DMA was mid-transfer. DecDCTout's CHCR=0 restart then dropped the words already
 * moved, and every later column was misaligned (Breath of Fire IV intro FMV:
 * vertical stripes, one-macroblock wrap, wrong colours).
 *
 * Build/run: ctest -R mdec_busy_drain_test
 */
#include "mdec.h"

#include <stdint.h>
#include <stdio.h>

uint64_t s_frame_count = 0;
uint64_t psx_cycle_count = 0;
int debug_server_fmv_quiet(void) { return 0; }

#define MDEC_DATA   0x1F801820u
#define MDEC_STATUS 0x1F801824u
#define BUSY        (1u << 29)
#define OUT_EMPTY   (1u << 31)

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

int main(void) {
    mdec_init();
    mdec_write(MDEC_STATUS, 0x80000000u);   /* reset */
    mdec_write(MDEC_STATUS, 0x60000000u);   /* enable DMA in + out */

    /* Decode, 24bpp (depth 2), 6 input words: one colour macroblock made of
     * six DC-only blocks (Cr, Cb, Y0..Y3), each a 0x0000 DC word + 0xFE00 end. */
    mdec_write(MDEC_DATA, 0x30000006u);
    CHECK(mdec_read(MDEC_STATUS) & BUSY, "busy while receiving parameters");
    for (int i = 0; i < 6; i++)
        mdec_dma_write_word(0xFE000000u);

    /* 16x16 pixels * 3 bytes = 768 bytes = 192 words of output. */
    const uint32_t total_words = 192u, fifo_words = 0x20u;
    for (uint32_t read = 0; read < total_words; read++) {
        uint32_t remaining = total_words - read;
        uint32_t st = mdec_read(MDEC_STATUS);
        if (remaining > fifo_words)
            CHECK(st & BUSY, "busy with %u words unread", remaining);
        else
            CHECK(!(st & BUSY), "ready with %u words unread (last FIFO load)", remaining);
        CHECK(!(st & OUT_EMPTY), "output empty with %u words unread", remaining);
        (void)mdec_dma_read_word();
    }
    uint32_t st = mdec_read(MDEC_STATUS);
    CHECK(!(st & BUSY), "ready after drain");
    CHECK(st & OUT_EMPTY, "output empty after drain");

    if (failures) {
        fprintf(stderr, "mdec_busy_drain_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mdec_busy_drain_test: ok\n");
    return 0;
}
