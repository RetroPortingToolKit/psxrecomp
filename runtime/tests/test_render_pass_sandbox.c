/* Render-pass sandbox: what a pass may write and what it gets back.
 *
 * 1. Store policy (render_pass_plan.c render_pass_store_to, the function
 *    memory.c routes every store made inside a pass through): RAM and
 *    scratchpad are written directly; SPU, CD, timer, other-DMA, GP1 and
 *    everything else outside the allow-list is dropped under its class and
 *    never reaches the device; GP0, GP1 DMA mode, I_STAT/I_MASK, GPU/OTC DMA
 *    and DPCR/DICR reach it.
 * 2. A real pass (render_pass.c, psx_cycles.c and the real timers.c) that
 *    runs 10^6 guest cycles, makes those stores, acknowledges and masks
 *    interrupts: the drops are counted per class, no device advances, no
 *    timer IRQ is raised, and RAM, scratchpad, I_STAT/I_MASK, timers and the
 *    clock are exactly as before. Status gates refuse the pass without
 *    running it. The same with 8 MiB RAM live (the opt-in 8 MB RAM map):
 *    stores reach unique RAM up to 0x7FFFFF and all 8 MiB are restored; a
 *    retail pass after it folds mirrors again.
 * 3. VRAM journal (render_pass_vram_policy / _journal_add / _rollback, the
 *    CPU side of gpu_gl_renderer.c's out-of-rect journal): writes inside the
 *    pass rect are allowed, writes outside are backed up first and rolled
 *    back exactly, covered writes are not journaled twice, a full journal or
 *    a renderer mode that cannot journal refuses.
 *
 * The GPU, presenter and remaining devices are mocks. Build/run:
 * ctest -R render_pass_sandbox_test */

#include "render_pass.h"
#include "render_pass_plan.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "mod_plugins.h"
#include "psx_cycles.h"
#include "timers.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

/* ---- runtime globals the units share with the rest of the runtime ----- */
int      g_ls_replay_active = 0;
int      g_ls_mode = 0;
int      g_precise_mode = 0;
int      g_psx_call_bail = 0;
int      g_psx_dispatch_depth = 0;
uint32_t i_stat = 0, i_mask = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;
uint32_t g_debug_last_store_pc = 0;
uint32_t g_debug_current_func_addr = 0;
uint32_t g_psx_icache_tv[1024];
uint64_t g_render_pass_dropped_writes[RENDER_PASS_DROP_CLASSES];
void   (*g_overlay_flush_pending_cycles)(void) = NULL;
int      g_call_unit_depth = 0;
int      g_dma_exec_depth = 0;
int      g_host_store_depth = 0;
int      g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr = 0, g_dma_cur_bcr = 0, g_dma_initiator_pc = 0;
int      g_dirty_interp_active = 0;
int      g_exec_phase = 0;
uint32_t g_dirty_safe_resume_pc = 0;

/* ---- memory and the MMIO devices a pass may reach ----------------------- */
/* Backing store sized for the largest geometry, as memory.c's; the live size
 * (what memory_get_ram_bytes() answers and memory.c passes the store policy)
 * is 2 MiB retail or 8 MiB with the 8 MB RAM mod. */
#define RAM_2MB (2u * 1024u * 1024u)
#define RAM_8MB (8u * 1024u * 1024u)
static uint8_t s_ram[RAM_8MB];
static uint32_t s_ram_live = RAM_2MB;
static uint8_t s_spad[1024];
static uint8_t s_dma_regs[64];
static uint32_t s_csv = 1234;
static int s_mmio_calls, s_gp0_words, s_gp1_calls, s_irq_refresh;
static uint32_t s_last_mmio;

static void mock_mmio_write(uint32_t phys, uint32_t val, uint32_t width) {
    (void)width;
    s_mmio_calls++;
    s_last_mmio = phys;
    if (phys == 0x1F801070u) i_stat &= val;          /* acknowledge */
    else if (phys == 0x1F801074u) i_mask = val;
    else if (phys == 0x1F801810u) s_gp0_words++;
    else if (phys == 0x1F801814u) s_gp1_calls++;
    else if (phys >= 0x1F801100u && phys <= 0x1F80112Fu)
        timers_write(phys, val);                      /* would move a timer */
}

