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
 * to unique decoding of registered high pages before memory_init(); with the
 * mod off the live geometry is retail and every helper below folds exactly as
 * retail hardware does.
 *
 * Host storage indexed by a RAM offset is sized with PSX_MAIN_RAM_BACKING_BYTES
 * (the largest geometry); bounds that describe the guest-visible RAM use the
 * live size (psx_ram_live_bytes()). Serialized/digested RAM state covers the
 * live size only, so retail savestates and netplay digests are unchanged. */
#define PSX_MAIN_RAM_RETAIL_BYTES   0x00200000u
#define PSX_MAIN_RAM_EXPANDED_BYTES 0x00800000u
#define PSX_MAIN_RAM_WINDOW_BYTES   0x00800000u
#define PSX_MAIN_RAM_BACKING_BYTES  PSX_MAIN_RAM_EXPANDED_BYTES

/* Live geometry (memory.c). Retail until a trusted activation plugin requests
 * expanded RAM (psx_mod_set_main_ram_8mb) and memory_init() applies it. */
extern uint32_t g_psx_ram_size;
extern uint32_t g_psx_ram_mask;

uint32_t memory_get_ram_bytes(void);
int      psx_ram_8mb_active(void);
void     psx_ram_reset_size_request(void);
/* memory_init(): latch the requested geometry for this boot. */
void     psx_ram_apply_size_request(void);

/* High pages [2 MiB, 8 MiB) decode uniquely only while registered; unregistered
 * high pages keep aliasing the low 2 MiB even in expanded mode. */
#define PSX_RAM_HIGH_PAGE0    (PSX_MAIN_RAM_RETAIL_BYTES >> 12)
#define PSX_RAM_HIGH_PAGES    \
    ((PSX_MAIN_RAM_EXPANDED_BYTES - PSX_MAIN_RAM_RETAIL_BYTES) >> 12)
#define PSX_RAM_HIGH_BITWORDS ((PSX_RAM_HIGH_PAGES + 31u) / 32u)
extern uint32_t g_psx_ram_high_unique[PSX_RAM_HIGH_BITWORDS];

static inline uint32_t psx_ram_live_bytes(void) { return g_psx_ram_size; }

static inline int psx_ram_high_page_unique(uint32_t page) {
    uint32_t i, bit;
    if (page < PSX_RAM_HIGH_PAGE0 ||
        page >= (PSX_MAIN_RAM_EXPANDED_BYTES >> 12))
        return 1;
    i = page - PSX_RAM_HIGH_PAGE0;
    bit = 1u << (i & 31u);
    return (g_psx_ram_high_unique[i >> 5] & bit) != 0;
}

/* Canonical RAM offset of a physical address inside the decode window.
 * Addresses at/above the window are returned unchanged (not RAM). */
static inline uint32_t psx_ram_map_read(uint32_t phys) {
    phys &= 0x1FFFFFFFu;
    if (phys >= PSX_MAIN_RAM_WINDOW_BYTES)
        return phys;
    if (g_psx_ram_size <= PSX_MAIN_RAM_RETAIL_BYTES)
        return phys & (PSX_MAIN_RAM_RETAIL_BYTES - 1u);
    if (phys < PSX_MAIN_RAM_RETAIL_BYTES)
        return phys;
    if (psx_ram_high_page_unique(phys >> 12))
        return phys;
    return phys & (PSX_MAIN_RAM_RETAIL_BYTES - 1u);
}

static inline uint32_t psx_ram_map_write(uint32_t phys) {
    return psx_ram_map_read(phys);
}

/* Strip KUSEG/KSEG0/KSEG1 and canonicalize within the 8 MiB DRAM decode
 * window through the live map. Retail folds all four aliases; expanded mode
 * preserves every decoded bit of a registered high page. */
static inline uint32_t psx_ram_canonical_offset(uint32_t address) {
    return psx_ram_map_read((address & 0x1FFFFFFFu) &
                            (PSX_MAIN_RAM_WINDOW_BYTES - 1u));
}

/* Resolve [address, address+width) to a live RAM offset. Fails for anything
 * outside the decode window, zero width, or a span past the live RAM end. */
static inline int psx_ram_resolve(uint32_t address, uint32_t width,
                                  uint32_t *offset) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    uint32_t off;
    if (phys >= PSX_MAIN_RAM_WINDOW_BYTES || width == 0u) return 0;
    off = psx_ram_map_read(phys);
    if (off >= g_psx_ram_size || width > g_psx_ram_size - off) return 0;
    if (offset) *offset = off;
    return 1;
}

/* Code-address form: keep the segment bits, fold only aliased mirrors. */
static inline uint32_t psx_ram_canon_code_addr_inline(uint32_t addr) {
    uint32_t seg = addr & 0xE0000000u;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES && phys >= PSX_MAIN_RAM_RETAIL_BYTES &&
        (g_psx_ram_size <= PSX_MAIN_RAM_RETAIL_BYTES ||
         !psx_ram_high_page_unique(phys >> 12)))
        phys &= (PSX_MAIN_RAM_RETAIL_BYTES - 1u);
    return seg | phys;
}

void     psx_ram_register_unique(uint32_t addr, uint32_t len);
uint32_t psx_ram_canon_code_addr(uint32_t addr);
/* After a bulk RAM restore into `ram` (the host backing). */
void     psx_ram_resync_high_after_restore(const uint8_t *ram);

#ifdef __cplusplus
}
#endif

#endif
