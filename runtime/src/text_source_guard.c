/* text_source_guard.c — see text_source_guard.h. */

#include "text_source_guard.h"

#include <stddef.h>

#include "crc32.h"

#define PAGE_BYTES (1u << PSX_TEXT_SOURCE_PAGE_SHIFT)

static void mark_page(uint32_t phys, uint32_t *bitmap, uint32_t bitmap_words,
                      uint32_t *marked, uint32_t *first_phys, int *have_first)
{
    const uint32_t page = phys >> PSX_TEXT_SOURCE_PAGE_SHIFT;
    const uint32_t bit = 1u << (page & 31u);
    if ((page >> 5) >= bitmap_words) return;
    if (bitmap[page >> 5] & bit) return;
    bitmap[page >> 5] |= bit;
    (*marked)++;
    if (!*have_first) {
        *have_first = 1;
        if (first_phys) *first_phys = page << PSX_TEXT_SOURCE_PAGE_SHIFT;
    }
}

uint32_t psx_text_source_mismatch(const uint8_t *image, uint32_t phys_lo,
                                  uint32_t len,
                                  const uint32_t *source_crc32,
                                  uint32_t source_count,
                                  uint32_t source_phys_lo, uint32_t source_len,
                                  uint32_t *bitmap, uint32_t bitmap_words,
                                  uint32_t *first_phys)
{
    uint32_t marked = 0;
    int have_first = 0;

    if (first_phys) *first_phys = 0;
    if (!image || len == 0 || !bitmap || bitmap_words == 0) return 0;
    if (!source_crc32 || source_count == 0 || source_len == 0) return 0;

    if (phys_lo != source_phys_lo) {
        /* Another program: nothing of the static code can be trusted. */
        uint32_t addr = phys_lo;
        uint32_t left = len;
        while (left) {
            uint32_t take = PAGE_BYTES - (addr & (PAGE_BYTES - 1u));
            if (take > left) take = left;
            mark_page(addr, bitmap, bitmap_words, &marked, first_phys,
                      &have_first);
            addr += take;
            left -= take;
        }
        return marked;
    }

    {
        uint32_t pos = 0;
        uint32_t addr = source_phys_lo;
        uint32_t i;
        for (i = 0; i < source_count && pos < source_len; i++) {
            uint32_t take = PAGE_BYTES - (addr & (PAGE_BYTES - 1u));
            if (take > source_len - pos) take = source_len - pos;
            if (pos + take > len ||
                crc32_compute(image + pos, (size_t)take) != source_crc32[i])
                mark_page(addr, bitmap, bitmap_words, &marked, first_phys,
                          &have_first);
            pos += take;
            addr += take;
        }
    }
    return marked;
}
