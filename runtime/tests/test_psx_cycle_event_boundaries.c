/*
 * Pin cross-device causality at a scheduler deadline.
 *
 * Build/run: ctest -R psx_cycle_event_boundaries_test
 */

#include "psx_cycles.h"
#include "psx_cycle_freeze.h"
#include "cpu_state.h"
#include "timers.h"
#include <string.h>

#include <stdint.h>
int g_boot_state_cpu_timing_restored;
int psx_mod_host_output_scope(void) { return 0; }
#include <stdio.h>

int g_ls_replay_active = 0;
int g_ls_mode = 0;
int g_precise_mode = 0;
int g_psx_call_bail = 0;
uint32_t i_mask = 0;
uint32_t i_stat = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;

static uint32_t s_cd_cycles_remaining = 5;
static uint32_t s_device_calls;
static uint32_t s_vblank_phase;
static int s_cd_ready = 0;
static uint32_t s_dma_ready_cycles = 0;
static int s_cd_raise_irq;
#if defined(PSX_TEST_BLOCK_MATERIALIZE) && PSX_TEST_BLOCK_MATERIALIZE
#include "psx_native_span.h"
volatile int g_ds_recording;
void (*g_overlay_flush_pending_cycles)(void);
static unsigned s_materializations, s_missing_receipt;
static uint32_t s_receipt;
static int s_require_receipt, s_redirect_observer;
static CPUState *s_receipt_cpu;
static void fixture_check_receipt(void) {
    if(s_require_receipt && (!s_materializations || s_receipt!=0x12345678u))
        ++s_missing_receipt;
}
static void fixture_materialize(CPUState *cpu) {
    ++s_materializations;s_receipt=cpu->gpr[2];s_receipt_cpu=cpu;
}
static void fixture_foreign_flush(void) { ++s_missing_receipt; }
#endif

#if defined(PSX_TEST_OVERLAY_CHARGE) && PSX_TEST_OVERLAY_CHARGE
#define TEST_OVERLAY_CPU_CHARGE(cycles) overlay_cpu_charge(cycles)
#else
#define TEST_OVERLAY_CPU_CHARGE(cycles) psx_advance_cycles(cycles)
#endif

static uint64_t s_sio_cycles, s_timer_cycles;
void sio_advance(uint32_t cycles) { s_sio_cycles += cycles; }

void cdrom_advance(uint32_t cycles) {
    s_device_calls++;
    if (s_cd_ready) return;
    if (cycles >= s_cd_cycles_remaining) {
        s_cd_cycles_remaining = 0;
        s_cd_ready = 1;
        if (s_cd_raise_irq) i_stat |= 1u << 2;
    } else {
        s_cd_cycles_remaining -= cycles;
    }
}

void dma_advance(uint32_t cycles) {
    if (s_cd_ready) s_dma_ready_cycles += cycles;
}

#if defined(PSX_CLOCK_REAL_TIMERS) && PSX_CLOCK_REAL_TIMERS
static uint64_t fixture_timer_cycles(void) { return timers_read(TIMER_BASE); }
static void fixture_timer_reset(void) { timers_init(); }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
#else
void timers_advance(uint32_t cycles) { s_timer_cycles += cycles; }
static uint64_t fixture_timer_cycles(void) { return s_timer_cycles; }
static void fixture_timer_reset(void) { s_timer_cycles = 0; }
#endif
void interrupts_advance_cycles(uint32_t cycles) {
#if defined(PSX_TEST_BLOCK_MATERIALIZE) && PSX_TEST_BLOCK_MATERIALIZE
    fixture_check_receipt();
#endif
    s_vblank_phase += cycles;
}
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) { return 564480u - s_vblank_phase % 564480u; }
#if !defined(PSX_CLOCK_REAL_TIMERS) || !PSX_CLOCK_REAL_TIMERS
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
#endif
uint32_t cdrom_cycles_to_irq(uint32_t mask) {
    return s_cd_ready || !(mask & (1u << 2)) ? UINT32_MAX : s_cd_cycles_remaining;
}
uint32_t cdrom_cycles_to_internal_event(void) { return cdrom_cycles_to_irq(UINT32_MAX); }
static uint32_t s_dma_next_event = UINT32_MAX;
static int s_cd_dma_active;
uint32_t dma_cycles_to_internal_event(void) { return s_dma_next_event; }
int dma_cdrom_transfer_active(void) { return s_cd_dma_active; }
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

#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
/* The delivery body is syntax-qualified separately; these are the inline
 * edge policy's seam, not a substitute implementation of exception handling. */
