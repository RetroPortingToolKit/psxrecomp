/* Live main-RAM geometry: links the runtime's own psx_ram_geometry.c so the
 * state machine memory_init() drives is the thing under test.
 *
 * The load-bearing claim is the opt-in guarantee: unless the 8 MB mod asks for
 * expanded RAM before memory_init(), every address in the 8 MiB decode window
 * folds onto the 2 MiB retail DRAM exactly as hardware mirrors it, for loads,
 * stores, code addresses and span resolution alike. */
#include "psx_memory.h"
#include "mod_plugins.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_ram[PSX_MAIN_RAM_BACKING_BYTES];

static int failures;

static int check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
        return 0;
    }
    return 1;
}

/* Every page of the decode window, at its first, middle and last word, through
 * all three CPU segments: the retail fold must be exact everywhere. */
static void check_retail_everywhere(const char *phase) {
    static const uint32_t segs[3] = { 0x00000000u, 0x80000000u, 0xA0000000u };
    static const uint32_t offs[3] = { 0x000u, 0x7F0u, 0xFFCu };
    uint32_t page, s, o, bad = 0;
    for (page = 0; page < (PSX_MAIN_RAM_WINDOW_BYTES >> 12); page++) {
        for (o = 0; o < 3; o++) {
            uint32_t phys = (page << 12) | offs[o];
            uint32_t want = phys & (PSX_MAIN_RAM_RETAIL_BYTES - 1u);
            uint32_t off = 0xFFFFFFFFu;
            if (psx_ram_map_read(phys) != want) bad++;
            if (psx_ram_map_write(phys) != want) bad++;
            for (s = 0; s < 3; s++) {
                uint32_t va = segs[s] | phys;
                if (psx_ram_canonical_offset(va) != want) bad++;
                if (!psx_ram_resolve(va, 4u, &off) || off != want) bad++;
                if (psx_ram_canon_code_addr(va) != (segs[s] | want)) bad++;
            }
        }
    }
    if (bad) fprintf(stderr, "  %s: %u retail-fold violations\n", phase, bad);
    check(bad == 0u, phase);
    check(memory_get_ram_bytes() == PSX_MAIN_RAM_RETAIL_BYTES, phase);
    check(!psx_ram_8mb_active(), phase);
    check(!psx_ram_resolve(0x801FFFFEu, 4u, 0), "retail span past 2 MiB end");
}

static void check_expanded_everywhere(const char *phase) {
    uint32_t page, bad = 0;
    for (page = 0; page < (PSX_MAIN_RAM_WINDOW_BYTES >> 12); page++) {
        uint32_t phys = (page << 12) | 0x7F0u;
        uint32_t off = 0xFFFFFFFFu;
        if (psx_ram_map_read(phys) != phys) bad++;
        if (psx_ram_map_write(phys) != phys) bad++;
        if (psx_ram_canonical_offset(0x80000000u | phys) != phys) bad++;
        if (!psx_ram_resolve(0x80000000u | phys, 4u, &off) || off != phys) bad++;
        if (psx_ram_canon_code_addr(0x80000000u | phys) != (0x80000000u | phys))
            bad++;
    }
    if (bad) fprintf(stderr, "  %s: %u expanded-decode violations\n", phase, bad);
    check(bad == 0u, phase);
    check(memory_get_ram_bytes() == PSX_MAIN_RAM_EXPANDED_BYTES, phase);
    check(psx_ram_8mb_active(), phase);
}

int main(void) {
    const uint32_t high = 0x00201000u;
    const uint32_t high_page = high >> 12;
    uint32_t i = high_page - PSX_RAM_HIGH_PAGE0;

    /* Process start: nothing requested. */
    check_retail_everywhere("default geometry is retail 2 MiB mirroring");

    /* memory_init() with no activation plugin. */
    psx_ram_apply_size_request();
    check_retail_everywhere("memory_init without the mod stays retail");

    /* mod_runtime_activate_plugins() always resets first; a plan without the
     * 8 MB package must leave retail in place. */
    psx_ram_reset_size_request();
    psx_ram_apply_size_request();
    check_retail_everywhere("reset + apply without the mod stays retail");

    /* The mod requests expanded RAM; nothing changes until memory_init(). */
    check(psx_mod_set_main_ram_8mb(1) == 1, "8 MB request accepted");
    check_retail_everywhere("8 MB request is inert before memory_init");
    psx_ram_apply_size_request();
    check_expanded_everywhere("8 MB request applies unique 8 MiB decode");

    /* A later boot whose plan drops the mod returns to retail mirroring. */
    psx_ram_reset_size_request();
    psx_ram_apply_size_request();
    check_retail_everywhere("reboot without the mod restores retail");

    /* Savestate restore in retail never leaves unique pages behind. */
    memset(g_psx_ram_high_unique, 0xFF, sizeof(g_psx_ram_high_unique));
    psx_ram_resync_high_after_restore(s_ram);
    check_retail_everywhere("retail restore clears unique high pages");

    /* Expanded geometry with partial registration: unregistered high pages
     * keep aliasing the low 2 MiB; registered ones decode uniquely. */
    psx_mod_set_main_ram_8mb(1);
    psx_ram_apply_size_request();
    memset(g_psx_ram_high_unique, 0, sizeof(g_psx_ram_high_unique));
    check(psx_ram_map_read(high) == 0x00001000u,
          "unregistered 8 MB high page still mirrors low RAM");
    g_psx_ram_high_unique[i >> 5] |= 1u << (i & 31u);
    check(psx_ram_map_read(high) == high,
          "registered 8 MB high page maps uniquely");
    check(psx_ram_canon_code_addr_inline(0x80201000u) == 0x80201000u,
          "registered high code PC remains unique");

    /* Expanded restore resync re-applies the registered (all-high) map. */
    memset(g_psx_ram_high_unique, 0, sizeof(g_psx_ram_high_unique));
    psx_mod_set_main_ram_8mb(1);
    psx_ram_apply_size_request();
    memset(g_psx_ram_high_unique, 0, sizeof(g_psx_ram_high_unique));
    psx_ram_resync_high_after_restore(s_ram);
    check_expanded_everywhere("expanded restore resync keeps unique decode");

    if (failures == 0) printf("PASS  psx_ram_runtime_map (%s)\n", "all phases");
    return failures ? 1 : 0;
}
