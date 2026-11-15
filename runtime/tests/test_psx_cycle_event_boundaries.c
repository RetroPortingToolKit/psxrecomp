/*
 * Pin cross-device causality at a scheduler deadline.
 *
 * Build/run: ctest -R psx_cycle_event_boundaries_test
 */

#include "psx_cycles.h"
#include "psx_cycle_freeze.h"
#include "cpu_state.h"

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

#if defined(PSX_TEST_OVERLAY_CHARGE) && PSX_TEST_OVERLAY_CHARGE
#define TEST_OVERLAY_CPU_CHARGE(cycles) overlay_cpu_charge(cycles)
#else
#define TEST_OVERLAY_CPU_CHARGE(cycles) psx_advance_cycles(cycles)
#endif

void sio_advance(uint32_t cycles) { (void)cycles; }

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

void timers_advance(uint32_t cycles) { (void)cycles; }
void interrupts_advance_cycles(uint32_t cycles) { s_vblank_phase += cycles; }
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) { return 564480u - s_vblank_phase % 564480u; }
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) {
    return s_cd_ready || !(mask & (1u << 2)) ? UINT32_MAX : s_cd_cycles_remaining;
}
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
    psx_devices_observe();
}
extern uint32_t psx_cycle_sample_period;
extern void (*psx_cycle_sample_hook)(uint64_t, uint32_t);
static unsigned s_samples;
static void sample(uint64_t cycle, uint32_t periods) { (void)cycle; s_samples += periods; }
#define VERIFY(c, text) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", text); return 1; } } while (0)
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
    fprintf(stderr, "PASS cross-device deadline preserves D-1 + 1 causality; "
                    "idle skip bounded by the SPU sample deadline\n");
    return 0;
}