static RenderPassStoreTarget target(void) {
    RenderPassStoreTarget t;
    memset(&t, 0, sizeof t);
    t.ram = s_ram;
    t.ram_size = s_ram_live;
    t.scratchpad = s_spad;
    t.scratchpad_size = sizeof s_spad;
    t.mmio_write = mock_mmio_write;
    return t;
}

/* What memory.c psx_write_word/half/byte do while a pass is active. */
static void guest_store(uint32_t addr, uint32_t val, uint32_t width) {
    RenderPassStoreTarget t = target();
    int cls;
    if (!g_psx_render_pass_active) {
        fprintf(stderr, "FAIL: guest_store outside a pass\n");
        failures++;
        return;
    }
    cls = render_pass_store_to(&t, addr, val, width);
    if (cls >= 0) g_render_pass_dropped_writes[cls]++;
}

uint8_t *memory_get_ram_ptr(void) { return s_ram; }
uint32_t memory_get_ram_bytes(void) { return s_ram_live; }
uint8_t *memory_get_scratchpad_ptr(void) { return s_spad; }
uint32_t dma_snapshot_bytes(void) { return sizeof s_dma_regs; }
void dma_snapshot_write(uint8_t *p) { memcpy(p, s_dma_regs, sizeof s_dma_regs); }
int dma_snapshot_read(const uint8_t *p, uint32_t len) {
    if (len != sizeof s_dma_regs) return 0;
    memcpy(s_dma_regs, p, len);
    return 1;
}
int dma_gpu_linked_list_active(void) { return 0; }
uint32_t interrupts_get_cycles_since_vblank(void) { return s_csv; }
void interrupts_set_cycles_since_vblank(uint32_t v) { s_csv = v; }
void psx_irq_refresh_cause_ip2(void) { s_irq_refresh++; }
static int s_irqs_raised;
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)detail;
    s_irqs_raised++;
    i_stat |= 1u << bit;
}
void event_ring_record_aux(int ev, uint8_t src, uint32_t aux) {
    (void)ev; (void)src; (void)aux;
}
uint32_t cdrom_snapshot_bytes(void) { return 0; }
void cdrom_snapshot_write(uint8_t *p) { (void)p; }
uint32_t spu_snapshot_bytes(void) { return 0; }
void spu_snapshot_write(uint8_t *p) { (void)p; }
uint32_t sio_snapshot_bytes(void) { return 0; }
void sio_snapshot_write(uint8_t *p) { (void)p; }
uint32_t mdec_snapshot_bytes(void) { return 0; }
void mdec_snapshot_write(uint8_t *p) { (void)p; }

int  psx_get_in_exception(void) { return 0; }
int  psx_netplay_active(void) { return 0; }
int  psx_netplay_is_resimulating(void) { return 0; }
int  psx_selfcheck_resim_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
int  psx_rewind_is_open(void) { return 0; }
static int s_fast_forward;
int  psx_presentation_fast_forward(void) { return s_fast_forward; }

/* psx_cycles.c device side (timers are the real ones). */
static uint64_t s_dev_cycles;
void sio_advance(uint32_t c) { s_dev_cycles += c; }
void cdrom_advance(uint32_t c) { s_dev_cycles += c; }
void dma_advance(uint32_t c) { s_dev_cycles += c; }
void interrupts_advance_cycles(uint32_t c) { s_dev_cycles += c; }
void interrupts_service_scheduled_events(void) {}
uint32_t interrupts_cycles_to_vblank(void) { return 564480u; }
uint32_t cdrom_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t sio_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return UINT32_MAX; }
void psx_spu_sample_event_service(void) {}
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}

static DirtyRamLoadDelay s_ld;
void dirty_ram_ld_delay_discard(void) { memset(&s_ld, 0, sizeof s_ld); }
void dirty_ram_ld_delay_save(DirtyRamLoadDelay *o) { *o = s_ld; }
void dirty_ram_ld_delay_restore(const DirtyRamLoadDelay *i) { s_ld = *i; }
void overlay_loader_native_nesting(int *d, uint32_t *ip) { *d = 0; *ip = 0; }
void overlay_loader_set_native_nesting(int d, uint32_t ip) { (void)d; (void)ip; }

