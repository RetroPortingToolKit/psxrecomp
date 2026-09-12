/* test_kernel_patch_ranges.c — the kernel-bless verifier must accept a body
 * whose DECLARED patch range has been overwritten, and still reject one
 * changed anywhere else.
 *
 * The regression this pins is the whole point of the mechanism. The verifier
 * used to memcmp a body's entire extent against the ROM source, so the
 * instant the guest's Psy-Q patcher wrote its install stub, the body
 * mismatched and every dispatch to it ran on the interpreter for the life of
 * the process. On Breath of Fire III that was the BIOS exception handler:
 * 1.22 billion interpreted instructions, 44.6 % of all interpreted work, with
 * the stub sitting at exactly the address the BIOS profile already declared.
 * Declaring a slot changed nothing because the verifier had never heard of
 * slots (`grep install_slot runtime/` returned nothing).
 *
 * Rule 18 is not relaxed here: a patched word is skipped by the VERIFIER, not
 * by execution. The emitted hook still dispatches those words to the
 * dirty-RAM interpreter, which runs the guest's own instructions.
 */
#include "kernel_patch_ranges.h"

#include <stdio.h>
#include <string.h>

static int s_fails = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_fails++;
    }
}

/* A synthetic 256-byte kernel body at RAM 0x500, sourced from ROM offset
 * 0x10000 — the same shape as SCPH-1001's "Kernel Part 2" copy. */
#define RAM_LO   0x500u
#define ROM_OFF  0x10000u
#define BODY_LO  0x500u
#define BODY_HI  0x600u

static uint8_t ram[0x800];
static uint8_t rom[0x11000];

static void reset(void) {
    /* Byte-verbatim copy: the claim every kernel body depends on. */
    for (unsigned i = 0; i < sizeof ram; i++) ram[i] = (uint8_t)(i * 7u + 3u);
    memset(rom, 0xAA, sizeof rom);
    memcpy(rom + ROM_OFF, ram + RAM_LO, sizeof ram - RAM_LO);
}

static int cmp(const PsxKernelPatchRange *r, uint32_t n, uint64_t *skips) {
    return psx_kernel_patch_cmp(r, n, ram, rom, RAM_LO, ROM_OFF,
                                BODY_LO, BODY_HI, skips);
}

int main(void) {
    /* ---- 1. An untouched body verifies, with and without ranges -------- */
    const PsxKernelPatchRange one[] = { { 0x540u, 0x550u } };
    reset();
    expect(cmp(0, 0, 0) == 0, "clean body, no ranges declared");
    expect(cmp(one, 1, 0) == 0, "clean body, one range declared");

    /* ---- 2. A word inside a DECLARED range does not unbless the body --- */
    reset();
    ram[0x544u] ^= 0xFFu;            /* inside [0x540, 0x550) */
    expect(cmp(one, 1, 0) == 0, "patched word inside a declared range: CLEAN");
    expect(cmp(0, 0, 0) != 0,
           "the same word with NO range declared: MISMATCH (the old behaviour)");

    /* Every word of the range, and both its edges. */
    reset();
    for (uint32_t a = 0x540u; a < 0x550u; a++) ram[a] ^= 0xFFu;
    expect(cmp(one, 1, 0) == 0, "whole declared range overwritten: CLEAN");

    /* ---- 3. A word OUTSIDE the range still unblesses ------------------- */
    reset();
    ram[0x53Cu] ^= 0xFFu;            /* one byte below the range */
    expect(cmp(one, 1, 0) != 0, "word just below the range: MISMATCH");
    reset();
    ram[0x550u] ^= 0xFFu;            /* first byte above the range */
    expect(cmp(one, 1, 0) != 0, "word just above the range: MISMATCH");
    reset();
    ram[BODY_LO] ^= 0xFFu;
    expect(cmp(one, 1, 0) != 0, "first word of the body: MISMATCH");
    reset();
    ram[BODY_HI - 1u] ^= 0xFFu;
    expect(cmp(one, 1, 0) != 0, "last word of the body: MISMATCH");

    /* ---- 4. Several ranges, and ranges outside this body --------------- */
    /* Sorted and non-overlapping, as the emitter guarantees. 0x700 lies past
     * the body: it must neither be skipped into nor stop the walk early. */
    const PsxKernelPatchRange many[] = {
        { 0x510u, 0x520u }, { 0x540u, 0x550u }, { 0x5F0u, 0x600u },
        { 0x700u, 0x710u },
    };
    reset();
    ram[0x514u] ^= 0xFFu;
    ram[0x548u] ^= 0xFFu;
    ram[0x5F8u] ^= 0xFFu;            /* a range flush against body_hi */
    expect(cmp(many, 4, 0) == 0, "three declared ranges patched: CLEAN");
    ram[0x530u] ^= 0xFFu;            /* in the gap between ranges */
    expect(cmp(many, 4, 0) != 0, "a gap between declared ranges still checked");

    /* A range that starts before the body still clamps correctly. */
    const PsxKernelPatchRange spanning[] = { { 0x4F0u, 0x510u } };
    reset();
    ram[0x504u] ^= 0xFFu;
    expect(cmp(spanning, 1, 0) == 0, "range starting below body_lo: CLEAN");

    /* ---- 5. The skip counter reports real work ------------------------- */
    {
        uint64_t skips = 0;
        reset();
        (void)cmp(many, 4, &skips);
        expect(skips == 3u, "skipped exactly the three intersecting ranges");
        skips = 0;
        (void)cmp(0, 0, &skips);
        expect(skips == 0u, "no ranges declared: nothing skipped");
    }

    /* ---- 6. The interpreter hand-back predicate ------------------------ */
    expect(psx_kernel_patch_ends_at(many, 4, 0x550u) == 1,
           "a range end is a resume PC");
    expect(psx_kernel_patch_ends_at(many, 4, 0x540u) == 0,
           "a range START is not a resume PC");
    expect(psx_kernel_patch_ends_at(many, 4, 0x548u) == 0,
           "an interior word is not a resume PC");
    expect(psx_kernel_patch_ends_at(0, 0, 0x550u) == 0,
           "no ranges: no resume PCs");

    if (s_fails == 0) {
        printf("test_kernel_patch_ranges: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "test_kernel_patch_ranges: %d failure(s)\n", s_fails);
    return 1;
}
