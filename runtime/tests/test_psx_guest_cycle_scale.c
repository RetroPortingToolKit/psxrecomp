/* [timing] guest_cycle_scale: instruction charges reach the guest clock at
 * 1/N of their cost; 1 leaves every charge exactly as it was. */
#undef NDEBUG   /* the checks below must run in Release too */
#include "psx_cyc.h"

#include <assert.h>
#include <stdint.h>

uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
int g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = 0;
int psx_in_device_service = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
int g_ls_mode = 0;
volatile int g_ds_recording = 0;
uint8_t *g_psx_ram = 0;
int g_psx_load_delay = 1;
uint32_t g_psx_gcs_recip_q16 = 65536u;
uint32_t g_psx_gcs_frac = 0u;
int g_psx_gcs_batch = 0;

/* Device service: records the guest cycle and the observation point (the
 * "interrupt check" index below) at which each deadline was serviced. */
static uint64_t s_deadline_step = 100000u;
static uint64_t s_service_cycle[64];
static int      s_service_check[64];
static int      s_services = 0, s_check = 0;
void psx_devices_service_to_now(void) {
    if (s_services < 64) {
        s_service_cycle[s_services] = psx_cycle_count;
        s_service_check[s_services] = s_check;
    }
    s_services++;
    psx_next_service_cycle = psx_cycle_count + s_deadline_step;
}
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }

static uint64_t run(uint32_t scale, uint32_t charges, uint32_t each) {
    g_psx_gcs_recip_q16 = psx_gcs_recip_for(scale);
    g_psx_gcs_frac = 0;
    psx_cycle_count = 0;
    psx_next_service_cycle = 1;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    for (uint32_t i = 0; i < charges; i++) psx_cyc_charge(each);
    psx_cyc_batch_flush();
    return psx_cycle_count;
}

/* [timing] guest_cycle_scale_batch: the same guest timeline with the scaled
 * charge batched. A run of 1-cycle charges (as psx_cyc_step makes) with an
 * interrupt check -- a batch flush -- every `every` charges; the deadline
 * every 997 guest cycles. Returns the total; fills the service log. */
static uint64_t run_checks(uint32_t scale, int batch, uint32_t charges, uint32_t every) {
    g_psx_gcs_recip_q16 = psx_gcs_recip_for(scale);
    g_psx_gcs_frac = 0;
    g_psx_gcs_batch = batch;
    psx_cycle_count = 0;
    s_deadline_step = 997u;
    psx_next_service_cycle = 997u;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    s_services = 0;
    s_check = 0;
    for (uint32_t i = 1; i <= charges; i++) {
        psx_cyc_charge(1u);
        if (i % every == 0) { psx_cyc_batch_flush(); s_check++; }
    }
    psx_cyc_batch_flush();
    g_psx_gcs_batch = 0;
    s_deadline_step = 100000u;
    return psx_cycle_count;
}

static void check_batch_matches(uint32_t scale, uint32_t charges, uint32_t every) {
    uint64_t off_cycle[64]; int off_check[64];
    uint64_t t_off = run_checks(scale, 0, charges, every);
    int n_off = s_services;
    for (int i = 0; i < n_off && i < 64; i++) { off_cycle[i] = s_service_cycle[i]; off_check[i] = s_service_check[i]; }
    uint64_t t_on = run_checks(scale, 1, charges, every);
    assert(t_on == t_off);                      /* same cycle totals */
    assert(s_services <= n_off);                /* never serviced more often */
    /* IRQ phase: every deadline is serviced by the same interrupt check and
     * so delivered there. Batched, the service may happen later inside that
     * check's interval but never at a later check. */
    int j = 0;
    for (int i = 0; i < n_off && i < 64; i++) {
        uint64_t deadline = 997u * (uint64_t)(i + 1);
        (void)off_cycle;
        while (j < s_services && j < 64 && s_service_cycle[j] < deadline) j++;
        assert(j < s_services);
        assert(s_service_check[j] == off_check[i]);
    }
}

int main(void) {
    assert(psx_gcs_recip_for(1) == 65536u);
    assert(psx_gcs_recip_for(0) == 65536u);          /* never slower than faithful */
    assert(psx_gcs_recip_for(2) == 32768u);
    assert(psx_gcs_recip_for(16) == 4096u);
    assert(psx_gcs_recip_for(999) == 1024u);         /* clamped to 64 */

    /* 1 (faithful): identical totals for every charge size. */
    assert(run(1, 1000, 1) == 1000u);
    assert(run(1, 1000, 7) == 7000u);

    /* Power-of-two scales: no drift across many 1-cycle charges. */
    assert(run(2, 1000, 1) == 500u);
    assert(run(4, 1000, 5) == 1250u);
    assert(run(16, 160000, 1) == 10000u);

    /* Non-power-of-two keeps the fraction: scale 3 of 3000 cycles is ~1000. */
    uint64_t t = run(3, 3000, 1);
    assert(t >= 999u && t <= 1001u);

    /* Batching the scaled charge changes neither the totals nor the
     * interrupt check at which each deadline is seen. */
    check_batch_matches(2, 20000, 7);
    check_batch_matches(3, 30000, 5);
    check_batch_matches(2, 20000, 64);
    return 0;
}