/* GPU and presenter. */
uint64_t gpu_pass_state_hash(void) { return 42; }
static int s_checkpoint_ok = 1;
int gpu_pass_checkpoint_save(void) { return s_checkpoint_ok; }
void gpu_pass_checkpoint_restore(void) {}
static uint64_t s_ticks;
uint64_t gl_renderer_perf_ticks(void) { return s_ticks += 1000; }
uint64_t gl_renderer_perf_frequency(void) { return 1000000000u; }
static uint32_t s_gl_status = PSX_MOD_RENDER_PASS_READY;
uint32_t gl_renderer_pass_unavailable(void) { return s_gl_status; }
uint32_t gl_renderer_pass_plan(uint32_t p, uint32_t s, uint32_t *a,
                               uint32_t max, uint32_t *wanted) {
    (void)p; (void)s; (void)a; (void)max;
    if (wanted) *wanted = 0;
    return 0;
}
static int s_open_passes, s_kept, s_begin_ok = 1;
void gl_renderer_pass_begin_diag(GLRenderPassBeginDiag *out) {
    memset(out, 0, sizeof *out);
    out->reason = "capture_size";
    out->requested_w = 320; out->requested_h = 240;
    out->capture_w = 512; out->capture_h = 240;
}
int gl_renderer_pass_begin(int x, int y, int w, int h, int open_gen,
                           uint32_t period, int reuse_backup) {
    (void)x; (void)y; (void)w; (void)h; (void)open_gen; (void)period;
    (void)reuse_backup;
    if (!s_begin_ok) return 0;
    s_open_passes++;
    return 1;
}
void gl_renderer_pass_end(uint32_t alpha_q16, int keep) {
    (void)alpha_q16;
    s_open_passes--;
    if (keep) s_kept++;
}
uint32_t gl_renderer_pass_leaks(void) { return 0; }
int gl_renderer_pass_verify_vram(void) { return 1; }
void gl_renderer_pass_note_cost(uint64_t t) { (void)t; }
void gl_renderer_pass_service_presents(void) {}

