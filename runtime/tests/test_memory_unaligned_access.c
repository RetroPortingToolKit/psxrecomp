/* Unaligned halfword and word accesses at the end of a memory region.
 *
 * A CPU load or store of a misaligned halfword or word raises an address
 * error (PSX-SPX cpuspecifications, "Load/Store Alignment"), so on hardware
 * such an access never reaches the bus. Host code still issues them: mods
 * through psx_mod_read_* / psx_mod_write_*, debug and HLE helpers, and the
 * compiled or interpreted guest wherever no address-error check is emitted.
 * memory.c performs such an access as byte accesses at consecutive
 * addresses, each decoded on its own, so the bytes past the end of a region
 * go to whatever the physical map holds next (the next RAM mirror, the mod
 * GPU-DMA aperture, open bus) and no host array is indexed past its end.
 *
 * This fixture links the production memory.c and render_pass_plan.c and
 * checks, in the retail 2 MiB map and in the 8 MB RAM map:
 *
 *   1. halfword and word reads and writes straddling the scratchpad end
 *      (0x1F8003FF, 0x1F8003FD), the BIOS ROM end and the live RAM end;
 *   2. the same stores during a render pass;
 *   3. the inline main-RAM load fast path (psx_cyc.h) at the RAM end;
 *   4. that aligned accesses next to those ends are unchanged.
 *
 * test_memory_unaligned_access.py builds it, with AddressSanitizer where the
 * toolchain has it, so an access past a host array fails the run even when
 * the bytes it reads happen to look right. */
#define PSX_ENABLE_BLOCK_CYCLES 1
#include "harness_common.h"
#include "mod_memory.h"
#include "mod_plugins.h"
#include "psx_cyc.h"
#include "psx_memory.h"
#include "render_pass.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *memory_get_ram_ptr(void);
uint8_t *memory_get_scratchpad_ptr(void);
void memory_init(const char *bios_path);
uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint8_t psx_read_byte(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t val);
void psx_write_half(uint32_t addr, uint16_t val);
extern uint64_t g_render_pass_dropped_writes[RENDER_PASS_DROP_CLASSES];

/* What the inline load path in psx_cyc.h reads besides memory.c. Load-delay
 * timing is switched off below, so none of it is used for a cycle charge. */
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
int g_psx_cyc_bb_defer;
uint32_t *g_psx_cyc_local_acc;
uint64_t g_psx_bail_flattened, g_psx_device_gen, psx_next_service_cycle;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
int g_psx_gcs_batch;
int psx_in_device_service;
void psx_advance_cycles_slow(uint32_t cycles) { (void)cycles; abort(); }
void psx_devices_service_to_now(void) { abort(); }

/* The targeted RAM read watch, recorded. */
static unsigned s_watch_hits;
static uint32_t s_watch_phys, s_watch_value;
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val) {
    s_watch_hits++;
    s_watch_phys = phys;
    s_watch_value = val;
}

#define BIOS_BYTES (512u * 1024u)

static int s_failures;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failures++;
    }
}

static uint8_t bios_byte(uint32_t off) { return (uint8_t)(off * 7u + 3u); }

static void write_bios(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", path); exit(2); }
    for (uint32_t i = 0; i < BIOS_BYTES; i++) fputc(bios_byte(i), f);
    fclose(f);
}

static void boot(const char *bios, int ram_8mb) {
    psx_mod_set_main_ram_8mb(ram_8mb);
    memory_init(bios);
}

static void scratchpad_end(void) {
    uint8_t *sp = memory_get_scratchpad_ptr();
    sp[0x000] = 0x5Au; sp[0x001] = 0x5Bu;
    sp[0x3FC] = 0xA1u; sp[0x3FD] = 0xA2u; sp[0x3FE] = 0xA3u; sp[0x3FF] = 0xA4u;

    /* 0x1F800400.. is not mapped: its bytes read 0 and drop stores. */
    check(psx_read_half(0x1F8003FFu) == 0x00A4u, "scratchpad: half at 0x1F8003FF");
    check(psx_read_word(0x1F8003FDu) == 0x00A4A3A2u, "scratchpad: word at 0x1F8003FD");
    check(psx_read_word(0x1F8003FCu) == 0xA4A3A2A1u, "scratchpad: aligned last word");
    check(psx_read_half(0x1F8003FEu) == 0xA4A3u, "scratchpad: aligned last half");

    psx_write_half(0x1F8003FFu, 0xBBCCu);
    check(sp[0x3FF] == 0xCCu, "scratchpad: half store keeps its in-range byte");
    psx_write_word(0x1F8003FDu, 0x11223344u);
    check(sp[0x3FD] == 0x44u && sp[0x3FE] == 0x33u && sp[0x3FF] == 0x22u,
          "scratchpad: word store keeps its in-range bytes");
    check(sp[0x000] == 0x5Au && sp[0x001] == 0x5Bu,
          "scratchpad: bytes past the end do not wrap to its start");
}

