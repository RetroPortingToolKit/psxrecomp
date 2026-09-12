#include "mod_memory.h"

#include <stdio.h>

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
    uint32_t off = 0;
    uint32_t base = PSX_MOD_GPU_DMA_APERTURE_BASE;
    uint32_t size = PSX_MOD_GPU_DMA_APERTURE_SIZE;
    uint32_t aliases[] = {0x1FC00000u, 0x9FC00000u, 0xBFC00000u,
                          0x1F000000u, 0x9F000000u, 0x1F801810u,
                          0xBF801810u, 0x1EFFFFFFu};
    unsigned i;

    check(psx_mod_gpu_dma_resolve_address_for(base + 0x1234u, 0u) ==
              ((base + 0x1234u) & 0x001FFFFCu),
          "unallocated aperture preserves retail 2 MiB folding");
    check(psx_mod_gpu_dma_resolve_address_for(0x80000000u + base + 0x1234u, 0x20000u) ==
              base + 0x1234u,
          "allocated KSEG0 aperture pointer survives 24-bit DMA");
    check(psx_mod_gpu_dma_resolve_address_for(base + 0x30000u, 0x20000u) ==
              ((base + 0x30000u) & 0x001FFFFCu),
          "unallocated aperture tail remains folded");
    check(psx_mod_gpu_dma_resolve_address_for(0x001ABCDEu, 0x20000u) ==
              0x001ABCDCu,
          "ordinary main RAM remains word-aligned and unchanged");
    check(psx_mod_gpu_dma_aperture_offset_for(
              base + 0x1FFFCu, 4u, 0x20000u, &off) &&
              off == 0x1FFFCu,
          "last allocated aperture word is accessible");
    check(!psx_mod_gpu_dma_aperture_offset_for(
              base + 0x20000u, 4u, 0x20000u, &off),
          "first unallocated aperture word is inaccessible");
    check(!psx_mod_gpu_dma_aperture_offset_for(
              0x00FFFFFCu, 8u, PSX_MOD_GPU_DMA_APERTURE_SIZE, &off),
          "cross-boundary access is rejected");
    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
        check(!psx_mod_gpu_dma_aperture_offset_for(aliases[i], 4u, size, &off),
              "CPU BIOS/expansion/MMIO addresses cannot alias DMA storage");
    check(psx_mod_gpu_dma_aperture_offset_for(0xA0000000u + base, 4u, size, &off)
              && off == 0u, "KSEG1 CPU aperture alias resolves correctly");
    check(psx_mod_gpu_dma_aperture_offset_for(base + size - 4u, 4u, size, &off)
              && off == size - 4u, "last word of full 4 MiB aperture is accessible");
    check(!psx_mod_gpu_dma_aperture_offset_for(base - 4u, 4u, size, &off),
          "word before aperture is rejected");
    check(!psx_mod_gpu_dma_aperture_offset_for(base + size, 4u, size, &off),
          "word after aperture is rejected");
    check(!psx_mod_gpu_dma_aperture_offset_for(base, 4u, size + 4u, &off),
          "invalid allocated size is rejected");
    check(psx_mod_gpu_dma_resolve_address_for(base + size - 4u, size) ==
              base + size - 4u, "full aperture DMA tag keeps its high address bits");

    return failures ? 1 : 0;
}
