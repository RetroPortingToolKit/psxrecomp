/* The store that kicks a GPU linked-list DMA returns only when the walk ends.
 *
 * PSX-SPX "CPU Operation during DMA": the CPU waits while DMA moves data. A
 * guest that rewrites a packet right after DrawOTag therefore cannot reach it
 * before the walk does. The retail SCPH1001 intro does exactly that: it
 * re-copies its text packets from a template, clearing the link word, right
 * after the kick. With the CPU running alongside the walk, the copy reached a
 * packet first and the walk followed link 0 into low RAM.
 *
 * Here the list is A -> B -> end. Right after the kicking store returns, the
 * "CPU" clears B's header, then time runs until the walk is done:
 *
 *   - held (the default): the GPU received A's and B's payload in order, and
 *     guest time advanced by the walk inside the store;
 *   - control: a kick made from inside the device service is not held (the
 *     walk is still running when the store returns).
 *
 * Production dma.c and dma_gpu_ll.c (test_dma_gpu_list_cpu_hold.py links
 * them); memory, GPU and clock are seams, everything else aborts. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dma.h"
#include "mod_memory.h"

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
static uint8_t mod_ot[0x10000u], otc_original_ram[sizeof ram];
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_read_word(uint32_t a) {
    uint32_t off;
    if (psx_mod_gpu_dma_aperture_offset_for(a,4u,sizeof mod_ot,&off)) {
        uint32_t v;memcpy(&v,mod_ot+off,4);return v;
    }
    uint32_t v; memcpy(&v, ram + (a & 0x1FFFFCu), 4); return v;
}
void psx_write_word(uint32_t a, uint32_t v) {
    uint32_t off;
    if (psx_mod_gpu_dma_aperture_offset_for(a,4u,sizeof mod_ot,&off)) memcpy(mod_ot+off,&v,4);
    else memcpy(ram+(a&0x1ffffcu),&v,4);
}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a) {
    return psx_mod_gpu_dma_resolve_address_for(a,sizeof mod_ot);
}

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
#define NODE_A 0x00001000u
#define NODE_B 0x00002000u
#define NODE_C 0x00003000u
#define END    0x00FFFFFFu

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

static void build_list(void) {
    memset(ram, 0, sizeof ram);
    psx_write_word(NODE_A,      (2u << 24) | NODE_B);
    psx_write_word(NODE_A + 4u, 0xE1000001u);
    psx_write_word(NODE_A + 8u, 0xE2000002u);
    psx_write_word(NODE_B,      (1u << 24) | NODE_C);
    psx_write_word(NODE_B + 4u, 0x01000003u);
    psx_write_word(NODE_C,      END);
}

/* Kicks the list; `in_service` makes the kick look like one from inside the
 * device service. Returns 1 when the walk was still active after the store. */
static int kick(int in_service) {
    dma_init();
    s_gp0_n = 0;
    psx_cycle_count = 1000u;
    s_serviced_to = psx_cycle_count;
    psx_next_service_cycle = UINT64_MAX;
    build_list();
    dma_write(0x1F8010F0u, 0x00000800u);           /* DPCR: channel 2 enabled */
    dma_write(0x1F8010A0u, NODE_A);                /* MADR */
    dma_write(0x1F8010A4u, 0u);                    /* BCR */
    psx_in_device_service = in_service;
    dma_write(0x1F8010A8u, 0x01000401u);           /* CHCR: from RAM, list, start */
    psx_in_device_service = 0;
    return dma_gpu_linked_list_active();
}

static void finish_walk(void) {
    for (int i = 0; i < 1000000 && dma_gpu_linked_list_active(); i++) {
        psx_cycle_count += 1u;
        psx_devices_service_to_now();
    }
}