int psx_defer_switch_pending, g_psx_savestate_pending, g_ls_suppress_record;
int psx_observation_wait_active;
uint64_t psx_observation_block_edges, psx_observation_slow_edges;
static unsigned s_edge_calls, s_wait_calls;
void psx_observation_edge(CPUState *cpu, uint32_t pc, int wait_site) {
    (void)cpu; (void)pc;
    s_edge_calls++;
    if (wait_site) s_wait_calls++;
    psx_observation_wait_active = wait_site;
#if defined(PSX_TEST_BLOCK_MATERIALIZE) && PSX_TEST_BLOCK_MATERIALIZE
    fixture_check_receipt();
#endif
    psx_devices_observe();
#if defined(PSX_TEST_BLOCK_MATERIALIZE) && PSX_TEST_BLOCK_MATERIALIZE
    if(s_redirect_observer) {
        cpu->pc=0x80024680u;cpu->gpr[2]=0x89ABCDEFu;
        g_psx_call_bail=1;g_event_step_conservative=1;g_psx_savestate_pending=0;
    }
#endif
}
extern uint32_t psx_cycle_sample_period;
extern void (*psx_cycle_sample_hook)(uint64_t, uint32_t);
static unsigned s_samples;
static void sample(uint64_t cycle, uint32_t periods) {
    (void)cycle;s_samples += periods;
#if defined(PSX_TEST_BLOCK_MATERIALIZE) && PSX_TEST_BLOCK_MATERIALIZE
    fixture_check_receipt();
    if(s_redirect_observer && s_receipt_cpu) {
        s_receipt_cpu->pc=0x80024680u;s_receipt_cpu->gpr[2]=0x89ABCDEFu;
        g_event_step_conservative=1;
    }
#endif
}
#define VERIFY(c, text) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", text); return 1; } } while (0)
#endif

#if defined(PSX_TEST_BLOCK_MATERIALIZE) && PSX_TEST_BLOCK_MATERIALIZE
static void materialize_case_reset(CPUState *cpu) {
    s_require_receipt=0;
    memset(cpu,0,sizeof *cpu);cpu->gpr[2]=0x12345678u;cpu->pc=0x80010000u;
    g_psx_guest_time_frozen=g_ls_replay_active=g_ls_mode=g_ds_recording=0;
    g_psx_render_pass_active=g_event_step_conservative=g_psx_call_bail=0;
    psx_in_device_service=psx_defer_switch_pending=g_psx_savestate_pending=0;
    psx_observation_wait_active=g_ls_suppress_record=0;
    g_psx_cyc_batch=0;g_psx_cyc_local_acc=NULL;g_overlay_flush_pending_cycles=NULL;
    i_stat=i_mask=0;s_cd_raise_irq=0;s_cd_ready=1;s_cd_dma_active=0;
    s_dma_next_event=s_spu_next_sample=UINT32_MAX;s_vblank_phase=0;
    psx_cycle_count=0;psx_cycle_sample_period=0;psx_cycle_sample_hook=NULL;
    psx_cycles_resync_after_restore(NULL);
    g_psx_gcs_recip_q16=65536u;g_psx_gcs_frac=0;
    psx_next_service_cycle=10000u;psx_next_cycle_sample=UINT64_MAX;
    s_materializations=s_missing_receipt=0;s_receipt=0;
    s_redirect_observer=0;s_receipt_cpu=NULL;s_require_receipt=1;
}
static int materialized_edge_products(void) {
    CPUState cpu;
    materialize_case_reset(&cpu);
    const uint64_t edges=psx_observation_block_edges;
    const unsigned devices=s_device_calls,slow=s_edge_calls;
    g_ls_suppress_record=2;
    VERIFY(!psx_block_edge_materialize(&cpu,0x80010000u,fixture_materialize) &&
           !s_materializations && psx_observation_block_edges==edges+1 &&
           g_ls_suppress_record==1 && s_edge_calls==slow,
           "quiet materializing edge keeps maintained counts/suppression without receipts or callbacks");
    VERIFY(!psx_native_span_charge_observable(3u),"quiet actual charge has no observer");
    psx_native_span_charge(3u);
    VERIFY(psx_cycle_count==3u && s_device_calls==devices && !s_materializations,
           "quiet charged region advances the real clock without materialization/device walks");

    materialize_case_reset(&cpu);
    cpu.cop0[12]=0x401u;i_stat=i_mask=4u;
    VERIFY(psx_block_edge_materialize(&cpu,0x80010000u,fixture_materialize) &&
           s_materializations==1 && !s_missing_receipt,
           "due IRQ materializes before the real observation boundary");

    materialize_case_reset(&cpu);
    psx_devices_refresh_observation_deadline();
    VERIFY(!psx_native_span_charge_observable(564480u),
           "device deadline alone does not invent a callback in an on-demand charge");
    psx_native_span_charge(564480u);
    VERIFY(s_vblank_phase==0 && psx_cycle_count==564480u,
           "frame work remains deferred to the maintained observation edge");
    VERIFY(psx_block_edge_materialize(&cpu,0x80010000u,fixture_materialize) &&
           s_materializations==1 && !s_missing_receipt && s_vblank_phase==564480u,
           "frame publication sees materialized receipts before catch-up");

    materialize_case_reset(&cpu);
    g_psx_savestate_pending=1;s_redirect_observer=1;
    VERIFY(psx_block_edge_materialize(&cpu,0x80010000u,fixture_materialize) &&
           s_materializations==1 && !s_missing_receipt && s_receipt==0x12345678u &&
           cpu.pc==0x80024680u && cpu.gpr[2]==0x89ABCDEFu &&
           g_psx_call_bail && g_event_step_conservative && !g_psx_savestate_pending,
           "returning observer CPU redirect/bail/policy is never overwritten by cold-edge publication");

    materialize_case_reset(&cpu);
    psx_cycle_sample_period=4;psx_cycle_sample_hook=sample;psx_next_cycle_sample=2;
    g_psx_gcs_recip_q16=21845u;g_psx_gcs_frac=65535u;
    const unsigned samples=s_samples;
    VERIFY(psx_native_span_charge_observable(4u) && !psx_native_span_charge_observable(1u) &&
           !psx_native_span_charge_observable(0u) && psx_cycle_count==0 && g_psx_gcs_frac==65535u,
           "upcoming charge preview is pure and includes exact fractional carry/sample boundary");
    fixture_materialize(&cpu);s_redirect_observer=1;
    psx_native_span_charge(4u);
    VERIFY(psx_cycle_count==2u && g_psx_gcs_frac==21843u && s_samples==samples+1 &&
           !s_missing_receipt && cpu.pc==0x80024680u && cpu.gpr[2]==0x89ABCDEFu,
           "sample callback sees receipts before actual charge and retains its CPU result");

    materialize_case_reset(&cpu);
    psx_cycle_sample_period=4;psx_cycle_sample_hook=sample;psx_next_cycle_sample=2;
    g_psx_cyc_batch=2;
    const unsigned batched_samples=s_samples;
    VERIFY(psx_native_span_charge_observable(0u) && g_psx_cyc_batch==2u && psx_cycle_count==0,
           "pending charge preview never flushes or consumes a batch");
    VERIFY(psx_block_edge_materialize(&cpu,0x80010000u,fixture_materialize) &&
           s_materializations==1 && !s_missing_receipt && !g_psx_cyc_batch &&
           psx_cycle_count==2u && s_samples==batched_samples+1,
           "edge materializes before batch publication can sample the CPU");

    materialize_case_reset(&cpu);
    g_overlay_flush_pending_cycles=fixture_foreign_flush;
    VERIFY(psx_native_span_charge_observable(1u) && !s_missing_receipt && psx_cycle_count==0,
           "foreign pending owner is conservatively observable without invoking it");
    VERIFY(psx_block_edge_materialize(&cpu,0x80010000u,fixture_materialize) &&
           s_materializations==1 && !s_missing_receipt,
           "foreign-owner private edge escapes without adding a new flush callback");

    materialize_case_reset(&cpu);
    uint32_t local=0;g_psx_cyc_local_acc=&local;
    VERIFY(psx_native_span_charge_observable(1u) && !local && psx_cycle_count==0,
           "even an empty local charge owner requires canonical execution");
    g_psx_cyc_local_acc=NULL;
    for(unsigned mode=0;mode<4;mode++) {
        materialize_case_reset(&cpu);
        if(mode==0)g_psx_guest_time_frozen=1;
        if(mode==1)g_ls_replay_active=1;
        if(mode==2)g_event_step_conservative=1;
        if(mode==3)psx_in_device_service=1;
        const uint64_t before=psx_observation_block_edges;
        VERIFY(psx_native_span_charge_observable(1u) && psx_cycle_count==0,
               "unsupported charge contexts stay pure and canonical");
        VERIFY(psx_block_edge_materialize(&cpu,0x80010000u,fixture_materialize) &&
               s_materializations==1 && !s_missing_receipt,
               "unsupported private edge materializes before canonical escape");
        VERIFY(psx_observation_block_edges==before+(mode<2?0u:1u),
               "freeze/replay keep original public edge bookkeeping");
    }
    materialize_case_reset(&cpu);s_require_receipt=0;
    fprintf(stderr,"PASS cold materialization before IRQ/frame/sample/batch callbacks, pure fractional charge preview, redirect/bail and canonical unsupported paths\n");
    return 0;
}
#endif

