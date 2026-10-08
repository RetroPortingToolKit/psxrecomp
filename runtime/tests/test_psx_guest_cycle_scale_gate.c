/* [timing] guest_cycle_scale against the real psx_cycles.c:
 *  - identical accounting across tiers: native per-instruction CPU charges,
 *    the batched psx_cyc_charge hot path and an overlay DLL's raw batch
 *    published once all reach the same guest total; at 1 the CPU charge is
 *    exactly psx_advance_cycles;
 *  - the declarative RAM gate (judged at VBlank) and the mod gate;
 *  - rollback: restoring the snapshot (fraction + gate state) makes a
 *    re-simulation land on the same cycle.
 *
 * Build/run: ctest -R psx_guest_cycle_scale_gate_test
 */
#include "psx_cyc.h"
#include "mod_plugins.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

volatile int g_ds_recording = 0;
uint8_t *g_psx_ram = 0;
uint32_t g_psx_ram_mask = 0x1FFFFFu;
int g_psx_load_delay = 1;

/* assert() is compiled out in Release (NDEBUG); this check is not. */
#define assert(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
    exit(1); } } while (0)

#include <stdint.h>
#include <stdio.h>

int g_ls_replay_active = 0;
int g_ls_mode = 0;
int g_precise_mode = 0;
int g_psx_call_bail = 0;
uint32_t i_mask = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;

static uint32_t s_cd_cycles_remaining = 5;
static int s_cd_ready = 0;
static uint32_t s_dma_ready_cycles = 0;

void sio_advance(uint32_t cycles) { (void)cycles; }

void cdrom_advance(uint32_t cycles) {
    if (s_cd_ready) return;
    if (cycles >= s_cd_cycles_remaining) {
        s_cd_cycles_remaining = 0;
        s_cd_ready = 1;
    } else {
        s_cd_cycles_remaining -= cycles;
    }
}

void dma_advance(uint32_t cycles) {
    if (s_cd_ready) s_dma_ready_cycles += cycles;
}

void timers_advance(uint32_t cycles) { (void)cycles; }
void interrupts_advance_cycles(uint32_t cycles) { (void)cycles; }
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) { return UINT32_MAX; }
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) {
    (void)mask;
    return s_cd_ready ? UINT32_MAX : s_cd_cycles_remaining;
}
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t mask) {
    (void)mask;
    return UINT32_MAX;
}
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
/* SPU sample-event scheduler (golden 1a973806): psx_cycles.c consults it; stub here. */
static uint32_t s_spu_next_sample = UINT32_MAX;
uint32_t psx_spu_sample_event_cycles_to_next(void) { return s_spu_next_sample; }
void psx_spu_sample_event_service(void) {}
int psx_get_in_exception(void) { return 0; }

void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}

int  psx_netplay_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}