int main(void) {
    /* Held: the walk is over when the kicking store returns. */
    const uint64_t before = 1000u;
    const int active = kick(0);
    CHECK(!active, "walk still active after the kicking store");
    CHECK(psx_cycle_count > before, "guest time did not advance during the kick");
    psx_write_word(NODE_B, 0u);                     /* the guest's packet copy */
    finish_walk();
    CHECK(s_gp0_n == 3u && s_gp0[0] == 0xE1000001u && s_gp0[1] == 0xE2000002u &&
          s_gp0[2] == 0x01000003u,
          "GPU received %u words (%08X %08X %08X), expected A's and B's payload",
          s_gp0_n, s_gp0[0], s_gp0[1], s_gp0[2]);

    /* The hold is recorded in the always-on hold ring (dma_hold_stats /
     * dma_hold_ring): A's header + 2 words, B's header + 1 word, C's header. */
    {
        DMAGpuHoldStats st; const DMAGpuHoldEntry *ring = NULL;
        dma_debug_get_gpu_hold(&st, &ring);
        CHECK(st.count == 1u && st.exc_count == 0u, "hold count %llu exc %llu",
              (unsigned long long)st.count, (unsigned long long)st.exc_count);
        CHECK(ring && ring[0].words == 6u && ring[0].held_cycles == psx_cycle_count - before
              && ring[0].cycle == before && ring[0].in_exception == 0u
              && ring[0].still_active == 0u,
              "hold entry words %u held %u cycle %llu", ring ? ring[0].words : 0u,
              ring ? ring[0].held_cycles : 0u,
              ring ? (unsigned long long)ring[0].cycle : 0ull);
        /* A kick from inside an exception handler is attributed to it. */
        s_in_exception = 1;
        kick(0);
        s_in_exception = 0;
        dma_debug_get_gpu_hold(&st, &ring);
        CHECK(st.count == 2u && st.exc_count == 1u && ring[1].in_exception == 1u &&
              st.exc_held_cycles == ring[1].held_cycles,
              "exception-kick attribution: count %llu exc %llu",
              (unsigned long long)st.count, (unsigned long long)st.exc_count);
    }

    /* Control: a kick from inside the device service is not held. */
    CHECK(kick(1), "a kick from inside the device service was held");
    finish_walk();
    CHECK(!dma_gpu_linked_list_active(), "control walk did not finish");

    /* Enlarged modern OTs must retain their 24-bit aperture addresses during
     * channel6 clear; their retail-RAM aliases stay byte-for-byte intact. */
    dma_init();memset(ram,0xa5,sizeof ram);memcpy(otc_original_ram,ram,sizeof ram);
    memset(mod_ot,0xcc,sizeof mod_ot);
    const uint32_t otc_base=PSX_MOD_GPU_DMA_APERTURE_BASE, otc_entries=6144u;
    dma_write(0x1f8010f0u,0x08000000u);
    dma_write(0x1f8010e0u,PSX_MOD_GPU_DMA_GUEST_BASE+(otc_entries-1u)*4u);
    dma_write(0x1f8010e4u,otc_entries);
    dma_write(0x1f8010e8u,0x11000002u);
    CHECK(!memcmp(ram,otc_original_ram,sizeof ram),"OTC aliased enhanced table into original RAM");
    for(uint32_t i=0;i<otc_entries;++i)
        CHECK(psx_read_word(otc_base+i*4u)==(i?otc_base+(i-1u)*4u:END),"enhanced OTC link %u",i);
    CHECK(psx_read_word(otc_base+otc_entries*4u)==0xccccccccu,"OTC crossed table end");
    /* An ordinary mirrored head still uses the retail decode window. */
    dma_init();dma_write(0x1f8010f0u,0x08000000u);
    dma_write(0x1f8010e0u,0x00201018u);dma_write(0x1f8010e4u,7u);
    dma_write(0x1f8010e8u,0x11000002u);
    for(uint32_t i=0;i<7u;++i)
        CHECK(psx_read_word(0x1000u+i*4u)==(i?0x1000u+(i-1u)*4u:END),"retail OTC link %u",i);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS: the GPU linked-list kick holds the CPU until the walk ends");
    return 0;
}