/* ---- 1. store policy --------------------------------------------------- */
static void test_store_policy(void) {
    RenderPassStoreTarget t = target();
    int calls;
    memset(s_ram, 0, sizeof s_ram);
    memset(s_spad, 0, sizeof s_spad);
    s_mmio_calls = 0;

    CHECK(render_pass_store_to(&t, 0x80001000u, 0x11223344u, 4) == -1 &&
          s_ram[0x1000] == 0x44 && s_ram[0x1003] == 0x11, "KSEG0 RAM word written");
    CHECK(render_pass_store_to(&t, 0xA0001004u, 0xBEEFu, 2) == -1 &&
          s_ram[0x1004] == 0xEF && s_ram[0x1005] == 0xBE, "KSEG1 RAM half written");
    CHECK(render_pass_store_to(&t, 0x00201008u, 0x5Au, 1) == -1 &&
          s_ram[0x1008] == 0x5A, "RAM mirror folds onto main RAM");
    CHECK(render_pass_store_to(&t, 0x1F800010u, 0xCAFEF00Du, 4) == -1 &&
          s_spad[0x10] == 0x0D && s_spad[0x13] == 0xCA, "scratchpad written");
    CHECK(s_mmio_calls == 0, "memory stores never touch MMIO");

    /* Dropped, per class, and never delivered. */
    CHECK(render_pass_store_to(&t, 0x1F801D88u, 0xFFFFu, 2) == RENDER_PASS_DROP_SPU,
          "SPU key-on dropped (SPU)");
    CHECK(render_pass_store_to(&t, 0x1F801C00u, 0x3FFFu, 2) == RENDER_PASS_DROP_SPU,
          "SPU voice volume dropped (SPU)");
    CHECK(render_pass_store_to(&t, 0x1F801801u, 0x1Bu, 1) == RENDER_PASS_DROP_CD,
          "CD command dropped (CD)");
    CHECK(render_pass_store_to(&t, 0x1F801104u, 0x58u, 4) == RENDER_PASS_DROP_TIMER,
          "timer mode dropped (TIMER)");
    CHECK(render_pass_store_to(&t, 0x1F801128u, 100u, 2) == RENDER_PASS_DROP_TIMER,
          "timer target dropped (TIMER)");
    CHECK(render_pass_store_to(&t, 0x1F8010C0u, 0x80100000u, 4) == RENDER_PASS_DROP_DMA,
          "SPU DMA MADR dropped (DMA)");
    CHECK(render_pass_store_to(&t, 0x1F8010B8u, 0x11000000u, 4) == RENDER_PASS_DROP_DMA,
          "CD DMA CHCR dropped (DMA)");
    CHECK(render_pass_store_to(&t, 0x1F801814u, 0x00000000u, 4) == RENDER_PASS_DROP_GPU,
          "GP1 reset dropped (GPU)");
    CHECK(render_pass_store_to(&t, 0x1F801810u, 0xE1000000u, 2) == RENDER_PASS_DROP_GPU,
          "a half-word GP0 store is dropped (GPU)");
    CHECK(render_pass_store_to(&t, 0x1F801040u, 0x11u, 1) == RENDER_PASS_DROP_OTHER,
          "SIO dropped (OTHER)");
    CHECK(render_pass_store_to(&t, 0x1F801000u, 0x1F000000u, 4) == RENDER_PASS_DROP_OTHER,
          "memory control dropped (OTHER)");
    CHECK(render_pass_store_to(&t, 0x1F801820u, 0u, 4) == RENDER_PASS_DROP_OTHER,
          "MDEC dropped (OTHER)");
    CHECK(render_pass_store_to(&t, 0x1F000000u, 1u, 1) == RENDER_PASS_DROP_OTHER,
          "expansion 1 dropped (OTHER)");
    CHECK(render_pass_store_to(&t, 0xFFFE0130u, 0x804u, 4) == RENDER_PASS_DROP_OTHER,
          "KSEG2 cache control dropped (OTHER)");
    CHECK(s_mmio_calls == 0, "no dropped store reached a device");

    /* Allowed: all restored after the pass. */
    calls = s_mmio_calls;
    CHECK(render_pass_store_to(&t, 0x1F801810u, 0x28FFFFFFu, 4) == -1, "GP0 word allowed");
    CHECK(render_pass_store_to(&t, 0x1F801814u, 0x04000002u, 4) == -1, "GP1 DMA mode allowed");
    CHECK(render_pass_store_to(&t, 0x1F801814u, 0x10000007u, 4) == -1, "GP1 info allowed");
    CHECK(render_pass_store_to(&t, 0x1F801070u, 0xFFFFFFFEu, 4) == -1, "I_STAT ack allowed");
    CHECK(render_pass_store_to(&t, 0x1F801074u, 0x0000000Du, 4) == -1, "I_MASK allowed");
    CHECK(render_pass_store_to(&t, 0x1F8010A8u, 0x01000401u, 4) == -1, "GPU DMA allowed");
    CHECK(render_pass_store_to(&t, 0x1F8010E8u, 0x11000002u, 4) == -1, "OTC DMA allowed");
    CHECK(render_pass_store_to(&t, 0x1F8010F0u, 0x0F6F4321u, 4) == -1, "DPCR allowed");
    CHECK(s_mmio_calls == calls + 8, "allowed stores reached their device");

    /* Isolated cache: nothing but the I-cache would change. */
    t.isolate_cache = 1;
    s_ram[0x2000] = 0x77;
    CHECK(render_pass_store_to(&t, 0x80002000u, 0u, 4) == -1 && s_ram[0x2000] == 0x77,
          "IsC store absorbed, RAM untouched");
}

/* ---- 2. a real pass ------------------------------------------------------ */
static int pass_fn(struct CPUState *cpu, void *user, uint32_t alpha_q16) {
    (void)user; (void)alpha_q16;
    cpu->gpr[8] = 0xDEADu;
    guest_store(0x80010000u, 0x12345678u, 4);        /* RAM */
    guest_store(0x1F800020u, 0x99u, 1);              /* scratchpad */
    guest_store(0x1F801D88u, 0x00FFu, 2);            /* SPU key-on */
    guest_store(0x1F801DAAu, 0xC000u, 2);            /* SPU control */
    guest_store(0x1F801801u, 0x0Au, 1);              /* CD: Init */
    guest_store(0x1F801124u, 0x0000u, 4);            /* timer 2 mode */
    guest_store(0x1F8010C8u, 0x01000201u, 4);        /* SPU DMA CHCR */
    guest_store(0x1F801814u, 0x03000001u, 4);        /* GP1 display off */
    guest_store(0x1F801044u, 0x0001u, 2);            /* SIO */
    guest_store(0x1F801810u, 0x02000000u, 4);        /* GP0 fill: allowed */
    guest_store(0x1F801070u, 0u, 4);                 /* ack every IRQ */
    guest_store(0x1F801074u, 0u, 4);                 /* mask every IRQ */
    for (int i = 0; i < 1000; i++) psx_advance_cycles(1000u);   /* 10^6 */
    /* A device-side change the store policy cannot see (as if some runtime
     * path wrote a timer directly): the checkpoint must still undo it. */
    timers_write(0x1F801100u, 0x1234u);
    return 1;
}