static uint8_t s_ram[0x200000];
static uint32_t read_ram(uint32_t phys, uint32_t size) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++) v |= (uint32_t)s_ram[(phys + i) & 0x1FFFFFu] << (8u * i);
    return v;
}
static void wr32(uint32_t addr, uint32_t v) {
    uint32_t a = addr & 0x1FFFFFu;
    for (int i = 0; i < 4; i++) s_ram[a + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

static void reset_clock(void) {
    psx_cycles_reset_for_boot();
    g_psx_gcs_frac = 0u;
}

/* The same instruction stream, charged the three ways tiers charge it. */
static const uint32_t k_stream[] = { 1, 1, 7, 1, 2, 5, 1, 1, 1, 4, 3, 1, 9, 1, 1, 6 };
enum { K_N = sizeof k_stream / sizeof k_stream[0], K_REPS = 997 };

static uint64_t run_native(void) {               /* psx_cpu_charge per charge */
    reset_clock();
    for (int r = 0; r < K_REPS; r++)
        for (int i = 0; i < K_N; i++) psx_cpu_charge(k_stream[i]);
    return psx_get_cycle_count();
}
static uint64_t run_batched(void) {              /* psx_cyc_charge hot path */
    reset_clock();
    for (int r = 0; r < K_REPS; r++)
        for (int i = 0; i < K_N; i++) psx_cyc_charge(k_stream[i]);
    psx_cyc_batch_flush();
    return psx_get_cycle_count();
}
static uint64_t run_dll(void) {                  /* raw DLL batch, published per rep */
    reset_clock();
    for (int r = 0; r < K_REPS; r++) {
        uint32_t pending = 0;
        for (int i = 0; i < K_N; i++) pending += k_stream[i];
        psx_cpu_charge(pending);                 /* the cpu_charge callback */
    }
    return psx_get_cycle_count();
}
static uint64_t run_device(void) {               /* the unscaled device advance */
    reset_clock();
    for (int r = 0; r < K_REPS; r++)
        for (int i = 0; i < K_N; i++) psx_advance_cycles(k_stream[i]);
    return psx_get_cycle_count();
}

int main(void) {
    uint64_t raw = 0;
    for (int i = 0; i < K_N; i++) raw += k_stream[i];
    raw *= K_REPS;

    /* Scale 1: every path is the faithful count, and the CPU charge is the
     * device advance (pass-through, fraction untouched). */
    psx_guest_cycle_scale_set(1);
    assert(run_native() == raw && run_batched() == raw &&
           run_dll() == raw && run_device() == raw);
    assert(g_psx_gcs_frac == 0u);

    /* Scaled: identical across tiers; device time is never scaled. */
    for (uint32_t s = 2; s <= 64; s = s * 2 + 1) {
        psx_guest_cycle_scale_set(s);
        uint64_t n = run_native();
        assert(run_batched() == n);
        assert(run_dll() == n);
        assert(n <= raw / s + 1 && n + 1 >= raw / s);
        assert(run_device() == raw);
    }
    psx_guest_cycle_scale_set(1);

    /* Declarative RAM gate: shut until a VBlank sees the predicate hold. */
    psx_guest_cycle_scale_set_ram_reader(read_ram);
    assert(!psx_guest_cycle_scale_ram_gate_add(0x800AC796u, 4, ~0u, 0x180u)); /* unaligned */
    assert(!psx_guest_cycle_scale_ram_gate_add(0x1F800000u, 4, ~0u, 0x180u)); /* not RAM */
    assert(!psx_guest_cycle_scale_ram_gate_add(0x800AC794u, 3, ~0u, 0x180u)); /* size */
    assert(psx_guest_cycle_scale_ram_gate_add(0x800AC794u, 4, ~0u, 0x180u));
    psx_guest_cycle_scale_set(64);
    assert(psx_guest_cycle_scale() == 1u);
    wr32(0x800AC794u, 0x180u);
    assert(psx_guest_cycle_scale() == 1u);       /* judged at VBlank only */
    psx_guest_cycle_scale_vblank();
    assert(psx_guest_cycle_scale() == 64u);
    wr32(0x800AC794u, 0x100u);
    psx_guest_cycle_scale_vblank();
    assert(psx_guest_cycle_scale() == 1u);
    /* Masked byte predicate ANDed with the first. */
    assert(psx_guest_cycle_scale_ram_gate_add(0x00010003u, 1, 0x0Fu, 0x05u));
    wr32(0x800AC794u, 0x180u);
    s_ram[0x10003] = 0xF4;
    psx_guest_cycle_scale_vblank();
    assert(psx_guest_cycle_scale() == 1u);
    s_ram[0x10003] = 0xA5;
    psx_guest_cycle_scale_vblank();
    assert(psx_guest_cycle_scale() == 64u);

    /* Mod gate on top: both must be open. */
    psx_guest_cycle_scale_set_gated(1);
    assert(psx_guest_cycle_scale() == 1u);
    psx_mod_set_guest_cycle_scale_gate(1);
    assert(psx_mod_guest_cycle_scale() == 64u);
    psx_guest_cycle_scale_ram_gate_clear();
    assert(psx_guest_cycle_scale() == 64u);      /* mod gate alone */
    psx_mod_set_guest_cycle_scale_gate(0);
    assert(psx_guest_cycle_scale() == 1u);
    psx_guest_cycle_scale_set_gated(0);

    /* Rollback: snapshot mid-stream (odd scale leaves a fraction), run a
     * tail, restore, re-simulate: the same cycle. Restoring the clock alone
     * (fraction lost) does not reproduce it. */
    assert(psx_guest_cycle_scale_ram_gate_add(0x800AC794u, 4, ~0u, 0x180u));
    wr32(0x800AC794u, 0x180u);
    psx_guest_cycle_scale_set(3);
    psx_guest_cycle_scale_vblank();
    assert(psx_guest_cycle_scale() == 3u);
    reset_clock();
    for (int i = 0; i < 5; i++) psx_cpu_charge(1);
    assert(g_psx_gcs_frac != 0u);
    uint32_t snap[3];
    psx_guest_cycle_scale_snapshot(snap);
    const uint64_t snap_cyc = psx_get_cycle_count();
    for (int i = 0; i < 1000; i++) psx_cpu_charge(k_stream[i % K_N]);
    wr32(0x800AC794u, 0x100u);                   /* the tail shuts the gate */
    psx_guest_cycle_scale_vblank();
    for (int i = 0; i < 100; i++) psx_cpu_charge(1);
    const uint64_t live = psx_get_cycle_count();

    wr32(0x800AC794u, 0x180u);                   /* guest RAM is restored too */
    psx_cycle_count = snap_cyc;
    psx_guest_cycle_scale_restore(snap);
    assert(psx_guest_cycle_scale() == 3u);
    for (int i = 0; i < 1000; i++) psx_cpu_charge(k_stream[i % K_N]);
    wr32(0x800AC794u, 0x100u);
    psx_guest_cycle_scale_vblank();
    for (int i = 0; i < 100; i++) psx_cpu_charge(1);
    assert(psx_get_cycle_count() == live);

    /* The fraction is load-bearing: two 1-cycle charges after the restore
     * carry a cycle with it and none without it. */
    wr32(0x800AC794u, 0x180u);
    psx_guest_cycle_scale_vblank();
    psx_cycle_count = snap_cyc;
    psx_guest_cycle_scale_restore(snap);
    psx_cpu_charge(1); psx_cpu_charge(1);
    const uint64_t with_frac = psx_get_cycle_count();
    psx_cycle_count = snap_cyc;
    {
        uint32_t lost[3] = { 0u, snap[1], snap[2] };
        psx_guest_cycle_scale_restore(lost);
    }
    psx_cpu_charge(1); psx_cpu_charge(1);
    assert(psx_get_cycle_count() != with_frac);

    puts("psx_guest_cycle_scale_gate_test: ok");
    return 0;
}
