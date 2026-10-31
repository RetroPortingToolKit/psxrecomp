/* Word and half loads from KSEG2 do not read main RAM.
 *
 * KSEG2 (0xC0000000 and up) holds only the cache control register at
 * 0xFFFE0130 (PSX-SPX "KUSEG,KSEG0,KSEG1,KSEG2 Memory Regions"). memory.c
 * reads every other KSEG2 address as 0 and counts it in g_kseg2_ignored_reads.
 * The inline word and half loads in psx_cyc.h take a main-RAM fast path in
 * block-cycle builds. Before the address exclusion that path masked the address with
 * 0x1FFFFFFF and so read the RAM mirror for 0xC0000000-0xC07FFFFF and
 * 0xE0000000-0xE07FFFFF, while the byte load of the same address read 0.
 *
 * This fixture links the real readers (memory.c) and calls the inline loads
 * as compiled code and the dirty-RAM interpreter do. It checks:
 *
 *   1. KSEG0 and KSEG1 word and half loads still read RAM, uncounted;
 *   2. word and half loads at 0xC0001000 and 0xE0001000 return 0 and add 1
 *      each to g_kseg2_ignored_reads, the same as the byte load;
 *   3. 0xFFFE0130 still reads the cache control register, uncounted.
 *
 * test_kseg2_inline_load.py builds it; link seams that this path does not
 * reach are stubs that abort (fixture_link.py). */
#define PSX_ENABLE_BLOCK_CYCLES 1
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "kernel_patch_ranges.h"
#include "psx_bios_image.h"
#include "psx_cyc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- memory.c under test ------------------------------------------------ */
extern uint64_t g_kseg2_ignored_reads;

/* ---- what memory.c and the inline loads read from the rest of the runtime */
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
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val) {
    (void)phys; (void)val;
}

/* ---- the fixture -------------------------------------------------------- */
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

#define RAM_WORD 0x12345678u

int main(void) {
    static CPUState cpu;
    uint32_t value = RAM_WORD;
    uint64_t before;

    cpu.ld_which_t = 32;
    /* No load-delay timing: this fixture is about the value and the counter,
     * so neither path calls the cycle scheduler. */
    g_psx_load_delay = 0;
    memcpy(g_psx_ram + 0x1000u, &value, sizeof value);

    /* 1. Main RAM through KSEG0 and KSEG1. */
    before = g_kseg2_ignored_reads;
    check(psx_cyc_load_word(&cpu, 0x80001000u, 2u, 0u) == RAM_WORD, "KSEG0 word reads RAM");
    check(psx_cyc_load_word(&cpu, 0xA0001000u, 2u, 0u) == RAM_WORD, "KSEG1 word reads RAM");
    check(psx_cyc_load_half(&cpu, 0x80001000u, 2u, 0u) == 0x5678u, "KSEG0 half reads RAM");
    check(psx_cyc_load_half(&cpu, 0xA0001002u, 2u, 0u) == 0x1234u, "KSEG1 half reads RAM");
    check(g_kseg2_ignored_reads == before, "RAM loads are not counted as KSEG2");

    /* 2. KSEG2 over the RAM mirrors: 0, counted once per load, like the byte load. */
    before = g_kseg2_ignored_reads;
    check(psx_cyc_load_byte(&cpu, 0xC0001000u, 2u, 0u) == 0u, "KSEG2 byte reads 0");
    check(g_kseg2_ignored_reads == before + 1u, "KSEG2 byte is counted");
    static const uint32_t kseg2[] = { 0xC0001000u, 0xE0001000u };
    for (unsigned i = 0; i < sizeof kseg2 / sizeof kseg2[0]; ++i) {
        before = g_kseg2_ignored_reads;
        uint32_t word = psx_cyc_load_word(&cpu, kseg2[i], 2u, 0u);
        uint64_t after_word = g_kseg2_ignored_reads;
        uint16_t half = psx_cyc_load_half(&cpu, kseg2[i], 2u, 0u);
        printf("KSEG2 0x%08X: word 0x%08X half 0x%04X counted %u+%u\n", (unsigned)kseg2[i],
               (unsigned)word, (unsigned)half, (unsigned)(after_word - before),
               (unsigned)(g_kseg2_ignored_reads - after_word));
        check(word == 0u, "KSEG2 word reads 0, not the RAM mirror");
        check(after_word == before + 1u, "KSEG2 word is counted in g_kseg2_ignored_reads");
        check(half == 0u, "KSEG2 half reads 0, not the RAM mirror");
        check(g_kseg2_ignored_reads == after_word + 1u,
              "KSEG2 half is counted in g_kseg2_ignored_reads");
    }

    /* 3. The cache control register is the one KSEG2 word that exists. */
    before = g_kseg2_ignored_reads;
    (void)psx_cyc_load_word(&cpu, 0xFFFE0130u, 2u, 0u);
    check(g_kseg2_ignored_reads == before, "cache control is read, not ignored");

    puts("PASS: KSEG2 word and half loads read 0 and are counted; RAM and cache control unchanged");
    return 0;
}
