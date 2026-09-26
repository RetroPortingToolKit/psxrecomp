#include "psx_memory.h"

#include <stdio.h>

/* The live geometry normally lives in memory.c; this test owns it so each
 * build of the test pins one geometry the way memory_init() would. */
uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;

static int failures;

static void check(int condition, const char *name) {
    if (condition) {
        printf("PASS  %s\n", name);
    } else {
        fprintf(stderr, "FAIL  %s\n", name);
        failures++;
    }
}

int main(void) {
    uint32_t off = 0xFFFFFFFFu;

#if defined(PSX_MEMORY_GEOMETRY_TEST_EXPANDED)
    /* What the 8 MB mod leaves behind after memory_init(). */
    g_psx_ram_size = PSX_MAIN_RAM_EXPANDED_BYTES;
    g_psx_ram_mask = PSX_MAIN_RAM_EXPANDED_BYTES - 1u;
#endif

    check(psx_ram_resolve(0x80000000u, 4u, &off) && off == 0u,
          "KSEG0 aliases physical RAM");
    check(psx_ram_resolve(0xA0000100u, 4u, &off) && off == 0x100u,
          "KSEG1 aliases physical RAM");
    check(!psx_ram_resolve(0x00800000u, 4u, &off),
          "first byte beyond DRAM decode is rejected");
    check(!psx_ram_resolve(0x1F801000u, 4u, &off),
          "MMIO is never mistaken for DRAM");
    check(!psx_ram_resolve(psx_ram_live_bytes() - 2u, 4u, &off),
          "cross-geometry word is rejected");

#if !defined(PSX_MEMORY_GEOMETRY_TEST_EXPANDED)
    check(psx_ram_canonical_offset(0x80612340u) == 0x00012340u,
          "retail canonicalization folds KSEG0 mirrors");
    check(psx_ram_resolve(0x00212340u, 4u, &off) && off == 0x00012340u,
          "retail geometry folds the second mirror");
    check(psx_ram_resolve(0x807FFFFCu, 4u, &off) && off == 0x001FFFFCu,
          "retail geometry folds the fourth mirror top");
    check(psx_ram_map_read(0x80780000u) == 0x00180000u,
          "retail bytes of a fourth-mirror PC are the folded DRAM");
    check(psx_ram_live_bytes() == PSX_MAIN_RAM_RETAIL_BYTES,
          "retail live size is 2 MiB");
#else
    check(psx_ram_canonical_offset(0x80612340u) == 0x00612340u,
          "expanded canonicalization preserves all decoded address bits");
    check(psx_ram_resolve(0x00212340u, 4u, &off) && off == 0x00212340u,
          "expanded geometry uniquely decodes the second bank");
    check(psx_ram_resolve(0x807FFFFCu, 4u, &off) && off == 0x007FFFFCu,
          "expanded geometry uniquely decodes the eighth MiB top");
    check(psx_ram_map_read(0x80780000u) == 0x00780000u,
          "expanded bytes of a high-bank PC stay in the high bank");
    check(psx_ram_live_bytes() == PSX_MAIN_RAM_EXPANDED_BYTES,
          "expanded live size is 8 MiB");
#endif

    return failures ? 1 : 0;
}
