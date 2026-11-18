/* Reuse the existing production-DMA fixture's hardware seams and scenarios.
 * The native variant must preserve those products without any helper reads. */
#define main reference_hold_checks
#define psx_read_word fixture_read_word
#include "test_dma_gpu_list_cpu_hold.c"
#undef psx_read_word
#undef main

#include <setjmp.h>
#include <stdlib.h>

static int fail_allocation, expect_allocation_halt;
static jmp_buf allocation_halt;
/* Only production modules rename realloc; the fixture calls the real CRT. */
void *fixture_realloc(void *p, size_t bytes) {
    return fail_allocation ? NULL : realloc(p, bytes);
}
void psx_fatal_halt(const char *reason) {
    if (expect_allocation_halt && strstr(reason, "allocation failed"))
        longjmp(allocation_halt, 1);
    fprintf(stderr, "Unexpected halt: %s\n", reason);
    abort();
}

int g_ls_mode, g_ram_read_watch_active;
volatile int g_ds_recording;
static uint64_t helper_reads, native_nodes;
uint32_t psx_read_word(uint32_t address) {
    helper_reads++;
    return fixture_read_word(address);
}
void gpu_ws_native_list_begin(void) {}
void gpu_ws_native_list_node(uint32_t a, uint32_t h, const uint32_t *words) {
    (void)a; (void)h; (void)words;
}
void gpu_ws_native_list_end(void) {}
void gpu_submit_gp0_node(uint32_t a, uint32_t n, const uint32_t *words) {
    gpu_set_gp0_linked_list_node(a, n);
    native_nodes++;
    for (uint32_t i = 0; i < n; ++i) gpu_write_gp0(words[i]);
}

int main(void) {
    CHECK(reference_hold_checks() == 0, "existing held/mixed-host contracts failed");
    /* The only helper reads in the existing suite come from its deliberately
     * asynchronous diagnostic control, not the native operation. */
    helper_reads = 0;
    const uint64_t nodes0 = native_nodes;
    CHECK(!kick(0), "native kick returned with an active list");
    DMAGpuNativeStats st;
    dma_debug_get_gpu_native(&st);
    CHECK(helper_reads == 0, "native operation made %llu helper reads",
          (unsigned long long)helper_reads);
    CHECK(native_nodes - nodes0 == 3, "native operation did not submit exactly three nodes");
    CHECK(st.lists && st.nodes && st.words == st.direct_words,
          "native direct-word accounting incomplete");
    CHECK(psx_cycle_count == 1006u, "native operation charged the wrong transfer budget");
    CHECK((dma_read(0x1F8010A8u) & (1u << 24)) == 0 &&
          dma_read(0x1F8010A0u) == END, "native completion registers wrong");
    const DMAGpuHoldEntry *ring;
    DMAGpuHoldStats hold;
    dma_debug_get_gpu_hold(&hold, &ring);
    const DMAGpuHoldEntry *last = &ring[(hold.count - 1u) & (DMA_GPU_HOLD_RING_CAP - 1u)];
    CHECK(last->words == 6 && last->steps == 1, "native operation retained per-word services");

    /* Reading/recording diagnostics must observe the selected native operation,
     * rather than change its simulation model to the per-word reference. */
    for (int mode = 0; mode < 4; ++mode) {
        dma_debug_get_gpu_native(&st);
        const uint64_t lists0 = st.lists;
        g_ls_mode = mode == 0;
        g_ds_recording = mode == 1;
        g_ram_read_watch_active = mode == 2;
        g_event_step_conservative = mode == 3;
        helper_reads = 0;
        CHECK(!kick(0), "observed native kick left an active list");
        dma_debug_get_gpu_native(&st);
        dma_debug_get_gpu_hold(&hold, &ring);
        last = &ring[(hold.count - 1u) & (DMA_GPU_HOLD_RING_CAP - 1u)];
        CHECK(st.lists == lists0 + 1u && last->steps == 1u,
              "diagnostic mode %d changed the selected native submission model", mode);
        CHECK(psx_cycle_count == 1006u && s_gp0_n == 3u &&
              s_gp0[0] == 0xE1000001u && s_gp0[2] == 0x01000003u,
              "diagnostic mode %d changed native output/time", mode);
    }
    g_ls_mode = g_ds_recording = g_ram_read_watch_active = g_event_step_conservative = 0;
    helper_reads = 0;

    /* A cyclic guest list remains bounded by the established node limit. */
    dma_init(); s_gp0_n = 0; psx_cycle_count = s_serviced_to = 1000;
    psx_next_service_cycle = UINT64_MAX;
    psx_write_word(NODE_A, NODE_A);
    dma_write(0x1F8010F0u, 0x800);
    dma_write(0x1F8010A0u, NODE_A);
    dma_write(0x1F8010A8u, 0x01000401u);
    CHECK(!dma_gpu_linked_list_active() && s_gp0_n == 0,
          "cyclic list failed to terminate at its node bound");
    CHECK(psx_cycle_count == 1000u + 0x40000u, "cyclic-list budget wrong");
    CHECK(helper_reads == 0, "cyclic native list used helper reads");

    /* A payload crosses the retail RAM mirror boundary. */
    dma_init(); s_gp0_n = 0; psx_cycle_count = s_serviced_to = 1000;
    psx_next_service_cycle = UINT64_MAX;
    psx_write_word(0x001FFFFCu, (2u << 24) | END);
    psx_write_word(0, 0xE1000001u);
    psx_write_word(4, 0xE2000002u);
    dma_write(0x1F8010F0u, 0x800);
    dma_write(0x1F8010A0u, 0x001FFFFCu);
    dma_write(0x1F8010A8u, 0x01000401u);
    CHECK(s_gp0_n == 2 && s_gp0[0] == 0xE1000001u && s_gp0[1] == 0xE2000002u,
          "native payload did not wrap through the live RAM geometry");
    CHECK(helper_reads == 0, "wrapped native list used helper reads");

    /* Resource failure refuses the operation before effects, rather than
     * silently changing the negotiated simulation model. */
    fail_allocation = expect_allocation_halt = 1;
    if (setjmp(allocation_halt) == 0) {
        kick(0);
        CHECK(0, "allocation failure neither halted nor refused the operation");
    } else {
        CHECK(s_gp0_n == 0 && psx_cycle_count == 1000u,
              "allocation failure published draws/time before refusal");
        CHECK(!dma_gpu_linked_list_active(), "allocation failure started the reference walker");
    }
    fail_allocation = expect_allocation_halt = 0;
    dma_init();
    if (failures) return 1;
    puts("PASS: native GPU submission preserves products, bounded lists and direct reads");
    return 0;
}
