#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "psx_native_span.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint8_t ram[PSX_MAIN_RAM_BACKING_BYTES];
uint8_t *g_psx_ram = ram;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;
int g_ls_mode, g_ls_replay_active, g_event_step_conservative;
volatile int g_ds_recording;
int g_ram_read_watch_active, g_dma_exec_depth;
uint64_t psx_cycle_count, psx_next_service_cycle = 100u;
int psx_in_device_service, g_psx_cyc_bb_defer;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
int g_psx_gcs_batch;
static unsigned services, reads;
static uint32_t last_addr;
void psx_devices_service_to_now(void) { ++services; psx_next_service_cycle = psx_cycle_count + 100u; }
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; psx_devices_service_to_now(); }
static uint32_t read_word(uint32_t addr) { ++reads; last_addr = addr; return 0xFEDCBA98u; }
static uint16_t read_half(uint32_t addr) { ++reads; last_addr = addr; return 0xBA98u; }
static uint8_t read_byte(uint32_t addr) { ++reads; last_addr = addr; return 0x98u; }

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.read_word = read_word;
    cpu.read_half = read_half;
    cpu.read_byte = read_byte;
    cpu.ld_which_t = 0x20u;
    cpu.read_absorb[1] = 9u;
#if defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS
    ram[0x100u] = 0x12u; ram[0x101u] = 0x34u;
    ram[0x102u] = 0x56u; ram[0x103u] = 0x78u;
    psx_native_span_charge(7u);
    CHECK(psx_cycle_count == 7u);
    psx_cyc_step(&cpu, 2u);
    psx_icache_fetch(&cpu, 0x80010000u);
    CHECK(psx_cycle_count == 7u && cpu.read_absorb[1] == 9u);
    CHECK(psx_cyc_load_word(&cpu, 0x80000100u, 2u, 1u) == 0x78563412u);
    CHECK(psx_cyc_load_half(&cpu, 0xA0200100u, 2u, 1u) == 0x3412u);
    CHECK(psx_cyc_load_byte(&cpu, 0x00600103u, 2u, 1u) == 0x78u);
    CHECK(psx_cyc_lwc2_read(&cpu, 0x100u) == 0x78563412u);
    CHECK(reads == 0u && psx_cycle_count == 7u);

    /* Read ownership and unsupported address cases must reach the real path. */
    int *guards[] = {&g_ls_mode, &g_ls_replay_active, &g_ram_read_watch_active, &g_dma_exec_depth};
    for (unsigned n = 0; n < sizeof guards / sizeof guards[0]; ++n) {
        *guards[n] = 1;
        CHECK(psx_cyc_load_word(&cpu, 0x100u, 2u, 1u) == 0xFEDCBA98u);
        *guards[n] = 0;
    }
    g_ds_recording = 1;
    CHECK(psx_cyc_load_byte(&cpu, 0x100u, 2u, 1u) == 0x98u);
    g_ds_recording = 0;
    CHECK(reads == 5u);
    CHECK(psx_cyc_load_word(&cpu, 0x1F801100u, 2u, 1u) == 0xFEDCBA98u);
    CHECK(last_addr == 0x1F801100u);
    CHECK(psx_cyc_load_half(&cpu, 0xC0000100u, 2u, 1u) == 0xBA98u);
    CHECK(last_addr == 0xC0000100u);
    CHECK(psx_cyc_load_word(&cpu, PSX_MAIN_RAM_RETAIL_BYTES - 2u, 2u, 1u) == 0xFEDCBA98u);
    CHECK(reads == 8u);

    /* Expanded RAM must decode unique bytes instead of retail mirrors. */
    ram[0x200100u] = 0xABu;
    CHECK(psx_cyc_load_byte(&cpu, 0x80200100u, 2u, 1u) == 0x12u);
    g_psx_ram_mask = PSX_MAIN_RAM_EXPANDED_BYTES - 1u;
    CHECK(psx_cyc_load_byte(&cpu, 0x80200100u, 2u, 1u) == 0xABu);
    CHECK(psx_cyc_load_byte(&cpu, 0xA0200100u, 2u, 1u) == 0xABu);
    CHECK(psx_cyc_load_word(&cpu, PSX_MAIN_RAM_EXPANDED_BYTES - 2u, 2u, 1u) == 0xFEDCBA98u);
    CHECK(reads == 9u);
    g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;

    /* Span charging still drives deadline service and the scale carry. */
    psx_native_span_charge(100u);
    CHECK(psx_cycle_count == 107u && services == 1u);
    g_psx_gcs_recip_q16 = 32768u;
    psx_native_span_charge(3u);
    psx_native_span_charge(3u);
    CHECK(psx_cycle_count == 110u && g_psx_gcs_frac == 0u);
#else
    psx_native_span_charge(7u);
    CHECK(psx_cycle_count == 0u);
    psx_cyc_step(&cpu, 2u);
    psx_cyc_batch_flush();
    CHECK(psx_cycle_count == 1u && cpu.read_absorb[1] == 0u);
#endif
    puts("PASS: build-selected native span values/progress or faithful timing");
    return 0;
}
