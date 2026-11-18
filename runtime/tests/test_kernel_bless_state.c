/* Kernel bless: the verify state of each row follows RAM writes, and nothing
 * else.
 *
 * Kernel bless runs a relocated kernel routine's compiled body only while its
 * bytes in RAM still equal the ROM source; each row of the table caches that
 * check. The generated BIOS dispatch calls dirty_ram_mark_executable_range(addr,
 * 4) on every RAM-alias dispatch of an exception-handler continuation key
 * (0xD00, 0xE10, 0xE28). That function used to reset every row, so each such
 * dispatch threw the whole table away and re-verified it.
 *
 * This drives the real memory.c bless code with a synthetic table: no BIOS
 * image, game data or timing stubs. LTO discards the unrelated memory-map
 * paths, as in the other single-source fixtures.
 *
 *   (no argument)  every row blesses; a patched body falls back to the
 *                  interpreter and returns once its bytes match again;
 *                  marking a range executable resets nothing; a store does
 *   overflow       a table larger than the verify-state array turns bless off,
 *                  and psx_kernel_bless_table_fits says so
 *   paranoid       PSX_KERNEL_BLESS_PARANOID=1 catches a RAM write that
 *                  told nobody, and counts nothing on a clean run
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/memory.c"

#define ROWS        4000u          /* fits KBLESS_MAX_ENTRIES */
#define WIN_LO      0x00000500u    /* relocated kernel window in RAM */
#define WIN_HI      0x00008500u
#define ROM_OFF     0x00010000u    /* its ROM source: 0x1FC10000 */
#define BODY_BYTES  0x100u         /* 64 four-byte keys share one body */
#define PATCHED_ROW 3900u          /* in the body of rows 3840..3903 */

/* The backend selection normally publishes these (psx_bios_backend.c). */
static PsxKernelBody s_bodies[KBLESS_MAX_ENTRIES + 1u];
const PsxKernelBody *psx_bios_kernel_bodies = s_bodies;
uint32_t psx_bios_kernel_body_count = 0;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges = 0;
uint32_t psx_bios_kernel_patch_range_count = 0;
PsxBiosImageInfo psx_bios_image;

static uint32_t row_key(uint32_t i) { return WIN_LO + 4u * i; }
static uint32_t row_body(uint32_t i) { return WIN_LO + (i / 64u) * BODY_BYTES; }

static void build_table(uint32_t rows) {
    for (uint32_t i = 0; i < rows; ++i) {
        s_bodies[i].key = row_key(i);
        s_bodies[i].body_lo = row_body(i);
        s_bodies[i].body_hi = row_body(i) + BODY_BYTES;
    }
    psx_bios_kernel_body_count = rows;
    memset(&psx_bios_image, 0, sizeof(psx_bios_image));
    psx_bios_image.kbless_ram_lo = WIN_LO;
    psx_bios_image.kbless_ram_hi = WIN_HI;
    psx_bios_image.kbless_rom_off = ROM_OFF;
    /* The BIOS copies this ROM slice into the window at boot. */
    for (uint32_t i = 0; i < WIN_HI - WIN_LO; ++i)
        bios_rom[ROM_OFF + i] = (uint8_t)(i * 7u + 3u);
    memcpy(ram + WIN_LO, bios_rom + ROM_OFF, WIN_HI - WIN_LO);
    psx_kernel_bless_reset_for_boot();
}

static uint64_t stat(unsigned index) {
    uint64_t s[8];
    psx_kernel_bless_stats(s);
    return s[index];
}

static int page_marked(uint32_t phys) {
    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;
    return (dirty_ram_bitmap[page >> 5] >> (page & 31u)) & 1u;
}

static uint64_t pstat(unsigned index) {
    uint64_t s[5];
    psx_kernel_bless_paranoid_stats(s);
    return s[index];
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int paranoid_main(void) {
    set_env("PSX_KERNEL_BLESS_PARANOID", "1");
    build_table(ROWS);
    for (uint32_t i = 0; i < ROWS; ++i)
        assert(psx_kernel_bless_dispatchable(row_key(i)) == 1);
    assert(pstat(0) == 1u);
    assert(pstat(1) == 0);        /* first dispatch verifies: no re-check */

    /* Clean run: every blessed dispatch re-compares and finds nothing. */
    for (uint32_t i = 0; i < ROWS; ++i)
        assert(psx_kernel_bless_dispatchable(row_key(i)) == 1);
    assert(pstat(1) == ROWS && pstat(2) == (uint64_t)ROWS * BODY_BYTES);
    assert(pstat(3) == 0 && pstat(4) == 0);

    /* A store that does report itself is not a paranoid finding. */
    const uint32_t patched = row_body(PATCHED_ROW) + 0x10u;
    ram[patched] ^= 0xFFu;
    dirty_ram_mark_kernel_write(patched);
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 0);
    assert(pstat(3) == 0);
    ram[patched] ^= 0xFFu;
    dirty_ram_mark_kernel_write(patched);
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 1);
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW - 1u)) == 1);

    /* A write that told nobody: the row still says clean. Paranoid mode
     * counts it and interprets; other bodies stay native. */
    ram[patched] ^= 0xFFu;
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 0);
    assert(pstat(3) == 1u);
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW - 1u)) == 0);   /* same body */
    assert(pstat(3) == 2u);
    assert(psx_kernel_bless_dispatchable(row_key(100u)) == 1);

    /* The reverse: a row left at mismatch over bytes that match again. */
    ram[patched] ^= 0xFFu;
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 0);
    assert(pstat(4) == 1u);

    printf("kernel_bless_state: PASS paranoid (%llu checks)\n",
           (unsigned long long)pstat(1));
    return 0;
}