static int never_fn(struct CPUState *cpu, void *user, uint32_t alpha_q16) {
    (void)cpu; (void)user; (void)alpha_q16;
    failures++;
    fprintf(stderr, "FAIL: a refused pass ran its callback\n");
    return 1;
}

static void test_pass(void) {
    CPUState cpu;
    PSXModRenderPass pass;
    RenderPassStats st;
    uint16_t tc0[3], tt0[3], tc1[3], tt1[3];
    uint32_t tm0[3], tf0[3], tm1[3], tf1[3];
    int32_t ti0[3], ti1[3];
    uint8_t ram0[64], spad0[64];
    uint64_t cyc0, dev0, next0;
    uint32_t stat0, mask0;
    int irqs0;

    memset(&cpu, 0, sizeof cpu);
    cpu.gpr[8] = 0x55u;
    memset(s_ram, 0xA5, sizeof s_ram);
    memset(s_spad, 0x3C, sizeof s_spad);
    psx_cycles_resync_after_restore(NULL);

    /* Live machine: timer 2 counts the system clock and interrupts on its
     * target every 1000 cycles; timer 0 runs free. */
    timers_init();
    timers_write(0x1F801128u, 1000u);
    timers_write(0x1F801124u, 0x0058u);
    i_mask = 0x0040u;                                 /* timer 2 unmasked */
    i_stat = 0x0001u;                                 /* a pending VBlank */
    psx_advance_cycles(2500u);
    CHECK(s_irqs_raised > 0, "sanity: timer 2 interrupts while time is live");

    memset(&pass, 0, sizeof pass);
    pass.struct_size = sizeof pass;
    pass.w = 320; pass.h = 240;
    pass.alpha_q16 = 32768u;

    timers_get_snapshot(tc0, tm0, tt0, ti0, tf0);
    memcpy(ram0, s_ram + 0x10000, sizeof ram0);
    memcpy(spad0, s_spad, sizeof spad0);
    cyc0 = psx_cycle_count;
    next0 = psx_next_service_cycle;
    dev0 = s_dev_cycles;
    stat0 = i_stat;
    mask0 = i_mask;
    irqs0 = s_irqs_raised;
    s_mmio_calls = 0;

    CHECK(psx_mod_render_pass(&cpu, &pass, pass_fn, NULL) == 1, "the pass ran and was kept");
    render_pass_get_stats(&st);
    CHECK(st.passes == 1 && s_kept == 1 && s_open_passes == 0, "one image, pass closed");
    CHECK(st.dropped[RENDER_PASS_DROP_SPU] == 2, "2 SPU stores dropped and counted");
    CHECK(st.dropped[RENDER_PASS_DROP_CD] == 1, "1 CD store dropped and counted");
    CHECK(st.dropped[RENDER_PASS_DROP_TIMER] == 1, "1 timer store dropped and counted");
    CHECK(st.dropped[RENDER_PASS_DROP_DMA] == 1, "1 other-DMA store dropped and counted");
    CHECK(st.dropped[RENDER_PASS_DROP_GPU] == 1, "1 GP1 store dropped and counted");
    CHECK(st.dropped[RENDER_PASS_DROP_OTHER] == 1, "1 SIO store dropped and counted");
    CHECK(s_mmio_calls == 3, "only GP0, I_STAT and I_MASK reached a device");
    CHECK(s_irqs_raised == irqs0, "no interrupt raised in 10^6 frozen cycles");
    CHECK(s_dev_cycles == dev0, "no device advanced");
    CHECK(psx_cycle_count == cyc0 && psx_next_service_cycle == next0,
          "guest clock and service deadline restored");
    CHECK(i_stat == stat0 && i_mask == mask0, "I_STAT / I_MASK restored");
    CHECK(s_irq_refresh > 0, "CAUSE IP2 recomputed after the restore");
    timers_get_snapshot(tc1, tm1, tt1, ti1, tf1);
    CHECK(memcmp(tc0, tc1, sizeof tc0) == 0 && memcmp(tm0, tm1, sizeof tm0) == 0 &&
          memcmp(tt0, tt1, sizeof tt0) == 0 && memcmp(ti0, ti1, sizeof ti0) == 0 &&
          memcmp(tf0, tf1, sizeof tf0) == 0, "timers exactly as before");
    CHECK(memcmp(ram0, s_ram + 0x10000, sizeof ram0) == 0, "RAM restored");
    CHECK(memcmp(spad0, s_spad, sizeof spad0) == 0, "scratchpad restored");
    CHECK(cpu.gpr[8] == 0x55u, "CPU registers restored");
    CHECK(!g_psx_render_pass_active, "time is live again");

    /* Time is live again: the same cycles now move the timer and interrupt. */
    psx_advance_cycles(2500u);
    CHECK(s_irqs_raised > irqs0, "timer 2 interrupts again after the pass");

    /* Status gates refuse without running the callback. */
    s_fast_forward = 1;
    CHECK(psx_mod_render_pass_status() == PSX_MOD_RENDER_PASS_FAST_FORWARD,
          "fast-forward status");
    CHECK(psx_mod_render_pass(&cpu, &pass, never_fn, NULL) == 0,
          "no pass during fast-forward");
    s_fast_forward = 0;
    s_gl_status = PSX_MOD_RENDER_PASS_BACKEND;
    CHECK(psx_mod_render_pass_status() == PSX_MOD_RENDER_PASS_BACKEND,
          "backend status");
    CHECK(psx_mod_render_pass(&cpu, &pass, never_fn, NULL) == 0,
          "no pass while the backend declines");
    s_gl_status = PSX_MOD_RENDER_PASS_READY;
    CHECK(psx_mod_render_pass_status() == PSX_MOD_RENDER_PASS_READY, "ready again");

    render_pass_get_stats(&st);
    CHECK(st.status_refused == 2 && st.refused == 0 &&
          st.last_failure.status == PSX_MOD_RENDER_PASS_BACKEND,
          "pass refusal counters are separate from plan refusals");
    s_begin_ok = 0;
    CHECK(psx_mod_render_pass(&cpu, &pass, never_fn, NULL) == 0, "begin refusal");
    render_pass_get_stats(&st);
    CHECK(st.begin_refused == 1 && st.checkpoint_refused == 0 &&
          strcmp(st.last_failure.reason, "capture_size") == 0 &&
          st.last_failure.gl.requested_w == 320 && st.last_failure.gl.capture_w == 512,
          "begin failure retains producer dimensions");
    uint64_t failed_attempt = st.last_failure.attempt;
    s_begin_ok = 1;
    s_checkpoint_ok = 0;
    CHECK(psx_mod_render_pass(&cpu, &pass, never_fn, NULL) == 0, "checkpoint refusal");
    render_pass_get_stats(&st);
    CHECK(st.checkpoint_refused == 1 && s_open_passes == 0 &&
          strcmp(st.last_failure.reason, "checkpoint_gpu") == 0 &&
          st.last_failure.attempt > failed_attempt,
          "checkpoint failure closes transaction without calling guest code");
    failed_attempt = st.last_failure.attempt;
    s_checkpoint_ok = 1;
    CHECK(psx_mod_render_pass(&cpu, &pass, pass_fn, NULL) == 1, "success after refusals");
    render_pass_get_stats(&st);
    CHECK(st.last_failure.attempt == failed_attempt && st.pass_attempts == 6,
          "success does not erase last failure");
    render_pass_reset_session();
    render_pass_get_stats(&st);
    CHECK(!st.last_failure.reason && st.pass_attempts == 0,
          "session reset clears failure diagnostics");
}

