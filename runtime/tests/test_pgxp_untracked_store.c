/* DMA and host stores against the PGXP shadow, end to end through the REAL
 * memory.c store paths and the REAL pgxp.cpp engine.
 *
 * A CPU store carries or resets its word's shadow through the PGXP_STORE
 * hook the emitter and the interpreters place after it. A DMA or host store
 * (psx_host_write_*) has no hook. Validation on read only catches a writer
 * that changes the word: one that writes the identical word - a GTE packet
 * re-uploaded by DMA, a host fill - would leave the old shadow believed,
 * and two projections that round to the same integer X/Y differ in their
 * fractions and depth. memory.c therefore drops the shadow of every word a
 * DMA or host store touches, in RAM (and its mirrors) and the scratchpad,
 * at word, half and byte width.
 *
 * Build/run: ctest -R pgxp_untracked_store_test */
#include "pgxp.h"
#include "pgxp_hooks.h"
#include "psx_memory.h"

#include <stdint.h>
#include <stdio.h>

extern void psx_write_word(uint32_t addr, uint32_t val);
extern void psx_write_half(uint32_t addr, uint16_t val);
extern void psx_write_byte(uint32_t addr, uint8_t val);
extern int  g_dma_exec_depth;
extern int  g_host_store_depth;
extern void memory_set_sr_ptr(const uint32_t *p);

/* gte.cpp position cache: off here, so only the dataflow shadow can answer. */
int gte_geometry_correction_lookup(uint32_t p, int32_t *x, int32_t *y) {
    (void)p; (void)x; (void)y; return 0;
}
int gte_geometry_correction_lookup_probe(uint32_t p, int32_t *x, int32_t *y) {
    (void)p; (void)x; (void)y; return 0;
}

static int failures;
static void check(int cond, const char *label) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", label); failures++; }
}

/* x = 160.5, y = 80.25 rounds to this word. */
#define PACKED 0x005000A0u
#define X16    ((160 << 16) | 0x8000)
#define Y16    ((80 << 16) | 0x4000)
#define SWC2_14 ((0x3Au << 26) | (1u << 21) | (14u << 16))

static void produce(uint32_t addr) {
    pgxp_gte_push_sxy(X16, Y16, 100, PACKED);
    psx_pgxp_cop2(NULL, SWC2_14, PACKED, addr);   /* the store's hook */
}

static int precise(uint32_t addr) {
    int32_t x, y; uint16_t z;
    return pgxp_get_precise_vertex(addr, PACKED, 160, 80, &x, &y, &z) ==
               PGXP_SRC_DATAFLOW && x == X16 && y == Y16;
}

int main(void) {
    static const uint32_t addrs[] = { 0x80100000u, 0x1F800100u };
    static const uint32_t sr = 0;
    psx_ram_apply_size_request();
    memory_set_sr_ptr(&sr);
    pgxp_set_enabled(1);
    pgxp_set_tolerance(-1.0f);

    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t a = addrs[i];

        /* A CPU store is the hook's business: memory.c leaves the shadow. */
        psx_write_word(a, PACKED);
        produce(a);
        psx_write_word(a, PACKED);
        check(precise(a), i ? "scratchpad: CPU store keeps the hooked shadow"
                            : "RAM: CPU store keeps the hooked shadow");

        /* The identical word written by DMA is not the projected vertex. */
        g_dma_exec_depth = 1;
        psx_write_word(a, PACKED);
        g_dma_exec_depth = 0;
        check(!precise(a), i ? "scratchpad: DMA word store drops the shadow"
                             : "RAM: DMA word store drops the shadow");

        produce(a);
        psx_host_write_word(a, PACKED);
        check(!precise(a), i ? "scratchpad: host word store drops the shadow"
                             : "RAM: host word store drops the shadow");

        produce(a);
        psx_host_write_half(a + 2u, (uint16_t)(PACKED >> 16));
        check(!precise(a), i ? "scratchpad: host half store drops the shadow"
                             : "RAM: host half store drops the shadow");

        produce(a);
        psx_host_write_byte(a + 3u, (uint8_t)(PACKED >> 24));
        check(!precise(a), i ? "scratchpad: host byte store drops the shadow"
                             : "RAM: host byte store drops the shadow");
        check(g_host_store_depth == 0, "host store depth unwinds");
    }

    /* A mirror address names the same word and the same shadow. */
    produce(0x80100000u);
    g_dma_exec_depth = 1;
    psx_write_word(0x00100000u, PACKED);
    g_dma_exec_depth = 0;
    check(!precise(0x80100000u), "DMA store through a mirror drops the shadow");

    if (failures) {
        fprintf(stderr, "pgxp_untracked_store_test: %d FAILURES\n", failures);
        return 1;
    }
    puts("pgxp_untracked_store_test: all checks passed");
    return 0;
}
