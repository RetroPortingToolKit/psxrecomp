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

void psx_devices_service_to_now(void) { psx_next_service_cycle = psx_cycle_count + 100000u; }
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
    return 0;
}