int main(int argc, char **argv) {
    uint32_t entries = 0, capacity = 0;

    if (argc > 1 && strcmp(argv[1], "overflow") == 0) {
        build_table(KBLESS_MAX_ENTRIES + 1u);
        assert(!psx_kernel_bless_table_fits(&entries, &capacity));
        assert(entries == KBLESS_MAX_ENTRIES + 1u && capacity == KBLESS_MAX_ENTRIES);
        assert(psx_kernel_bless_dispatchable(row_key(0)) == 0);
        assert(psx_kernel_bless_state() == 0);
        printf("kernel_bless_state: PASS overflow (%u rows, capacity %u: bless off)\n",
               (unsigned)entries, (unsigned)capacity);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "paranoid") == 0) return paranoid_main();

    assert(KBLESS_MAX_ENTRIES >= ROWS);
    build_table(ROWS);
    assert(psx_kernel_bless_table_fits(&entries, &capacity));
    assert(entries == ROWS && capacity == KBLESS_MAX_ENTRIES);
    assert(psx_kernel_bless_state() == -1); /* not latched before a dispatch */

    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 1);
    assert(psx_kernel_bless_state() == 1);
    for (uint32_t i = 0; i < ROWS; ++i)
        assert(psx_kernel_bless_dispatchable(row_key(i)) == 1);
    assert(stat(0) == ROWS && stat(1) == ROWS && stat(2) == 0);

    /* Not a table key, and outside the window: never native. */
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW) + 2u) == 0);
    assert(psx_kernel_bless_dispatchable(WIN_HI + 0x100u) == 0);

    /* A guest store into a body sends its rows back to the interpreter, and
     * only those rows. */
    const uint32_t patched = row_body(PATCHED_ROW) + 0x10u;
    ram[patched] ^= 0xFFu;
    kbless_note_write(patched);
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 0);
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW - 1u)) == 0);  /* same body */
    assert(psx_kernel_bless_dispatchable(row_key(100u)) == 1);   /* another body */
    assert(stat(2) == 2u);

    /* Restoring the ROM bytes makes the body native again. */
    ram[patched] ^= 0xFFu;
    kbless_note_write(patched);
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 1);
    assert(stat(2) == 0);

    /* A bulk write over the window and a state restore re-verify every row. */
    psx_kernel_bless_note_range(WIN_LO, WIN_HI - WIN_LO);
    assert(stat(1) == 0);
    assert(psx_kernel_bless_dispatchable(row_key(ROWS - 1u)) == 1);
    psx_kernel_bless_resync_after_restore();
    assert(stat(1) == 0);
    assert(psx_kernel_bless_dispatchable(row_key(3001u)) == 1);

    /* Marking a range executable writes no RAM, so no row re-verifies. The
     * generated BIOS dispatch marks 4 bytes on every RAM-alias dispatch of
     * an exception-handler key; when that reset the table, a two-minute
     * start re-verified it hundreds of thousands of times. */
    for (uint32_t i = 0; i < ROWS; ++i)
        assert(psx_kernel_bless_dispatchable(row_key(i)) == 1);
    {
        const uint64_t verifies = stat(4), invalidations = stat(5);
        assert(!page_marked(0x00000E10u));
        dirty_ram_mark_executable_range(0x00000E10u, 4u);
        assert(page_marked(0x00000E10u));          /* it still marks the page */
        if (stat(1) != ROWS || stat(5) != invalidations) {
            printf("kernel_bless_state: FAIL: marking 4 bytes executable reset "
                   "%llu of %u verified rows\n",
                   (unsigned long long)(stat(5) - invalidations), (unsigned)ROWS);
            return 1;
        }
        dirty_ram_mark_executable_range(WIN_LO, WIN_HI - WIN_LO);
        assert(stat(1) == ROWS && stat(5) == invalidations);
        for (uint32_t i = 0; i < ROWS; ++i)
            assert(psx_kernel_bless_dispatchable(row_key(i)) == 1);
        assert(stat(4) == verifies);

        /* The hook that psx_write_word/half/byte call before each RAM
         * store still sends the written body back to the interpreter. */
        ram[patched] ^= 0xFFu;
        dirty_ram_mark_kernel_write(patched);
        assert(stat(5) == invalidations + 64u);   /* that body's rows */
        assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 0);
        assert(psx_kernel_bless_dispatchable(row_key(100u)) == 1);
        ram[patched] ^= 0xFFu;
        dirty_ram_mark_kernel_write(patched);
        assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 1);

        /* A raw write that tells nobody leaves the row clean over changed
         * bytes. That is why every raw writer calls
         * psx_kernel_bless_note_range (tests/test_raw_ram_writers.py). */
        ram[patched] ^= 0xFFu;
        assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 1);
        psx_kernel_bless_note_range(patched, 1u);
        assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 0);
        ram[patched] ^= 0xFFu;
        psx_kernel_bless_note_range(patched, 1u);
        assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 1);
        assert(pstat(0) == 0 && pstat(1) == 0);    /* paranoid is opt-in */
    }

    /* PSX_KERNEL_BLESS=0 still turns it off after a re-boot latch. */
    set_env("PSX_KERNEL_BLESS", "0");
    psx_kernel_bless_reset_for_boot();
    assert(psx_kernel_bless_dispatchable(row_key(PATCHED_ROW)) == 0);
    assert(psx_kernel_bless_state() == 0);

    printf("kernel_bless_state: PASS (%u rows, capacity %u)\n",
           (unsigned)ROWS, (unsigned)KBLESS_MAX_ENTRIES);
    return 0;
}
