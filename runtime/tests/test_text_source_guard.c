/* test_text_source_guard.c — the loaded boot executable is compared, page by
 * page, with the image the static code was generated from, and native text
 * dispatch is refused on every page that differs.
 *
 * Shapes taken from measured multi-disc sets (no disc data here; the images
 * are synthetic):
 *   - one instruction byte differs between the discs (Star Wars: Rebel
 *     Assault II loads the disc number with a different immediate);
 *   - one string byte differs (Metal Gear Solid Europe);
 *   - a block straddles a page boundary at a load address that is not
 *     page-aligned (Dragon Warrior VII).
 *
 * Cases 1-9 exercise psx_text_source_mismatch (text_source_guard.c). Case 10
 * runs the real memory.c guard: with the mounted disc's executable as the
 * reference image, RAM and reference agree, so only the generated-from page
 * CRCs show that a page is not the code the static functions were built from.
 *
 * test_text_source_guard.py builds it; link seams that this path does not
 * reach are stubs that abort (fixture_link.py). */
#define PSX_ENABLE_BLOCK_CYCLES 1
#include "cpu_state.h"
#include "crc32.h"
#include "dirty_ram_interp.h"
#include "kernel_patch_ranges.h"
#include "psx_bios_image.h"
#include "text_source_guard.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what memory.c reads from the rest of the runtime ------------------- */
void (*g_overlay_flush_pending_cycles)(void);
uint32_t g_debug_last_store_pc, g_debug_current_func_addr;
uint32_t g_overlay_region_floor, g_text_image_lo;
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS];
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
int g_dma_exec_depth;
int g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr, g_dma_cur_bcr, g_dma_initiator_pc;
uint32_t g_dma_cpu_read_wait;
volatile int g_ds_recording;
int g_ls_mode, g_ls_replay_active, g_ls_suppress_record;
int g_ram_read_watch_active, g_event_step_conservative, g_insn_log_frozen;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
int g_psx_cyc_bb_defer;
uint32_t *g_psx_cyc_local_acc;
uint64_t g_psx_bail_flattened, g_psx_device_gen, psx_cycle_count, s_frame_count;
uint64_t psx_next_service_cycle;
int psx_in_device_service;
uint32_t g_psx_icache_tv[1024];
CPUState *debug_cpu_ptr;
PsxBiosImageInfo psx_bios_image;
const PsxKernelBody *psx_bios_kernel_bodies;
uint32_t psx_bios_kernel_body_count;
const PsxKernelPatchRange *psx_bios_kernel_patch_ranges;
uint32_t psx_bios_kernel_patch_range_count;

int source_gpu_runtime_active(void) { return 0; }
int fntrace_is_game_started(void) { return 1; }
int psx_get_in_exception(void) { return 0; }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }

/* Not assert(): a Release build defines NDEBUG, and the calls under test sit
 * inside the checks. */
static int s_fails = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); s_fails++; } } while (0)

#define PAGE 4096u
#define WORDS 16u /* 512 pages: the whole 2 MB of PS1 RAM */

/* Per-page CRCs of an image, as the emitter writes them. */
static uint32_t page_crcs(const uint8_t *image, uint32_t lo, uint32_t len,
                          uint32_t *out, uint32_t cap)
{
    uint32_t pos = 0, addr = lo, n = 0;
    while (pos < len) {
        uint32_t take = PAGE - (addr & (PAGE - 1u));
        if (take > len - pos) take = len - pos;
        CHECK(n < cap);
        out[n++] = crc32_compute(image + pos, take);
        pos += take;
        addr += take;
    }
    return n;
}

static uint8_t *make_image(uint32_t len, uint32_t salt)
{
    uint8_t *image = (uint8_t *)malloc(len);
    uint32_t i;
    CHECK(image);
    for (i = 0; i < len; i++)
        image[i] = (uint8_t)((i * 2654435761u + salt * 40503u) >> 13);
    return image;
}

static int bit(const uint32_t *bitmap, uint32_t phys)
{
    const uint32_t page = phys >> 12;
    return (bitmap[page >> 5] >> (page & 31u)) & 1u;
}

static uint32_t count_bits(const uint32_t *bitmap)
{
    uint32_t n = 0, i, b;
    for (i = 0; i < WORDS; i++)
        for (b = 0; b < 32u; b++)
            n += (bitmap[i] >> b) & 1u;
    return n;
}