/* ---- 2b. 8 MiB main RAM live (psx.enhancement.8mb-ram) ------------------ */
static int s_retail_fold_seen;
static int pass8_fn(struct CPUState *cpu, void *user, uint32_t alpha_q16) {
    (void)user; (void)alpha_q16;
    cpu->gpr[9] = 0xBEEFu;
    guest_store(0x80600000u, 0xCAFEBABEu, 4);        /* unique above 2 MiB */
    guest_store(0xA07FFFFCu, 0x01020304u, 4);        /* last word of the map */
    guest_store(0x80200010u, 0x55u, 1);              /* no retail fold */
    guest_store(0x80000100u, 0x66u, 1);              /* low RAM as well */
    return 1;
}

static int pass_retail_fn(struct CPUState *cpu, void *user, uint32_t alpha_q16) {
    const uint8_t *before = (const uint8_t *)user;
    (void)cpu; (void)alpha_q16;
    guest_store(0x80600010u, 0x77u, 1);              /* retail: folds onto 0x10 */
    s_retail_fold_seen = s_ram[0x10] == 0x77 && s_ram[0x600010] == before[0x600010];
    return 1;
}

static void test_ram_8mb(void) {
    RenderPassStoreTarget t;
    CPUState cpu;
    PSXModRenderPass pass;
    RenderPassStats st0, st1;
    uint8_t *ram0 = (uint8_t *)malloc(RAM_8MB);
    if (!ram0) { CHECK(0, "8 MiB test buffer"); return; }

    /* Store policy with the expanded map: the window no longer mirrors. */
    s_ram_live = RAM_8MB;
    t = target();
    memset(s_ram, 0, sizeof s_ram);
    CHECK(render_pass_store_to(&t, 0x80600000u, 0x11223344u, 4) == -1 &&
          s_ram[0x600000] == 0x44 && s_ram[0x600003] == 0x11 && s_ram[0] == 0,
          "8 MiB: 0x80600000 is its own RAM, not a mirror of 0");
    CHECK(render_pass_store_to(&t, 0x00201008u, 0x5Au, 1) == -1 &&
          s_ram[0x201008] == 0x5A && s_ram[0x1008] == 0,
          "8 MiB: 0x00201008 is not folded onto 0x1008");
    CHECK(render_pass_store_to(&t, 0xA07FFFFEu, 0xBEEFu, 2) == -1 &&
          s_ram[0x7FFFFE] == 0xEF && s_ram[0x7FFFFF] == 0xBE,
          "8 MiB: the top of the window is RAM");
    CHECK(render_pass_store_to(&t, 0x80800000u, 1u, 1) == RENDER_PASS_DROP_OTHER,
          "8 MiB: past the decode window is not RAM");

    /* A real pass: all 8 MiB come back, the upper 6 MiB included. */
    for (uint32_t i = 0; i < RAM_8MB; i++)
        s_ram[i] = (uint8_t)((i * 2654435761u) >> 24);
    memcpy(ram0, s_ram, RAM_8MB);
    memset(&cpu, 0, sizeof cpu);
    memset(&pass, 0, sizeof pass);
    pass.struct_size = sizeof pass;
    pass.w = 320; pass.h = 240;
    pass.alpha_q16 = 16384u;
    render_pass_get_stats(&st0);
    CHECK(psx_mod_render_pass(&cpu, &pass, pass8_fn, NULL) == 1,
          "8 MiB: the pass ran and was kept");
    render_pass_get_stats(&st1);
    CHECK(st1.passes == st0.passes + 1, "8 MiB: one more pass");
    CHECK(memcmp(ram0, s_ram, RAM_8MB) == 0,
          "8 MiB: all of RAM restored, above 2 MiB too");
    CHECK(cpu.gpr[9] == 0u, "8 MiB: CPU registers restored");

    /* The next session without the mod (retail again): stores fold into the
     * low 2 MiB and the restore covers them; the backing bytes above the
     * live size are neither written nor needed. */
    s_ram_live = RAM_2MB;
    s_retail_fold_seen = 0;
    CHECK(psx_mod_render_pass(&cpu, &pass, pass_retail_fn, ram0) == 1,
          "retail after 8 MiB: the pass ran");
    CHECK(s_retail_fold_seen, "retail after 8 MiB: 0x80600010 folded onto 0x10");
    CHECK(memcmp(ram0, s_ram, RAM_8MB) == 0,
          "retail after 8 MiB: RAM restored");
    free(ram0);
}