#if defined(PSX_CLOCK_REAL_GPU) && PSX_CLOCK_REAL_GPU
/* As in gpu_interlace_status_test, whole-program optimization discards
 * unrelated renderer code. Keep the actual reader and lazy scheduler in one
 * TU so their real dependency is exercised without renderer/provider stubs. */
#include "../src/psx_cycles.c"
#include "../src/gpu.c"
uint32_t g_psx_vblank_cycles = 564480u;
uint32_t interrupts_get_cycles_since_vblank(void) { return s_vblank_phase; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
static int gpustat_clock_product(void) {
    g_event_step_conservative = g_psx_guest_time_frozen = g_ls_replay_active = 0;
    g_psx_render_pass_active = 0; i_mask = i_stat = 0;
    psx_cycle_sample_period = 0; psx_cycle_sample_hook = NULL;
    s_cd_ready = 1; s_cd_dma_active = 0; s_device_calls = 0;
    s_dma_next_event = s_spu_next_sample = UINT32_MAX;
    s_vblank_phase = 0; s_sio_cycles = 0; fixture_timer_reset();
    psx_cycles_reset_for_boot();
    vertical_interlace = vres = lcf = 1u; video_mode = 0u;
    v_display_y1 = 16u; v_display_y2 = 256u;
    const uint32_t blank_status = gpu_read_gpustat();
    VERIFY(!(blank_status & 0x80000000u), "field starts with blanking LCF masked");
    psx_advance_cycles(1000);
    psx_devices_mmio_sync_at(0x1F801810u, 0); /* GP0 read */
    psx_devices_mmio_sync_at(0x1F801814u, 1); /* GP1 mode write */
    psx_devices_mmio_sync_at(0x1F801820u, 0); /* MDEC read */
    VERIFY(!s_vblank_phase && !s_sio_cycles && !s_device_calls &&
           fixture_timer_cycles() == 1000,
           "GP0/MDEC and GP1 writes retain DMA/timer-only observation");
    psx_devices_mmio_sync_at(0x1F801814u, 0);
    VERIFY(s_vblank_phase == 1000 && !(gpu_read_gpustat() & 0x80000000u),
           "GPUSTAT observes elapsed blanking phase without exposing active LCF");
    psx_advance_cycles(59000);
    VERIFY(s_vblank_phase == 1000 && !(gpu_read_gpustat() & 0x80000000u),
           "unobserved frame phase remains stale despite active guest time");
    psx_devices_mmio_sync_at(0x1F801817u, 0); /* byte-addressed same register */
    const uint32_t active_status = gpu_read_gpustat();
    VERIFY(s_vblank_phase == 60000 && psx_cycle_count == 60000 &&
           ((active_status ^ blank_status) & 0x80000000u),
           "GPUSTAT observation releases the real active-field LCF change poll");
    psx_devices_mmio_sync_at(0x1F801814u, 0);
    VERIFY(s_vblank_phase == 60000 && (gpu_read_gpustat() & 0x80000000u) &&
           !s_sio_cycles && !s_device_calls,
           "repeated GPUSTAT polls advance no time or unrelated quiet devices");
    fprintf(stderr, "PASS lazy frame observation releases active-field GPUSTAT LCF poll\n");
    return 0;
}
#endif

int main(void) {
    /* NULL cpu: this test pins the scheduler boundary, not the CPU-state
     * rewind. psx_cycles_resync_after_restore guards that block on `if (cpu)`. */
    psx_cycles_resync_after_restore(NULL);
#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
    psx_cycle_sample_period = 4;
    psx_cycle_sample_hook = sample;
    for (unsigned n = 0; n < 5; n++) psx_advance_cycles(1);
    VERIFY(!s_cd_ready && s_device_calls == 0, "CPU charges never service masked devices");
    VERIFY(s_samples == 1, "periodic sampling runs independently of devices");
    psx_devices_mmio_sync();
#else
    psx_advance_cycles(5);
#endif

    if (psx_get_cycle_count() != 5) {
        fprintf(stderr, "FAIL cycle count: expected 5 got %llu\n",
                (unsigned long long)psx_get_cycle_count());
        return 1;
    }
    if (!s_cd_ready) {
        fprintf(stderr, "FAIL CD event did not fire at cycle 5\n");
        return 1;
    }
    if (s_dma_ready_cycles != 1) {
        fprintf(stderr,
                "FAIL retroactive DMA credit: expected 1 boundary cycle got %u\n",
                s_dma_ready_cycles);
        return 1;
    }

    /* Idle-skip observation boundary vs the SPU sample scheduler (mstan/psxrecomp#239
     * review): with IRQ9 unmasked, a wait loop must stop at the FIRST 768-cycle
     * sample boundary, not be skipped across several; with IRQ9 masked the
     * sample deadline is not observable and must not shorten the skip. */
    s_cd_ready = 1;                       /* no CD event ahead */
    s_spu_next_sample = 300;              /* next sample boundary in 300 cycles */
    i_mask = 0;
    if (psx_idle_cycles_to_next_observable_event() == 300) {
        fprintf(stderr, "FAIL masked SPU IRQ9 must not bound the idle skip\n");
        return 1;
    }
    i_mask = 1u << 9;                     /* IRQ_SPU unmasked */
    if (psx_idle_cycles_to_next_observable_event() != 300) {
        fprintf(stderr, "FAIL idle skip must stop at the first SPU sample boundary (300), got %u\n",
                psx_idle_cycles_to_next_observable_event());
        return 1;
    }
    s_spu_next_sample = 768 * 4;          /* several boundaries away: still the nearest event */
    if (psx_idle_cycles_to_next_observable_event() != 768 * 4) {
        fprintf(stderr, "FAIL idle skip must not cross a later SPU sample boundary either\n");
        return 1;
    }
    s_spu_next_sample = UINT32_MAX; i_mask = 0;

#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
    /* MMIO side effects occur after its entry barrier: a newly armed event
     * must replace the pre-write horizon before any edge can run past it. */
    psx_devices_mmio_sync();
    s_cd_ready = 0; s_cd_cycles_remaining = 3; i_mask = 1u << 2;
    unsigned calls_before = s_device_calls;
    psx_advance_cycles(2);
    psx_devices_refresh_observation_deadline();
    VERIFY(psx_next_service_cycle == 8 && psx_cycle_count == 7,
           "MMIO mutation horizon uses the synchronized device clock");
    VERIFY(s_device_calls == calls_before, "refresh does not step devices");
    psx_advance_cycles(1);
    VERIFY(!s_cd_ready, "deadline alone does not step devices before a CPU edge");
    psx_devices_observe();
    VERIFY(s_cd_ready && s_dma_ready_cycles == 2, "unmasked event retains D-1 + 1 causality");

    /* IRQ masking cannot hide DMA publication into CPU-visible RAM. A CD
     * transfer blocked on sector input must also observe masked CD readiness. */
    i_mask = 0; s_cd_ready = 0; s_cd_cycles_remaining = 7;
    s_dma_next_event = 3; s_cd_dma_active = 1;
    psx_devices_refresh_observation_deadline();
    VERIFY(psx_next_service_cycle == 11, "masked DMA RAM publication remains observable");
    s_dma_next_event = UINT32_MAX;
    psx_devices_refresh_observation_deadline();
    VERIFY(psx_next_service_cycle == 15, "masked CD readiness bounds a dependent RAM transfer");
    s_cd_dma_active = 0; i_mask = 1u << 2;

    /* A latched source is not a new observation until guest acknowledgement.
     * Unknown loops still have an unconditional frame boundary. */
    s_cd_ready = 0; s_cd_cycles_remaining = 1; i_stat = i_mask;
    psx_devices_refresh_observation_deadline();
    VERIFY(psx_next_service_cycle == 564480, "latched IRQ cannot churn one-cycle checks");
    i_stat = 0; s_cd_ready = 1; i_mask = 0;
    psx_devices_refresh_observation_deadline();
    CPUState cpu = {0};
    calls_before = s_device_calls;
    for (unsigned n = 0; n < 1000; n++) { psx_advance_cycles(1); PSX_BLOCK_EDGE(&cpu, 0x80010000u); }
    VERIFY(s_edge_calls == 0 && s_device_calls == calls_before,
           "ordinary and unrecognized loops have no out-of-line edge/device calls");
    cpu.cop0[12] /* SR */ = 0x101u; cpu.cop0[13] /* CAUSE */ = 0x100u;
    PSX_BLOCK_EDGE(&cpu, 0x80010000u);
    VERIFY(s_edge_calls == 1, "software interrupt forces observation");
    cpu.cop0[12] /* SR */ = 0; cpu.cop0[13] /* CAUSE */ = 0;
    psx_defer_switch_pending = 1; PSX_BLOCK_EDGE(&cpu, 0x80010000u); psx_defer_switch_pending = 0;
    g_psx_savestate_pending = 1; PSX_BLOCK_EDGE(&cpu, 0x80010000u); g_psx_savestate_pending = 0;
    VERIFY(s_edge_calls == 3, "deferred thread and staged save force observation");
    PSX_BLOCK_EDGE(&cpu, 0x8002E6C0u);
    PSX_BLOCK_EDGE(&cpu, 0x80010000u);
    VERIFY(s_wait_calls == 1 && !psx_observation_wait_active && s_edge_calls == 5,
           "only audited anchor enters induction and departure resets once");
    uint64_t now = psx_cycle_count;
    psx_advance_cycles((uint32_t)(564480 - now));
    VERIFY(s_vblank_phase == now, "charges do not advance frame state");
    PSX_BLOCK_EDGE(&cpu, 0x80010000u);
    VERIFY(s_vblank_phase == 564480 && s_edge_calls == 6, "unknown loop makes bounded frame progress");

    PsxCycleFreeze save;
    now = psx_cycle_count; calls_before = s_device_calls;
    uint64_t sample_deadline = psx_next_cycle_sample;
    VERIFY(psx_cycle_freeze_begin(&save, 0, NULL), "freeze starts");
    TEST_OVERLAY_CPU_CHARGE(1000); psx_devices_mmio_sync(); PSX_BLOCK_EDGE(&cpu, 0x8002E6C0u);
    VERIFY(s_device_calls == calls_before && s_edge_calls == 6, "frozen rendering owns no observations");
    psx_cycle_freeze_end(&save);
    VERIFY(psx_cycle_count == now && psx_next_cycle_sample == sample_deadline,
           "freeze restores clock and sampling cadence");
    g_ls_replay_active = 1;
    VERIFY(psx_cycle_replay_begin(123), "private replay starts");
    TEST_OVERLAY_CPU_CHARGE(100); psx_devices_mmio_sync(); PSX_BLOCK_EDGE(&cpu, 0x8002E6C0u);
    VERIFY(psx_cycle_replay_end() == 223 && psx_cycle_count == now,
           "private replay restores authoritative clock");
    g_ls_replay_active = 0;
    VERIFY(s_device_calls == calls_before && s_edge_calls == 6, "replay never observes authoritative devices");
    psx_cycle_count = 50; psx_cycles_resync_after_restore(NULL);
    VERIFY(psx_next_service_cycle == 0 && psx_next_cycle_sample == 0,
           "restore rederives clock-dependent horizons");
#if defined(PSX_TEST_OVERLAY_CHARGE) && PSX_TEST_OVERLAY_CHARGE
    /* A native DLL's charge precedes its no-pending-IRQ fast return. An
     * overdue completion must become visible even when no IRQ is latched yet. */
    s_cd_ready = 0; s_cd_cycles_remaining = 5; s_cd_raise_irq = 1;
    i_mask = 1u << 2; i_stat = 0;
    psx_devices_refresh_observation_deadline();
    psx_advance_cycles(9);
    VERIFY(!s_cd_ready && i_stat == 0 && psx_cycle_count > psx_next_service_cycle,
           "overlay regression starts overdue with no latched IRQ");
    TEST_OVERLAY_CPU_CHARGE(1);
    VERIFY(s_cd_ready && (i_stat & i_mask),
           "overlay charge raises overdue completion before IRQ fast return");
    calls_before = s_device_calls;
    TEST_OVERLAY_CPU_CHARGE(1);
    VERIFY(s_device_calls == calls_before,
           "overlay charge under the next horizon does not service devices");
    /* The new DLL edge owns the raw charge and enters the same native
     * observation handler. No previously-latched IRQ is needed to observe a
     * completion; the mandatory eager charge callback above stays intact. */
    s_cd_ready = 0; s_cd_cycles_remaining = 5; i_stat = 0;
    psx_devices_refresh_observation_deadline();
    const PSXObservationView *dll_clock = overlay_observation_view();
    VERIFY(dll_clock->bytes == sizeof(*dll_clock) && dll_clock->model == 1 &&
           dll_clock->clock == &psx_cycle_count &&
           dll_clock->scale_frac == &g_psx_gcs_frac &&
           dll_clock->deadline == &psx_next_service_cycle &&
           dll_clock->block_edges == &psx_observation_block_edges &&
           dll_clock->wait_site_count == 1 && dll_clock->wait_sites[0] == 0x8002E6C0u,
           "actual DLL descriptor borrows real clock/fraction/deadline/M0 and audited PCs");
    unsigned edges_before = s_edge_calls;
    dll_clock->edge(&cpu, 0x80010000u, 0, 9);
    VERIFY(s_cd_ready && (i_stat & i_mask) && s_edge_calls == edges_before + 1,
           "actual DLL observation callback publishes and catches overdue CD without latched IRQ");
    dll_clock->edge(&cpu, 0x8002E6C0u, 1, 1);
    VERIFY(psx_observation_wait_active,
           "actual DLL observation callback enters host audited wait induction");
    dll_clock->edge(&cpu, 0x80010000u, 0, 0);
    VERIFY(!psx_observation_wait_active,
           "actual DLL observation callback resets induction on departure");
    uint64_t blocks_before = psx_observation_block_edges;
    uint64_t slow_before = psx_observation_slow_edges;
    overlay_note_skipped_edge();
    VERIFY(psx_observation_block_edges == blocks_before + 1 &&
           psx_observation_slow_edges == slow_before + 1,
           "actual conservative DLL early return fills the existing M0 lanes once");
    g_psx_guest_time_frozen = 1; overlay_note_skipped_edge(); g_psx_guest_time_frozen = 0;
    g_ls_replay_active = 1; overlay_note_skipped_edge(); g_ls_replay_active = 0;
    VERIFY(psx_observation_block_edges == blocks_before + 1,
           "private frozen/replay edges do not enter authoritative M0 counts");
    s_cd_ready = 0; s_cd_cycles_remaining = 5; i_stat = 0;
    psx_devices_refresh_observation_deadline();
    uint32_t local_charge = 9;
    g_psx_cyc_local_acc = &local_charge;
    unsigned legacy_before = s_fixture_legacy_checks;
    dll_clock->fallback(&cpu, 0x80010000u, 0);
    VERIFY(local_charge == 0 && s_cd_ready && (i_stat & i_mask) &&
           s_fixture_legacy_checks == legacy_before + 1,
           "actual conservative callback publishes host-local work and overdue CD before legacy wrapper");
    g_psx_cyc_local_acc = NULL;
    uint64_t before_scaled = psx_cycle_count;
    uint32_t saved_recip = g_psx_gcs_recip_q16, saved_frac = g_psx_gcs_frac;
    g_psx_gcs_recip_q16 = 21845; g_psx_gcs_frac = 65535;
    VERIFY(psx_gcs_scaled_charge(4, g_psx_gcs_recip_q16, g_psx_gcs_frac) == 2 &&
           g_psx_gcs_frac == 65535,
           "DLL clock preview reads fractional carry without publishing it");
    TEST_OVERLAY_CPU_CHARGE(4);
    VERIFY(psx_cycle_count == before_scaled + 2 && g_psx_gcs_frac == 21843,
           "actual host charge exactly matches DLL preview and publishes fractional carry once");
    g_psx_gcs_recip_q16 = saved_recip; g_psx_gcs_frac = saved_frac;
    s_cd_raise_irq = 0; i_mask = 0; i_stat = 0;
#endif
    fprintf(stderr, "PASS observation charges/edges, MMIO arm, IRQ/frame, waits, sample, freeze/replay/restore\n");
#endif

#if !defined(PSX_DISABLE_HEADROOM_COUNTERS) || !PSX_DISABLE_HEADROOM_COUNTERS
    extern uint32_t psx_cycle_sample_period;
    extern void (*psx_cycle_sample_hook)(uint64_t,uint32_t);
#ifndef VERIFY
#define VERIFY(test, message) do { if(!(test)) { fprintf(stderr,"FAIL %s\n",message);return 1; } } while(0)
#endif
    /* Counting must reuse the scheduler's decisions and classify an actual
     * service, including an SPU deadline, without changing its timeline. */
    g_event_step_conservative=0;psx_cycle_sample_period=0;psx_cycle_sample_hook=NULL;
    s_cd_ready=1;s_dma_next_event=UINT32_MAX;s_cd_dma_active=0;
    uint64_t gpu_before=psx_hr_service_requests[PSX_HR_REQUEST_GPU_HOLD];
    psx_devices_service_to_now_cause(PSX_HR_REQUEST_GPU_HOLD);
    VERIFY(psx_hr_service_requests[PSX_HR_REQUEST_GPU_HOLD]==gpu_before+1,"explicit GPU hold service counted");
    psx_next_service_cycle=UINT64_MAX;
    psx_advance_cycles(2);
    uint64_t mmio_before=psx_hr_service_requests[PSX_HR_REQUEST_MMIO];
    psx_devices_mmio_sync();
    VERIFY(psx_hr_service_requests[PSX_HR_REQUEST_MMIO]==mmio_before+1,"MMIO service counted");
    s_spu_next_sample=3;i_mask=1u<<9;i_stat=0;
    psx_devices_service_to_now();
    uint64_t spu_before=psx_hr_service_deadlines[PSX_HR_DEADLINE_SPU];
    psx_advance_cycles(3);psx_devices_service_to_now();
    VERIFY(psx_hr_service_deadlines[PSX_HR_DEADLINE_SPU]>spu_before,"advertised SPU deadline counted");
    uint64_t implicit_before=psx_hr_gpu_hold_implicit_services;
    gpu_before=psx_hr_service_requests[PSX_HR_REQUEST_GPU_HOLD];
    PSX_HR_GPU_HOLD_BEGIN();
    VERIFY(psx_hr_gpu_hold_depth==1,"GPU hold enters attribution context");
    psx_next_service_cycle=0;
    psx_advance_cycles(1);
#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
    /* This policy defers automatic device work; the generic catch-up entry
     * must still inherit the synchronous hold's attribution context. */
    psx_devices_service_to_now();
#endif
    PSX_HR_GPU_HOLD_END();
    VERIFY(!psx_hr_gpu_hold_depth,"GPU hold context ends after catch-up");
    VERIFY(psx_hr_gpu_hold_implicit_services==implicit_before+1 &&
           psx_hr_service_requests[PSX_HR_REQUEST_GPU_HOLD]==gpu_before+1,
           "implicit GPU-hold service counted in hold request partition");
    uint64_t other_before=psx_hr_service_requests[PSX_HR_REQUEST_OTHER];
    psx_devices_service_to_now();
    VERIFY(psx_hr_service_requests[PSX_HR_REQUEST_OTHER]==other_before+1,
           "GPU hold context cannot leak into later ordinary work");
    uint64_t requests=0,deadlines=0;
    for(unsigned i=0;i<PSX_HR_REQUEST_COUNT;++i)requests+=psx_hr_service_requests[i];
    for(unsigned i=0;i<PSX_HR_DEADLINE_COUNT;++i)deadlines+=psx_hr_service_deadlines[i];
    VERIFY(requests==psx_device_service_calls && deadlines==requests,"service classifications partition actual calls");
    PSX_HR_GPU_HOLD_BEGIN();psx_cycles_reset_for_boot();PSX_HR_GPU_HOLD_END();
    VERIFY(!psx_hr_gpu_hold_depth,"reset/early-clear cannot leak or underflow hold context");
#endif
#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME
    /* Addressed GP0/timer observations must not advance unrelated quiet
     * SIO/CD/frame state. Materialization is required at an actual read or
     * full snapshot barrier, including at the same global timeline position. */
    g_event_step_conservative = 0; i_mask = 0; i_stat = 0;
    s_cd_ready = 1; s_dma_next_event = UINT32_MAX; s_spu_next_sample = UINT32_MAX;
    s_cd_dma_active = 0; s_vblank_phase = 0; s_sio_cycles = 0; fixture_timer_reset();
    psx_cycles_reset_for_boot();
    calls_before = s_device_calls;
    psx_advance_cycles(37); psx_devices_mmio_sync_at(0x1F801810u, 0);
    psx_advance_cycles(11); psx_devices_mmio_sync_at(0x1F801810u, 0);
    VERIFY(s_device_calls == calls_before && s_sio_cycles == 0 &&
           fixture_timer_cycles() == 0 && s_vblank_phase == 0,
           "GPU MMIO leaves unrelated quiet devices unmaterialized");
    psx_advance_cycles(13); psx_devices_mmio_sync_at(0x1F801100u, 0);
    VERIFY(fixture_timer_cycles() == 61 && s_device_calls == calls_before && s_sio_cycles == 0,
           "timer reads materialize elapsed arithmetic without unrelated catch-up");
#ifdef PSX_ENABLE_BLOCK_CYCLES
    VERIFY(psx_next_service_cycle > psx_cycle_count,
           "timed timer reads retain the freshly computed schedule");
#endif
    psx_devices_mmio_sync_at(0x1F8010A8u, 0);
    VERIFY(psx_next_service_cycle > psx_cycle_count,
           "DMA register polling retains the freshly computed schedule");
    const unsigned reads_edge_calls = s_edge_calls;
    cpu.cop0[12] /* SR */ = 0;
    PSX_BLOCK_EDGE(&cpu, 0x80012340u);
    VERIFY(s_edge_calls == reads_edge_calls,
           "schedule-stable MMIO does not force a common slow block edge");
    psx_devices_mmio_sync_at(0x1F801040u, 0);
    VERIFY(psx_next_service_cycle == 0,
           "side-effecting SIO reads retain conservative invalidation");
    VERIFY(s_sio_cycles == 61 && s_device_calls == calls_before,
           "SIO observation uses its own elapsed interval");
    /* A DMA write must also materialize CD. Test it after the unchanged
     * SIO-only interval assertion, rather than changing that assertion. */
    psx_devices_mmio_sync_at(0x1F8010A8u, 1);
    VERIFY(psx_next_service_cycle == 0,
           "DMA register writes still invalidate the pre-handler schedule");
    psx_devices_mmio_sync_at(0x1F801800u, 1);
    s_cd_ready = 0; s_cd_cycles_remaining = 7; s_dma_ready_cycles = 0;
    psx_advance_cycles(7); psx_devices_mmio_sync_at(0x1F801814u, 0);
    VERIFY(s_cd_ready && s_dma_ready_cycles == 1 && psx_cycle_count == 68,
           "unrelated MMIO still processes real CD-to-DMA event with one boundary cycle");
    psx_devices_service_to_now();
    VERIFY(s_sio_cycles == 68 && fixture_timer_cycles() == 68 && s_vblank_phase == 68,
           "same-position snapshot barrier materializes every pending device interval");
    g_event_step_conservative = 1; calls_before = s_device_calls;
    psx_advance_cycles(3);
    VERIFY(psx_cycle_count == 71 && s_device_calls == calls_before + 3 &&
           s_sio_cycles == 71 && fixture_timer_cycles() == 71 && s_vblank_phase == 71,
           "runtime conservative fallback retains single-cycle servicing");
    g_event_step_conservative = 0; calls_before = s_device_calls;
    psx_advance_cycles(9); psx_devices_mmio_sync_at(0x1F801814u, 0);
    VERIFY(psx_cycle_count == 80 && s_device_calls == calls_before,
           "return from conservative stepping re-anchors independent clocks");
    psx_cycle_count = 20; psx_cycles_resync_after_restore(NULL);
    fixture_timer_reset();
    psx_advance_cycles(4); psx_devices_mmio_sync_at(0x1F801100u, 0);
    VERIFY(fixture_timer_cycles() == 4 && psx_cycle_count == 24,
           "restore never replays pre-restore elapsed device time");
#if defined(PSX_CLOCK_REAL_TIMERS) && PSX_CLOCK_REAL_TIMERS
    /* Compare production counter arithmetic/IRQ state at a real observation
     * against the same clock's single-cycle conservative path. */
    uint16_t counts[2][3], targets[2][3];
    uint32_t modes[2][3], fractions[2][3], status[2];
    int32_t lines[2][3];
    uint64_t raises[2][3];
    extern uint64_t g_timer_irq_fired[3];
    for (unsigned pass = 0; pass < 2; ++pass) {
        g_event_step_conservative = (int)pass;
        i_mask = i_stat = 0; s_vblank_phase = 0; s_cd_ready = 1;
        psx_cycles_reset_for_boot(); timers_init();
        memset(g_timer_irq_fired, 0, sizeof(uint64_t) * 3);
        timers_write(TIMER_BASE + 0x28, 10);
        timers_write(TIMER_BASE + 0x24, 0x58); /* repeat, target reset + dwell */
        timers_write(TIMER_BASE + 0x08, 19);
        timers_write(TIMER_BASE + 0x04, 0x158); /* divided dotclock */
        for (unsigned tick = 0; tick < 1000; ++tick) {
            psx_advance_cycles(1);
            if (tick % 17 == 0) psx_devices_mmio_sync_at(0x1F801814u, 0);
        }
        psx_devices_service_to_now();
        timers_get_snapshot(counts[pass], modes[pass], targets[pass], lines[pass], fractions[pass]);
        status[pass] = i_stat;
        memcpy(raises[pass], g_timer_irq_fired, sizeof(raises[pass]));
    }
    VERIFY(!memcmp(counts[0], counts[1], sizeof(counts[0])) &&
           !memcmp(targets[0], targets[1], sizeof(targets[0])) &&
           !memcmp(modes[0], modes[1], sizeof(modes[0])) &&
           !memcmp(lines[0], lines[1], sizeof(lines[0])) &&
           !memcmp(fractions[0], fractions[1], sizeof(fractions[0])) &&
           !memcmp(raises[0], raises[1], sizeof(raises[0])) && status[0] == status[1],
           "production root counter/IRQ state matches conservative timeline across unrelated MMIO");
    g_event_step_conservative = 0;
    fprintf(stderr, "PASS production timer snapshot/IRQ differential against conservative stepping\n");
#endif
    fprintf(stderr, "PASS addressed quiet-device elimination, CD/DMA dependency, full barrier, conservative switch and restore\n");
#endif
    fprintf(stderr, "PASS cross-device deadline preserves D-1 + 1 causality; "
                    "idle skip bounded by the SPU sample deadline\n");
#if defined(PSX_TEST_BLOCK_MATERIALIZE) && PSX_TEST_BLOCK_MATERIALIZE
    if(materialized_edge_products()) return 1;
#endif
#if defined(PSX_CLOCK_REAL_GPU) && PSX_CLOCK_REAL_GPU
    if(gpustat_clock_product()) return 1;
#endif
    return 0;
}