/* 10. The memory.c guard refuses native dispatch on the differing page. */
static void dispatch_refused_on_foreign_page(void)
{
    const uint32_t lo = 0x00010000u, len = 0x3000u;
    uint8_t *source = make_image(len, 3);
    uint8_t *disc2 = (uint8_t *)malloc(len);
    uint32_t crcs[3];
    uint32_t first = 0xFFFFFFFFu, n;
    /* {virtual lo, byte len}: one range inside page 0x10000, one that
     * crosses from page 0x10000 into the differing page 0x11000. */
    const uint32_t inside[2] = { 0x80010100u, 0x40u };
    const uint32_t crossing[2] = { 0x80010FF0u, 0x20u };

    /* The boot text [0x10000, 0x100000) as main.cpp sets it up: pages
     * there are not overlay RAM, so an untouched page is clean. */
    g_text_image_lo = 0x00010000u;
    g_overlay_region_floor = 0x00100000u;
    CHECK(disc2);
    n = page_crcs(source, lo, len, crcs, 3);
    CHECK(n == 3u);
    memcpy(disc2, source, len);
    disc2[0x00011AFCu - lo] ^= 0x03u; /* the immediate of one instruction */

    /* An installed product: the reference is the mounted disc's executable,
     * so RAM and reference agree and the page looks clean. */
    dirty_ram_register_text_image(lo, disc2, len);
    CHECK(dirty_ram_text_native_ok(0x00011AFCu) == 1);

    CHECK(dirty_ram_text_note_source_image(disc2, lo, len, crcs, n, lo, len,
                                           &first) == 1);
    CHECK(first == 0x00011000u);
    CHECK(dirty_ram_text_foreign_pages() == 1u);
    CHECK(dirty_ram_text_native_ok(0x00011AFCu) == 0);
    CHECK(dirty_ram_text_native_ok(0x00011000u) == 0);
    CHECK(dirty_ram_text_native_ok(0x00010004u) == 1);
    CHECK(dirty_ram_text_native_ok(0x00012004u) == 1);
    CHECK(dirty_ram_text_native_ok_ranges_from(inside, 1, inside[0]) == 1);
    CHECK(dirty_ram_text_native_ok_ranges_from(crossing, 1, crossing[0]) == 0);

    /* The disc the build was generated from: nothing is refused. */
    dirty_ram_register_text_image(lo, source, len);
    CHECK(dirty_ram_text_foreign_pages() == 0u);
    CHECK(dirty_ram_text_note_source_image(source, lo, len, crcs, n, lo, len,
                                           &first) == 0);
    CHECK(dirty_ram_text_native_ok(0x00011AFCu) == 1);
    CHECK(dirty_ram_text_native_ok_ranges_from(crossing, 1, crossing[0]) == 1);

    /* No table (a dispatcher generated before it existed): as before. */
    dirty_ram_register_text_image(lo, disc2, len);
    CHECK(dirty_ram_text_note_source_image(disc2, lo, len, NULL, 0, 0, 0,
                                           &first) == 0);
    CHECK(dirty_ram_text_native_ok(0x00011AFCu) == 1);
    /* source and disc2 stay registered with the guard for the process. */
}