static void bios_end(void) {
    const uint32_t b0 = bios_byte(0x7FFFEu), b1 = bios_byte(0x7FFFFu);
    check(psx_read_half(0xBFC7FFFFu) == b1, "BIOS: half at the last byte");
    check(psx_read_word(0xBFC7FFFEu) == (b0 | b1 << 8), "BIOS: word over the end");
    check(psx_read_word(0xBFC7FFFCu) ==
              (bios_byte(0x7FFFCu) | (uint32_t)bios_byte(0x7FFFDu) << 8 |
               b0 << 16 | b1 << 24),
          "BIOS: aligned last word");
}

/* Retail: the 2 MiB DRAM repeats across the 8 MiB window, so a straddle of
 * the live end continues in the next mirror. */
static void retail_ram_end(void) {
    uint8_t *ram = memory_get_ram_ptr();
    ram[0x1FFFFDu] = 0xC1u; ram[0x1FFFFEu] = 0xC2u; ram[0x1FFFFFu] = 0xC3u;
    ram[0] = 0xD0u; ram[1] = 0xD1u; ram[2] = 0xD2u;

    check(psx_read_half(0x801FFFFFu) == 0xD0C3u, "retail RAM: half into the next mirror");
    check(psx_read_word(0x801FFFFEu) == 0xD1D0C3C2u, "retail RAM: word into the next mirror");
    check(psx_read_word(0x801FFFFCu) == (0xC3C2C100u | ram[0x1FFFFCu]),
          "retail RAM: aligned last word");
    /* The 4th mirror ends at the window: past it is the GPU-DMA aperture. */
    check(psx_read_word(0x807FFFFEu) == 0x0000C3C2u, "retail RAM: word past the window");

    psx_write_word(0x801FFFFDu, 0x44332211u);
    check(ram[0x1FFFFDu] == 0x11u && ram[0x1FFFFEu] == 0x22u &&
          ram[0x1FFFFFu] == 0x33u && ram[0] == 0x44u,
          "retail RAM: word store continues in the next mirror");
    psx_write_half(0x801FFFFFu, 0x6655u);
    check(ram[0x1FFFFFu] == 0x55u && ram[0] == 0x66u,
          "retail RAM: half store continues in the next mirror");
    check(ram[0x200000u] == 0u && ram[0x200001u] == 0u,
          "retail RAM: no store lands in the backing beyond the live RAM");
}

/* 8 MB map: the window decodes uniquely and ends at the backing array's end;
 * the mod GPU-DMA aperture starts right after it. */
static void expanded_ram_end(void) {
    CPUState cpu = {0};
    uint8_t *ram = memory_get_ram_ptr();
    ram[0x7FFFFCu] = 0xE0u; ram[0x7FFFFDu] = 0xE1u;
    ram[0x7FFFFEu] = 0xE2u; ram[0x7FFFFFu] = 0xE3u;

    check(psx_read_half(0x807FFFFFu) == 0x00E3u, "8 MB RAM: half at the last byte");
    check(psx_read_word(0x807FFFFDu) == 0x00E3E2E1u, "8 MB RAM: word over the end");
    check(psx_read_word(0x807FFFFCu) == 0xE3E2E1E0u, "8 MB RAM: aligned last word");

    cpu.ld_which_t = 32;
    g_psx_load_delay = 0;
    check(psx_cyc_load_word(&cpu, 0x807FFFFDu, 2u, 0u) == 0x00E3E2E1u,
          "8 MB RAM: inline word load over the end");
    check(psx_cyc_load_half(&cpu, 0x807FFFFFu, 2u, 0u) == 0x00E3u,
          "8 MB RAM: inline half load at the last byte");
    check(psx_cyc_load_word(&cpu, 0x807FFFFCu, 2u, 0u) == 0xE3E2E1E0u,
          "8 MB RAM: inline aligned last word");

    /* No aperture allocated: the bytes past the end are dropped. */
    psx_write_word(0x807FFFFDu, 0x44332211u);
    check(ram[0x7FFFFDu] == 0x11u && ram[0x7FFFFEu] == 0x22u && ram[0x7FFFFFu] == 0x33u,
          "8 MB RAM: word store keeps its in-range bytes");

    /* With an aperture, the bytes past the end land in it. */
    check(psx_mod_gpu_dma_memory_alloc(16u, 4u) == PSX_MOD_GPU_DMA_GUEST_BASE,
          "8 MB RAM: aperture allocated at its base");
    psx_write_word(0x807FFFFEu, 0xA4A3A2A1u);
    check(ram[0x7FFFFEu] == 0xA1u && ram[0x7FFFFFu] == 0xA2u,
          "8 MB RAM: word store over the end keeps its RAM bytes");
    check(psx_read_half(PSX_MOD_GPU_DMA_GUEST_BASE) == 0xA4A3u,
          "8 MB RAM: word store over the end continues in the aperture");
    check(psx_read_word(0x807FFFFEu) == 0xA4A3A2A1u,
          "8 MB RAM: word read over the end continues in the aperture");
    check(psx_cyc_load_half(&cpu, PSX_MOD_GPU_DMA_GUEST_BASE, 2u, 0u) == 0xA4A3u &&
              psx_cyc_load_word(&cpu, 0x807FFFFEu, 2u, 0u) == 0xA4A3A2A1u,
          "8 MB RAM: inline loads past the RAM window read the aperture");
}