/* ---- 3. VRAM journal ----------------------------------------------------- */
#define VW 1024
#define VH 512
static uint16_t s_vram[VW * VH], s_vram0[VW * VH];

static void paint(int x, int y, int w, int h, uint16_t v) {
    for (int r = y; r < y + h; r++)
        for (int c = x; c < x + w; c++) s_vram[r * VW + c] = v;
}

static void test_journal(void) {
    RenderPassJournal j;
    int x, y, w, h, n;
    memset(&j, 0, sizeof j);
    for (int i = 0; i < VW * VH; i++) s_vram[i] = (uint16_t)(i * 2654435761u >> 16);
    memcpy(s_vram0, s_vram, sizeof s_vram);

    /* The pass draws buffer 1 at (0, 240) 320x240. */
#define POLICY(X, Y, W, H, CAN) \
    (x = (X), y = (Y), w = (W), h = (H), \
     render_pass_vram_policy(&j, 0, 240, 320, 240, VW, VH, (CAN), &x, &y, &w, &h))
    CHECK(POLICY(10, 250, 100, 100, 1) == RENDER_PASS_VRAM_ALLOW, "inside the rect: allowed");
    paint(10, 250, 100, 100, 0x1111);
    CHECK(POLICY(0, 0, 0, 5, 1) == RENDER_PASS_VRAM_ALLOW, "empty write: allowed");

    CHECK(POLICY(640, 0, 64, 32, 1) == RENDER_PASS_VRAM_JOURNAL, "outside: journal");
    CHECK(render_pass_journal_add(&j, s_vram, VW, x, y, w, h) == 0, "journaled as entry 0");
    paint(640, 0, 64, 32, 0x2222);
    CHECK(POLICY(650, 4, 16, 16, 1) == RENDER_PASS_VRAM_ALLOW, "covered: not journaled twice");
    paint(650, 4, 16, 16, 0x3333);

    CHECK(POLICY(300, 200, 40, 60, 1) == RENDER_PASS_VRAM_JOURNAL, "straddling the rect: journal");
    CHECK(render_pass_journal_add(&j, s_vram, VW, x, y, w, h) == 1, "entry 1");
    paint(300, 200, 40, 60, 0x4444);

    /* Overlapping entry 0 partly: rolled back newest first. */
    CHECK(POLICY(600, 16, 64, 32, 1) == RENDER_PASS_VRAM_JOURNAL, "overlapping: journal");
    CHECK(render_pass_journal_add(&j, s_vram, VW, x, y, w, h) == 2, "entry 2");
    paint(600, 16, 64, 32, 0x5555);

    /* Clipped to VRAM. */
    CHECK(POLICY(-8, -8, 24, 24, 1) == RENDER_PASS_VRAM_JOURNAL && x == 0 && y == 0 &&
          w == 16 && h == 16, "negative origin clipped");
    CHECK(render_pass_journal_add(&j, s_vram, VW, x, y, w, h) == 3, "entry 3");
    paint(x, y, w, h, 0x6666);
    CHECK(POLICY(1016, 508, 32, 32, 1) == RENDER_PASS_VRAM_JOURNAL && w == 8 && h == 4,
          "far edge clipped");
    CHECK(render_pass_journal_add(&j, s_vram, VW, x, y, w, h) == 4, "entry 4");
    paint(x, y, w, h, 0x7777);

    CHECK(POLICY(800, 300, 8, 8, 0) == RENDER_PASS_VRAM_REFUSE,
          "a mode that cannot journal refuses");

    n = j.n;
    while (j.n < RENDER_PASS_JOURNAL_MAX) {
        int k = j.n;
        CHECK(POLICY(400 + 8 * k, 100, 4, 4, 1) == RENDER_PASS_VRAM_JOURNAL, "fill journal");
        render_pass_journal_add(&j, s_vram, VW, x, y, w, h);
        paint(x, y, w, h, (uint16_t)(0x8000 + k));
    }
    CHECK(j.n == RENDER_PASS_JOURNAL_MAX && n == 5, "journal full");
    CHECK(POLICY(900, 400, 4, 4, 1) == RENDER_PASS_VRAM_REFUSE, "full journal refuses");
    CHECK(render_pass_journal_add(&j, s_vram, VW, 900, 400, 4, 4) == -1,
          "add refuses past the limit");

    /* End of pass: roll back, then the pass rect itself is restored by the
     * renderer's own backup; outside it VRAM must equal the original. */
    render_pass_journal_rollback(&j, s_vram, VW);
    CHECK(j.n == 0, "journal emptied");
    paint(0, 240, 320, 240, 0);
    {
        int bad = 0;
        uint16_t *a = s_vram, *b = s_vram0;
        for (int r = 0; r < VH; r++)
            for (int c = 0; c < VW; c++) {
                if (r >= 240 && r < 480 && c < 320) continue;
                if (a[r * VW + c] != b[r * VW + c]) bad++;
            }
        CHECK(bad == 0, "every out-of-rect write rolled back exactly");
    }
    render_pass_journal_free(&j);
#undef POLICY
}

int main(void) {
    test_store_policy();
    test_pass();
    test_ram_8mb();
    test_journal();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
