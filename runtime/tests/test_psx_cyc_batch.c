#include "psx_thread_local.h"
#include "psx_cyc.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

PSX_THREAD_LOCAL uint64_t psx_cycle_count = 0;
PSX_THREAD_LOCAL uint64_t psx_next_service_cycle = 0;
PSX_THREAD_LOCAL uint32_t g_psx_cyc_batch = 0;
PSX_THREAD_LOCAL uint32_t g_psx_cyc_batch_limit = 0;
PSX_THREAD_LOCAL int g_psx_cyc_bb_defer = 0;
PSX_THREAD_LOCAL uint32_t *g_psx_cyc_local_acc = 0;
PSX_THREAD_LOCAL int psx_in_device_service = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
int g_ls_mode = 0;
volatile int g_ds_recording = 0;
PSX_THREAD_LOCAL uint8_t *g_psx_ram = 0;
int g_psx_load_delay = 1;
PSX_THREAD_LOCAL uint32_t g_psx_gcs_recip_q16 = 65536u;
PSX_THREAD_LOCAL uint32_t g_psx_gcs_frac = 0u;
PSX_THREAD_LOCAL int g_psx_gcs_batch = 0;

static int service_count;
static CPUState *service_cpu;

void psx_devices_service_to_now(void) {
    service_count++;
    /* Event/IRQ callbacks can change the pipeline before this instruction's
     * dependency/LDS phase. Check that inlining preserves that ordering. */
    if (service_cpu) {
        service_cpu->read_absorb[service_cpu->read_absorb_which] = 17;
        service_cpu->ld_which_t = 4;
        service_cpu->ld_absorb = 7;
        service_cpu->gpr[3] ^= 0x19u;
    }
    psx_next_service_cycle = psx_cycle_count + 1000u;
}

void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
}

static void reset_clock(uint64_t deadline) {
    psx_cycle_count = 0;
    psx_next_service_cycle = deadline;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    g_psx_cyc_bb_defer = 0;
    g_psx_cyc_local_acc = 0;
    service_count = 0;
}

static void clock_receipt(uint64_t out[7], uint32_t local) {
    out[0] = psx_cycle_count; out[1] = psx_next_service_cycle;
    out[2] = g_psx_cyc_batch; out[3] = g_psx_cyc_batch_limit;
    out[4] = g_psx_gcs_frac; out[5] = (uint64_t)service_count; out[6] = local;
}
static void restore_receipt(const uint64_t in[7], uint32_t *local) {
    psx_cycle_count = in[0]; psx_next_service_cycle = in[1];
    g_psx_cyc_batch = (uint32_t)in[2]; g_psx_cyc_batch_limit = (uint32_t)in[3];
    g_psx_gcs_frac = (uint32_t)in[4]; service_count = (int)in[5]; *local = (uint32_t)in[6];
}
static void test_one_cycle_specialization(void) {
    static const uint32_t scales[] = {1, 2, 3, 64};
    uint32_t random = 0x514A7u;
    for (unsigned scale = 0; scale < 4; ++scale)
    for (unsigned mode = 0; mode < 128; ++mode) {
        reset_clock((mode % 11u) * 7u);
        g_psx_gcs_recip_q16 = psx_gcs_recip_for(scales[scale]);
        g_psx_gcs_frac = mode * 509u;
        g_psx_gcs_batch = !!(mode & 1u);
        g_ls_replay_active = !!(mode & 2u);
        g_event_step_conservative = !!(mode & 4u);
        psx_in_device_service = !!(mode & 8u);
        g_psx_cyc_bb_defer = !!(mode & 16u);
        uint32_t local = (mode & 64u) ? UINT32_MAX - 2u : 17u;
        g_psx_cyc_local_acc = (mode & 32u) ? &local : NULL;
        CPUState cpu = {0};
        cpu.ld_which_t = 0x20u;
        for (unsigned instruction = 0; instruction < 128; ++instruction) {
            random = random * 1664525u + 1013904223u;
            uint32_t mask = random & 0xFFFFFFFEu;
            cpu.read_absorb_which = (uint8_t)((random >> 16) % 33u);
            cpu.read_absorb[cpu.read_absorb_which] = (uint8_t)((random >> 24) & 3u);
            cpu.ld_which_t = (uint8_t)((random >> 8) % 33u);
            cpu.ld_absorb = (random >> 4) & 7u;
            CPUState reference = cpu;
            uint64_t before[7], expected[7], actual[7];
            clock_receipt(before, local);
            service_cpu = instruction % 3u ? NULL : &reference;
            /* Keep the reference independent of the optimized base helper. */
            if (reference.read_absorb[reference.read_absorb_which])
                --reference.read_absorb[reference.read_absorb_which];
            else psx_cyc_charge(1u);
            psx_cyc_deps(&reference, mask); psx_cyc_lds(&reference);
            clock_receipt(expected, local);
            restore_receipt(before, &local);
            service_cpu = instruction % 3u ? NULL : &cpu;
            psx_cyc_step(&cpu, mask);
            service_cpu = NULL;
            clock_receipt(actual, local);
            assert(!memcmp(expected, actual, sizeof expected));
            assert(!memcmp(&reference, &cpu, sizeof cpu));
        }
    }
    g_psx_gcs_recip_q16 = 65536u; g_psx_gcs_frac = 0; g_psx_gcs_batch = 0;
    g_ls_replay_active = g_event_step_conservative = psx_in_device_service = 0;
    reset_clock(1000u);
}
int main(void) {
    test_one_cycle_specialization();
    reset_clock(5u);
    for (int i = 0; i < 4; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 4u);
    assert(service_count == 0);

    psx_cyc_charge(1u);
    assert(psx_cycle_count == 5u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 1);

    reset_clock(1000u);
    for (int i = 0; i < 63; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 63u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 64u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 0);

    reset_clock(5u);
    psx_cyc_bb_defer_begin();
    for (int i = 0; i < 10; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 10u);
    psx_cyc_bb_defer_flush();
    assert(psx_cycle_count == 10u);
    assert(service_count == 1);
    assert(g_psx_cyc_bb_defer == 1);
    psx_cyc_bb_defer_end();
    assert(g_psx_cyc_bb_defer == 0);

    reset_clock(0u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 1u);
    assert(service_count == 1);

    /* Emitter-level local accumulator: charges stay off the published clock
     * until local_publish / batch_flush; guest total at the barrier matches. */
    reset_clock(1000u);
    {
        uint32_t acc = 0;
        psx_cyc_local_begin(&acc);
        for (int i = 0; i < 100; i++) psx_cyc_charge(5u);
        assert(psx_cycle_count == 0u);
        assert(g_psx_cyc_batch == 0u);
        assert(acc == 500u);
        psx_cyc_bb_defer_flush();
        assert(psx_cycle_count == 500u);
        assert(acc == 0u);
        psx_cyc_local_end();
        assert(g_psx_cyc_local_acc == 0);
    }
    return 0;
}