/* PSX_READ_WATCH: a split word read still reports once, with the RAM offset
 * of its first byte and the composed value, as before the split. */
static void misaligned_read_watch(void) {
    uint8_t *ram = memory_get_ram_ptr();
    ram[0x1001u] = 0x11u; ram[0x1002u] = 0x22u; ram[0x1003u] = 0x33u; ram[0x1004u] = 0x44u;
    s_watch_hits = 0;
    g_ram_read_watch_active = 1;
    check(psx_read_word(0x80001001u) == 0x44332211u, "read watch: misaligned word value");
    g_ram_read_watch_active = 0;
    check(s_watch_hits == 1u && s_watch_phys == 0x1001u && s_watch_value == 0x44332211u,
          "read watch: a misaligned word read is reported once");
}

static void render_pass_stores(const char *bios) {
    uint64_t dropped;

    /* Retail: the next mirror. */
    boot(bios, 0);
    uint8_t *ram = memory_get_ram_ptr();
    uint8_t *sp = memory_get_scratchpad_ptr();
    g_psx_render_pass_active = 1;
    psx_write_word(0x801FFFFEu, 0x44332211u);
    check(ram[0x1FFFFEu] == 0x11u && ram[0x1FFFFFu] == 0x22u &&
          ram[0] == 0x33u && ram[1] == 0x44u,
          "render pass, retail RAM: word store continues in the next mirror");
    check(ram[0x200000u] == 0u, "render pass, retail RAM: nothing beyond the live RAM");

    /* Scratchpad: the bytes past the end are unmapped, dropped and counted. */
    dropped = g_render_pass_dropped_writes[RENDER_PASS_DROP_OTHER];
    psx_write_word(0x1F8003FEu, 0x88776655u);
    check(sp[0x3FE] == 0x55u && sp[0x3FF] == 0x66u && sp[0] == 0u,
          "render pass, scratchpad: word store keeps its in-range bytes");
    check(g_render_pass_dropped_writes[RENDER_PASS_DROP_OTHER] == dropped + 2u,
          "render pass, scratchpad: the two unmapped bytes are counted as dropped");
    g_psx_render_pass_active = 0;

    /* 8 MB: the aperture. */
    psx_mod_memory_reset_session();
    boot(bios, 1);
    check(psx_mod_gpu_dma_memory_alloc(16u, 4u) == PSX_MOD_GPU_DMA_GUEST_BASE,
          "render pass, 8 MB RAM: aperture allocated");
    g_psx_render_pass_active = 1;
    psx_write_word(0x807FFFFEu, 0xD4D3D2D1u);
    psx_write_half(0x807FFFFFu, 0xF2F1u);
    g_psx_render_pass_active = 0;
    check(ram[0x7FFFFEu] == 0xD1u && ram[0x7FFFFFu] == 0xF1u,
          "render pass, 8 MB RAM: stores over the end keep their RAM bytes");
    check(psx_read_half(PSX_MOD_GPU_DMA_GUEST_BASE) == 0xD4F2u,
          "render pass, 8 MB RAM: stores over the end continue in the aperture");
    psx_mod_memory_reset_session();   /* also retires the pass's arena journal */
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <bios scratch path>\n", argv[0]); return 2; }
    write_bios(argv[1]);

    boot(argv[1], 0);
    scratchpad_end();
    bios_end();
    retail_ram_end();
    misaligned_read_watch();

    boot(argv[1], 1);
    scratchpad_end();
    expanded_ram_end();
    psx_mod_memory_reset_session();

    render_pass_stores(argv[1]);

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: unaligned accesses at region ends decode byte by byte");
    return 0;
}
