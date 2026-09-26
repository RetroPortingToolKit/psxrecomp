#ifndef PSXRECOMP_PSX_MEMORY_H
#define PSXRECOMP_PSX_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One main-RAM geometry contract for every runtime subsystem.
 *
 * Retail DRAM is 2 MiB, mirrored four times across the 8 MiB decode window
 * (physical 0x00000000-0x007FFFFF of every segment). The opt-in 8 MB main RAM
 * enhancement (builtin mod psx.enhancement.8mb-ram) switches the LIVE geometry
 * to unique decoding of the whole window before memory_init(); with the mod
 * off the live geometry is retail and every helper below folds exactly as
 * retail hardware does. The geometry is one mask, latched once per boot: the
 * retail fold is `phys & 0x1FFFFF`, the expanded map `phys & 0x7FFFFF`.
 *
 * Two identities, never mixed:
 *   BYTES -- where an address's data lives: psx_ram_map_read /
 *     psx_ram_canonical_offset / psx_ram_resolve fold through the live mask.
 *     Every RAM access, DMA cursor, dirty/watch page and code-range CRC uses it.
 *   CODE  -- which compiled body may run at a PC: the segment-stripped address
 *     (addr & 0x1FFFFFFF), NEVER mirror-folded. A compiled function bakes its
 *     own PCs into $ra/EPC/branch targets, so running the body compiled for
 *     0x80180000 at a 0x80780000 PC would hand the guest a different $ra than
 *     hardware does. Architectural state (PC, $ra, EPC) is never folded; a PC
 *     in a retail mirror executes the folded bytes with its own address.
 *     (One pre-existing exception is kept bit-identical: the BIOS dispatch
 *     table's emitted normalize() still folds retail mirrors onto its RAM
 *     keys, via `& g_psx_ram_mask`; the 8 MB map does not fold.)
 *
 * Host storage indexed by a RAM offset is sized with PSX_MAIN_RAM_BACKING_BYTES
 * (the largest geometry); bounds that describe the guest-visible RAM use the
 * live size (psx_ram_live_bytes()). Serialized/digested RAM state covers the
 * live size only, so retail savestates and netplay digests are unchanged. */
#define PSX_MAIN_RAM_RETAIL_BYTES   0x00200000u
#define PSX_MAIN_RAM_EXPANDED_BYTES 0x00800000u
#define PSX_MAIN_RAM_WINDOW_BYTES   0x00800000u
#define PSX_MAIN_RAM_BACKING_BYTES  PSX_MAIN_RAM_EXPANDED_BYTES

/* Live geometry (psx_ram_geometry.c). Retail until a trusted activation plugin
 * requests expanded RAM (psx_mod_set_main_ram_8mb) and memory_init() applies
 * it. g_psx_ram_mask == g_psx_ram_size - 1 always. */
extern uint32_t g_psx_ram_size;
extern uint32_t g_psx_ram_mask;

uint32_t memory_get_ram_bytes(void);
int      psx_ram_8mb_active(void);
void     psx_ram_reset_size_request(void);
/* memory_init(): latch the requested geometry for this boot. */
void     psx_ram_apply_size_request(void);

static inline uint32_t psx_ram_live_bytes(void) { return g_psx_ram_size; }

/* BYTE offset of a physical address. Inside the decode window it folds through
 * the live mask; addresses at/above the window are returned unchanged (not
 * RAM), so callers may pass any physical address. */
static inline uint32_t psx_ram_map_read(uint32_t phys) {
    phys &= 0x1FFFFFFFu;
    return phys < PSX_MAIN_RAM_WINDOW_BYTES ? (phys & g_psx_ram_mask) : phys;
}

static inline uint32_t psx_ram_map_write(uint32_t phys) {
    return psx_ram_map_read(phys);
}

/* Strip KUSEG/KSEG0/KSEG1 and fold within the 8 MiB DRAM decode window through
 * the live mask. Bits above the window are ignored (DMAC-style 24-bit keys). */
static inline uint32_t psx_ram_canonical_offset(uint32_t address) {
    return address & (PSX_MAIN_RAM_WINDOW_BYTES - 1u) & g_psx_ram_mask;
}

/* Resolve [address, address+width) to a live RAM offset. Fails for anything
 * outside the decode window, zero width, or a span past the live RAM end
 * (retail: a span crossing a 2 MiB mirror boundary has no contiguous bytes). */
static inline int psx_ram_resolve(uint32_t address, uint32_t width,
                                  uint32_t *offset) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    uint32_t off;
    if (phys >= PSX_MAIN_RAM_WINDOW_BYTES || width == 0u) return 0;
    off = phys & g_psx_ram_mask;
    if (width > g_psx_ram_size - off) return 0;
    if (offset) *offset = off;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
