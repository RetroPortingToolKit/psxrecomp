/* A mid-command snapshot must reserve the rest of the transfer on a fresh
 * runtime, before either MMIO or DMA continues writing the input FIFO. */
#include "../src/mdec.c"

#include <stdio.h>

uint64_t psx_cycle_count;
uint64_t s_frame_count;
int debug_server_fmv_quiet(void) { return 1; }

static void check(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); exit(1); }
}

static void fresh(void) {
    free(mdec.input);
    free(mdec.output);
    mdec_init();
}

static void input_word(int dma, uint32_t value) {
    if (dma) mdec_dma_write_word(value);
    else mdec_write(0, value);
}

static void interrupted_input(int dma) {
    const unsigned words = 768, prefix = 16;
    fresh();
    mdec_write(0, (1u << 29) | (1u << 27) | words);
    for (unsigned i = 0; i < prefix; ++i) input_word(dma, 0xFE000000u);
    check(mdec.busy && mdec.input_count < mdec.expected_halfwords,
          "snapshot contains an unfinished input command");
    uint32_t size = mdec_snapshot_bytes();
    uint8_t *saved = malloc(size);
    check(saved != NULL, "snapshot allocation");
    mdec_snapshot_write(saved);
    for (unsigned i = prefix; i < words; ++i) input_word(dma, 0xFE000000u);
    uint32_t completed_size = mdec_snapshot_bytes();
    uint8_t *expected = malloc(completed_size), *actual = malloc(completed_size);
    check(expected != NULL && actual != NULL, "completed snapshot allocation");
    mdec_snapshot_write(expected);

    fresh(); /* No buffer survives from the runtime that saved the command. */
    check(mdec_snapshot_validate(saved, size), "validate partial command");
    check(mdec_snapshot_read(saved, size), "restore partial command");
    /* Check before resuming: the old reader fails without writing past a buffer. */
    check(mdec.input_cap >= mdec.expected_halfwords,
          "restore reserves the entire command before continued writes");
    for (unsigned i = prefix; i < words; ++i) input_word(dma, 0xFE000000u);
    check(mdec_snapshot_bytes() == completed_size, "resumed snapshot size");
    mdec_snapshot_write(actual);
    check(memcmp(expected, actual, completed_size) == 0,
          "resumed command equals uninterrupted command");
    free(saved);
    free(expected);
    free(actual);
}

static void malformed_and_output(void) {
    fresh();
    mdec_write(0, (1u << 29) | (1u << 27) | 1u);
    mdec_write(0, 0xFE000000u);
    check(mdec.output_size == 64u, "pending output exists");
    (void)mdec_read(0);
    uint32_t size = mdec_snapshot_bytes();
    uint8_t *saved = malloc(size);
    check(saved != NULL, "output snapshot allocation");
    mdec_snapshot_write(saved);
    fresh();
    check(mdec_snapshot_read(saved, size), "restore pending output");
    for (unsigned i = 4; i < 64; i += 4)
        check(mdec_read(0) == 0x80808080u, "pending output drains after restore");
    check(!mdec_dma_read_ready(), "output drain completes");

    PstW w;
    pst_w_init(&w, saved + 8u, 4u); /* expected_halfwords */
    check(pst_w_u32(&w, MDEC_SNAP_INPUT_MAX + 1u), "patch expected length");
    uint32_t before = mdec_snapshot_bytes();
    uint8_t *unchanged = malloc(before), *after = malloc(before);
    check(unchanged != NULL && after != NULL, "refusal snapshot allocation");
    mdec_snapshot_write(unchanged);
    check(!mdec_snapshot_validate(saved, size), "validate refuses oversized command");
    check(!mdec_snapshot_read(saved, size), "read refuses oversized command");
    check(mdec_snapshot_bytes() == before, "refused read preserves snapshot size");
    mdec_snapshot_write(after);
    check(memcmp(unchanged, after, before) == 0, "refused read preserves state");
    free(saved);
    free(unchanged);
    free(after);
}

int main(void) {
    interrupted_input(0);
    interrupted_input(1);
    malformed_and_output();
    fresh();
    puts("mdec_snapshot_capacity_test: PASS (MMIO, DMA, output, malformed input)");
    return 0;
}
