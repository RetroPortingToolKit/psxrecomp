/* DMA6 (OTC) starts only when CHCR bit 28 is set.
 *
 * PSX-SPX "D#_CHCR": bit 28 is the manual start/trigger used for SyncMode 0,
 * and OTC has no DREQ. Authored test programs run on a reference emulator
 * agree: CHCR = 01000002h on channel 6 never starts (busy stays set and the
 * table is not written), while 11000002h writes the table and completes.
 * The channels with a device request start on bit 24 alone.
 *
 * Production dma.c, dma_gpu_ll.c and gpu_timeline.c (test_dma_otc_start.py
 * links them); memory, GPU and clock are seams, everything else aborts. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dma.h"

void     dma_init(void);
void     dma_write(uint32_t addr, uint32_t val);
void     dma_advance(uint32_t cycles);
int      dma_gpu_linked_list_active(void);
uint32_t dma_cycles_to_internal_event(void);

/* ---- clock and device service ------------------------------------------ */
uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle;
int      psx_in_device_service;
int      g_event_step_conservative;
int      g_ls_replay_active;
int      g_psx_render_pass_active;
int      g_psx_guest_time_frozen;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;

static uint64_t s_serviced_to;
void psx_devices_service_to_now(void) {
    if (psx_in_device_service) return;
    psx_in_device_service = 1;
    const uint64_t now = psx_cycle_count;
    dma_advance((uint32_t)(now - s_serviced_to));
    s_serviced_to = now;
    const uint32_t next = dma_cycles_to_internal_event();
    psx_next_service_cycle = next == 0xFFFFFFFFu ? UINT64_MAX : now + (next ? next : 1u);
    psx_in_device_service = 0;
}
void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
    psx_devices_service_to_now();
}

/* ---- memory ------------------------------------------------------------- */
uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;
static uint8_t ram[0x00200000u];
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_read_word(uint32_t a) {
    uint32_t v; memcpy(&v, ram + (a & 0x1FFFFCu), 4); return v;
}
void psx_write_word(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFCu), &v, 4); }
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) { return a; }
int psx_mod_host_output_scope(void) { return 0; }
int psx_mod_gpu_dma_memory_contains(uint32_t a,uint32_t n) {
    return a>=0x80800000u && a<=0x80801000u && n<=0x80801000u-a;
}
const uint8_t *psx_mod_gpu_dma_read_view(uint32_t a,uint32_t n) {
    return psx_mod_gpu_dma_memory_contains(a,n)?ram+(a&0x1FFFFFu):NULL;
}
extern int psx_mod_set_host_gpu_draw_range(unsigned,uint32_t,uint32_t);
extern void dma_debug_host_draw(uint64_t*,uint64_t*);

/* ---- GPU ---------------------------------------------------------------- */
static uint32_t s_gp0[64];
static unsigned s_gp0_n;
void gpu_write_gp0(uint32_t val) { if (s_gp0_n < 64u) s_gp0[s_gp0_n] = val; ++s_gp0_n; }
void gpu_set_gp0_source(uint32_t addr) { (void)addr; }
void gpu_set_gp0_linked_list_node(uint32_t addr, uint32_t n) { (void)addr; (void)n; }
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t start_addr) { (void)start_addr; }
void gpu_ws_validate_linked_list_header(uint32_t addr, uint32_t h) { (void)addr; (void)h; }
void gpu_ws_validate_linked_list_node(uint32_t addr, uint32_t n) { (void)addr; (void)n; }

/* ---- observers and IRQ (inert) ----------------------------------------- */
uint32_t i_stat;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint64_t s_frame_count;
uint64_t g_io_openbus_reads;
uint64_t g_io_openbus_writes;
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
static int s_in_exception;
int psx_get_in_exception(void) { return s_in_exception; }

/* ---- scenario ------------------------------------------------------------ */
static int failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

#define OT_TOP   0x00001010u   /* last entry; the table runs down to 1000h */
#define OT_WORDS 5u
#define SENTINEL 0xA5A5A5A5u
#define CH6_CHCR 0x1F8010E8u

uint32_t dma_read(uint32_t addr);

static void setup(void) {
    dma_init();
    psx_cycle_count = 1000u;
    s_serviced_to = psx_cycle_count;
    psx_next_service_cycle = UINT64_MAX;
    memset(ram, 0, sizeof ram);
    for (uint32_t i = 0; i < OT_WORDS; i++) psx_write_word(OT_TOP - 4u * i, SENTINEL);
    dma_write(0x1F8010F0u, 0x08000000u);       /* DPCR: channel 6 enabled */
    dma_write(0x1F8010E0u, OT_TOP);            /* MADR */
    dma_write(0x1F8010E4u, OT_WORDS);          /* BCR */
}

static void settle(void) {
    for (int i = 0; i < 100000; i++) {
        psx_cycle_count += 1u;
        psx_devices_service_to_now();
    }
}

static int table_written(void) {
    for (uint32_t i = 0; i < OT_WORDS; i++) {
        uint32_t a = OT_TOP - 4u * i;
        uint32_t want = i == OT_WORDS - 1u ? 0x00FFFFFFu : a - 4u;
        if (psx_read_word(a) != want) return 0;
    }
    return 1;
}

int main(void) {
    /* Bit 24 alone: no start. */
    setup();
    dma_write(CH6_CHCR, 0x01000002u);
    settle();
    CHECK(psx_read_word(OT_TOP) == SENTINEL && psx_read_word(OT_TOP - 16u) == SENTINEL,
          "OTC wrote the table with bit 28 clear (top %08X)", psx_read_word(OT_TOP));
    CHECK(dma_read(CH6_CHCR) & (1u << 24),
          "OTC with bit 28 clear completed (CHCR %08X)", dma_read(CH6_CHCR));

    /* Setting bit 28 on the waiting channel starts it. */
    dma_write(CH6_CHCR, 0x11000002u);
    settle();
    CHECK(table_written(), "OTC did not write the table once bit 28 was set");
    CHECK(!(dma_read(CH6_CHCR) & ((1u << 24) | (1u << 28))),
          "OTC still busy after completion (CHCR %08X)", dma_read(CH6_CHCR));

    /* Bits 24 and 28 together: the usual start. */
    setup();
    dma_write(CH6_CHCR, 0x11000002u);
    settle();
    CHECK(table_written(), "OTC 11000002h did not write the table");
    CHECK(!(dma_read(CH6_CHCR) & (1u << 24)), "OTC 11000002h still busy");

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS: DMA6 OTC starts only with CHCR bit 28");
    return 0;
}