int main(void)
{
    uint32_t crcs[600];
    uint32_t bitmap[WORDS];
    uint32_t first;

    /* The table format is zlib's CRC-32, as the emitter computes it. */
    CHECK(crc32_compute((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    /* 1. The disc the build was generated from: nothing differs. */
    {
        const uint32_t lo = 0x00010000u, len = 0xA9800u;
        uint8_t *source = make_image(len, 1);
        const uint32_t n = page_crcs(source, lo, len, crcs, 600);
        CHECK(n == 170u); /* 169 full pages and one half page */
        memset(bitmap, 0, sizeof(bitmap));
        first = 0xFFFFFFFFu;
        CHECK(psx_text_source_mismatch(source, lo, len, crcs, n, lo, len,
                                        bitmap, WORDS, &first) == 0);
        CHECK(count_bits(bitmap) == 0 && first == 0);

        /* 2. One instruction byte differs: `ori v0,zero,1` became
         *    `ori v0,zero,2` at 0x80032AFC. Exactly that page is marked. */
        {
            uint8_t *disc2 = (uint8_t *)malloc(len);
            CHECK(disc2);
            memcpy(disc2, source, len);
            disc2[0x00032AFCu - lo] ^= 0x03u;
            CHECK(psx_text_source_mismatch(disc2, lo, len, crcs, n, lo, len,
                                            bitmap, WORDS, &first) == 1);
            CHECK(count_bits(bitmap) == 1);
            CHECK(bit(bitmap, 0x00032AFCu) && first == 0x00032000u);
            CHECK(!bit(bitmap, 0x00031FFCu) && !bit(bitmap, 0x00033000u));
            /* Marking is idempotent: the same page is not counted twice. */
            CHECK(psx_text_source_mismatch(disc2, lo, len, crcs, n, lo, len,
                                            bitmap, WORDS, &first) == 0);
            CHECK(count_bits(bitmap) == 1);

            /* 3. A second difference, one string byte in the last (partial)
             *    page, marks that page too. */
            disc2[len - 5u] ^= 0x01u;
            memset(bitmap, 0, sizeof(bitmap));
            CHECK(psx_text_source_mismatch(disc2, lo, len, crcs, n, lo, len,
                                            bitmap, WORDS, &first) == 2);
            CHECK(bit(bitmap, 0x00032AFCu) && bit(bitmap, lo + len - 5u));
            CHECK(first == 0x00032000u);
            free(disc2);
        }

        /* 4. A loaded image longer than the generated-from image: the extra
         *    bytes have no static code and are not compared. */
        {
            uint8_t *longer = (uint8_t *)malloc(len + 0x800u);
            CHECK(longer);
            memcpy(longer, source, len);
            memset(longer + len, 0xEE, 0x800u);
            memset(bitmap, 0, sizeof(bitmap));
            CHECK(psx_text_source_mismatch(longer, lo, len + 0x800u, crcs, n,
                                            lo, len, bitmap, WORDS, &first) == 0);
            free(longer);
        }

        /* 5. A loaded image shorter than the generated-from image: the pages
         *    it does not fully cover are marked. */
        memset(bitmap, 0, sizeof(bitmap));
        CHECK(psx_text_source_mismatch(source, lo, len - 0x1800u, crcs, n, lo,
                                        len, bitmap, WORDS, &first) == 2);
        CHECK(bit(bitmap, lo + len - 1u) && bit(bitmap, lo + len - 0x1800u));
        CHECK(!bit(bitmap, lo + len - 0x2000u));

        /* 6. Another load address is another program: every page of the
         *    loaded image is marked. */
        memset(bitmap, 0, sizeof(bitmap));
        CHECK(psx_text_source_mismatch(source, lo + 0x1000u, 0x3000u, crcs, n,
                                        lo, len, bitmap, WORDS, &first) == 3);
        CHECK(bit(bitmap, lo + 0x1000u) && bit(bitmap, lo + 0x3FFFu));
        CHECK(!bit(bitmap, lo) && !bit(bitmap, lo + 0x4000u));
        CHECK(first == lo + 0x1000u);

        /* 7. No table: a dispatcher generated before the table existed. */
        memset(bitmap, 0, sizeof(bitmap));
        CHECK(psx_text_source_mismatch(source, lo, len, NULL, 0, 0, 0, bitmap,
                                        WORDS, &first) == 0);
        CHECK(psx_text_source_mismatch(source, lo, len, crcs, 0, lo, len,
                                        bitmap, WORDS, &first) == 0);
        CHECK(count_bits(bitmap) == 0);
        free(source);
    }

    /* 8. A load address that is not page-aligned (0x80017F00): the first page
     *    holds 0x100 bytes. A block that straddles a page boundary marks both
     *    pages; a byte in the short first page marks the first page. */
    {
        const uint32_t lo = 0x00017F00u, len = 0xA4800u;
        uint8_t *source = make_image(len, 2);
        uint8_t *disc2 = (uint8_t *)malloc(len);
        const uint32_t n = page_crcs(source, lo, len, crcs, 600);
        uint32_t i;
        CHECK(disc2 && n == 166u);
        memcpy(disc2, source, len);
        for (i = 0; i < 0xC00u; i++)
            disc2[(0x000B5700u - lo) + i] ^= 0x5Au;
        memset(bitmap, 0, sizeof(bitmap));
        CHECK(psx_text_source_mismatch(disc2, lo, len, crcs, n, lo, len,
                                        bitmap, WORDS, &first) == 2);
        CHECK(bit(bitmap, 0x000B5700u) && bit(bitmap, 0x000B6000u));
        CHECK(!bit(bitmap, 0x000B4FFCu) && !bit(bitmap, 0x000B7000u));
        CHECK(first == 0x000B5000u);

        disc2[0xFFu] ^= 0x01u; /* 0x80017FFF: last byte of the short first page */
        CHECK(psx_text_source_mismatch(disc2, lo, len, crcs, n, lo, len,
                                        bitmap, WORDS, &first) == 1);
        CHECK(bit(bitmap, 0x00017F00u) && !bit(bitmap, 0x00018000u));
        free(disc2);
        free(source);
    }

    /* 9. Bad arguments mark nothing. */
    memset(bitmap, 0, sizeof(bitmap));
    crcs[0] = 0;
    CHECK(psx_text_source_mismatch(NULL, 0x10000u, 16u, crcs, 1, 0x10000u, 16u,
                                    bitmap, WORDS, &first) == 0);
    CHECK(psx_text_source_mismatch((const uint8_t *)"x", 0x10000u, 0u, crcs, 1,
                                    0x10000u, 16u, bitmap, WORDS, &first) == 0);
    CHECK(count_bits(bitmap) == 0);

    dispatch_refused_on_foreign_page();

    if (s_fails) {
        fprintf(stderr, "text_source_guard_test: %d check(s) failed\n", s_fails);
        return 1;
    }
    printf("text_source_guard_test: 10 cases passed\n");
    return 0;
}
