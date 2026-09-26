/* psx_ram_geometry.c -- live main-RAM geometry state (see psx_memory.h).
 *
 * Retail 2 MiB mirroring is the default and the only geometry reachable
 * without the opt-in 8 MB main RAM mod. A trusted activation plugin requests
 * expanded RAM (psx_mod_set_main_ram_8mb) before memory_init(), which applies
 * the request exactly once per boot (psx_ram_apply_size_request). Keeping the
 * state machine here, free of memory.c's device dependencies, lets the retail
 * guarantee be unit-tested against the same code the runtime links. */
#include "psx_memory.h"
#include "mod_plugins.h"

#include <stdint.h>
#include <string.h>

uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;
uint32_t g_psx_ram_high_unique[PSX_RAM_HIGH_BITWORDS];

static int s_ram_8mb_requested;
static uint32_t s_ram_high_registered[PSX_RAM_HIGH_BITWORDS];

static inline void psx_ram_high_page_mark(uint32_t page) {
    uint32_t i, bit;
    if (page < PSX_RAM_HIGH_PAGE0 ||
        page >= (PSX_MAIN_RAM_EXPANDED_BYTES >> 12))
        return;
    i = page - PSX_RAM_HIGH_PAGE0;
    bit = 1u << (i & 31u);
    g_psx_ram_high_unique[i >> 5] |= bit;
    s_ram_high_registered[i >> 5] |= bit;
}

void psx_ram_register_unique(uint32_t addr, uint32_t len) {
    uint32_t phys, end, page, last;
    if (len == 0u)
        return;
    phys = addr & 0x1FFFFFFFu;
    if (phys >= PSX_MAIN_RAM_WINDOW_BYTES)
        return;
    end = phys + len;
    if (end < phys || end > PSX_MAIN_RAM_WINDOW_BYTES)
        end = PSX_MAIN_RAM_WINDOW_BYTES;
    if (end <= PSX_MAIN_RAM_RETAIL_BYTES)
        return;
    if (phys < PSX_MAIN_RAM_RETAIL_BYTES)
        phys = PSX_MAIN_RAM_RETAIL_BYTES;
    page = phys >> 12;
    last = (end - 1u) >> 12;
    for (; page <= last; page++)
        psx_ram_high_page_mark(page);
}

uint32_t psx_ram_canon_code_addr(uint32_t addr) {
    return psx_ram_canon_code_addr_inline(addr);
}

void psx_ram_resync_high_after_restore(const uint8_t *ram) {
    uint32_t page;
    if (g_psx_ram_size <= PSX_MAIN_RAM_RETAIL_BYTES) {
        memset(g_psx_ram_high_unique, 0, sizeof(g_psx_ram_high_unique));
        return;
    }
    memcpy(g_psx_ram_high_unique, s_ram_high_registered,
           sizeof(g_psx_ram_high_unique));
    if (!ram) return;
    for (page = PSX_RAM_HIGH_PAGE0;
         page < (PSX_MAIN_RAM_EXPANDED_BYTES >> 12); page++) {
        uint32_t dst = page << 12;
        uint32_t src = dst & (PSX_MAIN_RAM_RETAIL_BYTES - 1u);
        if (psx_ram_high_page_unique(page))
            continue;
        if (memcmp(ram + dst, ram + src, 4096u) != 0)
            psx_ram_high_page_mark(page);
    }
}

uint32_t memory_get_ram_bytes(void) { return g_psx_ram_size; }
int psx_ram_8mb_active(void) {
    return g_psx_ram_size > PSX_MAIN_RAM_RETAIL_BYTES;
}

void psx_ram_reset_size_request(void) {
    s_ram_8mb_requested = 0;
    memset(s_ram_high_registered, 0, sizeof(s_ram_high_registered));
    memset(g_psx_ram_high_unique, 0, sizeof(g_psx_ram_high_unique));
}

int psx_mod_set_main_ram_8mb(int enabled) {
    s_ram_8mb_requested = enabled ? 1 : 0;
    memset(s_ram_high_registered, 0, sizeof(s_ram_high_registered));
    memset(g_psx_ram_high_unique, 0, sizeof(g_psx_ram_high_unique));
    if (enabled)
        psx_ram_register_unique(PSX_MAIN_RAM_RETAIL_BYTES,
                                PSX_MAIN_RAM_EXPANDED_BYTES -
                                    PSX_MAIN_RAM_RETAIL_BYTES);
    return 1;
}

static int psx_ram_any_high_registered(void) {
    uint32_t i;
    for (i = 0; i < PSX_RAM_HIGH_BITWORDS; i++) {
        if (s_ram_high_registered[i] != 0u)
            return 1;
    }
    return 0;
}

void psx_ram_apply_size_request(void) {
    if (s_ram_8mb_requested) {
        g_psx_ram_size = PSX_MAIN_RAM_EXPANDED_BYTES;
        g_psx_ram_mask = PSX_MAIN_RAM_EXPANDED_BYTES - 1u;
        if (!psx_ram_any_high_registered())
            psx_ram_register_unique(PSX_MAIN_RAM_RETAIL_BYTES,
                                    PSX_MAIN_RAM_EXPANDED_BYTES -
                                        PSX_MAIN_RAM_RETAIL_BYTES);
        memcpy(g_psx_ram_high_unique, s_ram_high_registered,
               sizeof(g_psx_ram_high_unique));
    } else {
        g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
        g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;
        memset(g_psx_ram_high_unique, 0, sizeof(g_psx_ram_high_unique));
    }
}
