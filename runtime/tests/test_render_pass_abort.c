/* Render-pass watchdog abort (render_pass.c psx_mod_render_pass).
 *
 * A pass that runs away is cut off by a longjmp from the frozen clock's
 * watchdog, deep inside nested guest frames. That longjmp skips every exit
 * those frames would have run: generated functions' bb_defer cleanup,
 * overlay_loader_call_native's unit depth, the DMA kick's exec depth, the
 * interpreter's active/phase/precise flags and pending load, the native-shard
 * stack and its cycle flush hook. The live game must resume exactly as if
 * the pass never happened, so this test aborts a pass from such frames and
 * checks each of those values, the clock and RAM afterwards. The GPU, VRAM,
 * device snapshots and presenter are mocks; render_pass.c and psx_cycles.c
 * are the real ones. Build/run: ctest -R render_pass_abort_test */

#include "render_pass.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "mod_plugins.h"
#include "mod_runtime.h"
#include "psx_cyc.h"

static ModFunctionEntryContext s_mod_entry;
void mod_runtime_function_entry_context_save(ModFunctionEntryContext *out) {
    *out = s_mod_entry;
}
void mod_runtime_function_entry_context_restore(const ModFunctionEntryContext *in) {
    s_mod_entry = *in;
}
static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

/* ---- runtime globals the two units share with the rest of the runtime -- */
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

/* ---- mock devices and memory ------------------------------------------ */
static uint8_t s_ram[2u * 1024u * 1024u];
static uint8_t s_spad[1024];
static uint8_t s_dma_regs[64];
static uint32_t s_csv = 1234;
static uint16_t s_tc[3] = {1, 2, 3}, s_tt[3] = {4, 5, 6};
static uint32_t s_tm[3] = {7, 8, 9}, s_tf[3];
static int32_t  s_ti[3];

uint8_t *memory_get_ram_ptr(void) { return s_ram; }
uint32_t memory_get_ram_bytes(void) { return (uint32_t)sizeof s_ram; }
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
void psx_irq_refresh_cause_ip2(void) {}
void timers_get_snapshot(uint16_t c[3], uint32_t m[3], uint16_t t[3],
                         int32_t irq[3], uint32_t f[3]) {
    memcpy(c, s_tc, sizeof s_tc); memcpy(m, s_tm, sizeof s_tm);
    memcpy(t, s_tt, sizeof s_tt); memcpy(irq, s_ti, sizeof s_ti);
    memcpy(f, s_tf, sizeof s_tf);
}
void timers_set_snapshot(const uint16_t c[3], const uint32_t m[3],
                         const uint16_t t[3], const int32_t irq[3],
                         const uint32_t f[3]) {
    memcpy(s_tc, c, sizeof s_tc); memcpy(s_tm, m, sizeof s_tm);
    memcpy(s_tt, t, sizeof s_tt); memcpy(s_ti, irq, sizeof s_ti);
    memcpy(s_tf, f, sizeof s_tf);
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

/* psx_cycles.c device side: nothing may advance inside a pass. */
static uint64_t s_dev_cycles;
void sio_advance(uint32_t c) { s_dev_cycles += c; }
void cdrom_advance(uint32_t c) { s_dev_cycles += c; }
void dma_advance(uint32_t c) { s_dev_cycles += c; }
void timers_advance(uint32_t c) { s_dev_cycles += c; }
void interrupts_advance_cycles(uint32_t c) { s_dev_cycles += c; }
void interrupts_service_scheduled_events(void) {}
uint32_t interrupts_cycles_to_vblank(void) { return 564480u; }
uint32_t timers_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t sio_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return UINT32_MAX; }
void psx_spu_sample_event_service(void) {}
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}

/* The interpreter's deferred load and the native-shard stack live in those
 * modules' statics; model them here the same way. */
static DirtyRamLoadDelay s_ld;
void dirty_ram_ld_delay_discard(void) { memset(&s_ld, 0, sizeof s_ld); }
void dirty_ram_ld_delay_save(DirtyRamLoadDelay *o) { *o = s_ld; }
void dirty_ram_ld_delay_restore(const DirtyRamLoadDelay *i) { s_ld = *i; }
static int s_ov_active_depth;
static uint32_t s_ov_inprogress;
void overlay_loader_native_nesting(int *d, uint32_t *ip) {
    *d = s_ov_active_depth; *ip = s_ov_inprogress;
}
void overlay_loader_set_native_nesting(int d, uint32_t ip) {
    s_ov_active_depth = d; s_ov_inprogress = ip;
}

/* GPU and presenter. */
uint64_t gpu_pass_state_hash(void) { return 42; }
int gpu_pass_checkpoint_save(void) { return 1; }
void gpu_pass_checkpoint_restore(void) {}
static uint64_t s_ticks;
uint64_t gl_renderer_perf_ticks(void) { return s_ticks += 1000; }
uint64_t gl_renderer_perf_frequency(void) { return 1000000000u; }
uint32_t gl_renderer_pass_unavailable(void) { return 0; }
int psx_presentation_fast_forward(void) { return 0; }
uint32_t gl_renderer_pass_plan(uint32_t p, uint32_t s, uint32_t *a,
                               uint32_t max, uint32_t *wanted) {
    (void)p; (void)s; (void)a; (void)max;
    if (wanted) *wanted = 0;
    return 0;
}
static int s_open_passes, s_kept;
static uint32_t s_stereo_mask;
static uint64_t s_stereo_published;
void gl_renderer_pass_begin_diag(GLRenderPassBeginDiag *out) {
    memset(out, 0, sizeof *out);
}
int gl_renderer_pass_begin(int x, int y, int w, int h, int open_gen,
                           uint32_t period, int reuse_backup) {
    (void)x; (void)y; (void)w; (void)h; (void)open_gen; (void)period;
    (void)reuse_backup;
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
uint32_t g_psx_vblank_cycles = 564480u;
uint32_t gl_renderer_stereo_unavailable(void) { return 0; }
int gl_renderer_stereo_begin(int x, int y, int w, int h, int reuse) {
    return gl_renderer_pass_begin(x, y, w, h, 0, 0, reuse);
}
int gl_renderer_stereo_end(uint32_t eye, int keep) {
    gl_renderer_pass_end(0, keep);
    if (keep) s_stereo_mask |= 1u << eye;
    return 1;
}
void gl_renderer_stereo_stage_reset(void) { s_stereo_mask = 0; }
void gl_renderer_stereo_reset(void) { s_stereo_mask = 0; s_stereo_published = 0; }
int gl_renderer_stereo_set_presentation(uint32_t mode) { return mode <= 1; }
static int32_t s_view[3];
void gte_render_view_get(int32_t xyz[3]) { memcpy(xyz, s_view, sizeof s_view); }
void gte_render_view_set(const int32_t xyz[3]) { memcpy(s_view, xyz, sizeof s_view); }
static PSXModRenderView s_pose;
void gte_render_pose_get(PSXModRenderView *v) { *v = s_pose; memcpy(v->translation, s_view, sizeof s_view); }
void gte_render_pose_set(const PSXModRenderView *v) { s_pose = *v; memcpy(s_view, v->translation, sizeof s_view); }
int gl_renderer_stereo_publish(uint64_t id, uint64_t cycle, const int32_t view[2][3]) {
    (void)cycle; (void)view;
    if (s_stereo_mask != 3u) return 0;
    s_stereo_published = id;
    return 1;
}

/* ---- an overlay shard's cycle shim (overlay_dispatch_preamble.c.inc) --- */
static uint32_t s_shard_pending;
static void shard_flush(void) {
    uint32_t c = s_shard_pending;
    s_shard_pending = 0;
    if (c) psx_advance_cycles(c);
}
static void live_flush(void) {}

/* ---- guest frames ------------------------------------------------------ */
typedef struct Frames {
    int depth;        /* nested frames to open */
    int run_away;     /* innermost frame spins until the watchdog fires */
} Frames;

/* One generated function that calls through a native overlay unit, a DMA
 * kick and the interpreter, each with the runtime's own entry/exit protocol,
 * then recurses. */
static void guest_frame(CPUState *cpu, const Frames *f, int level) {
    __attribute__((cleanup(psx_cyc_bb_defer_cleanup))) int guard = 1;
    int prev_unit = g_call_unit_depth, prev_active = g_dirty_interp_active;
    int prev_phase = g_exec_phase, prev_precise = g_precise_mode;
    int prev_ov_depth = s_ov_active_depth;
    uint32_t prev_ov_ip = s_ov_inprogress;
    void (*prev_flush)(void) = g_overlay_flush_pending_cycles;
    ModFunctionEntryContext prev_mod = s_mod_entry;
    s_mod_entry.depth++;
    s_mod_entry.plugin = f;
    (void)guard;
    psx_cyc_bb_defer_begin();
    g_call_unit_depth = prev_unit + 1;          /* overlay_loader_call_native */
    s_ov_active_depth = prev_ov_depth + 1;      /* overlay_loader_dispatch */
    s_ov_inprogress = 0x80100000u + (uint32_t)level;
    g_overlay_flush_pending_cycles = shard_flush;
    g_exec_phase = 2;
    g_dma_exec_depth++;                          /* dma.c kick */
    g_host_store_depth++;                        /* psx_host_write_* */
    g_dma_cur_ch = 2;
    g_dirty_interp_active = 1;                   /* dirty_ram_dispatch */
    g_precise_mode = 1;                          /* psx_run_precise */
    s_ld.armed = 1; s_ld.rt = 8; s_ld.val = 0xDEAD0000u + (uint32_t)level;
    s_shard_pending = 777;                       /* shard-side charges */
    s_ram[0x1000 + level] = 0xAA;                /* the pass draws */
    cpu->gpr[8] = 0x1234u + (uint32_t)level;

    if (level + 1 < f->depth) {
        guest_frame(cpu, f, level + 1);
    } else if (f->run_away) {
        /* A wait loop that never ends in frozen time: charges per
         * instruction, publishes at every branch edge. */
        for (uint32_t i = 0; i < 100000000u; i++) {
            psx_cyc_charge(4);
            psx_cyc_bb_defer_flush();
        }
    }

    s_mod_entry = prev_mod;
    s_ld.armed = 0;                              /* retired on interpreter exit */
    g_precise_mode = prev_precise;
    g_dirty_interp_active = prev_active;
    g_dma_exec_depth--;
    g_host_store_depth--;
    g_dma_cur_ch = -1;
    g_exec_phase = prev_phase;
    shard_flush();
    g_overlay_flush_pending_cycles = prev_flush;
    s_ov_inprogress = prev_ov_ip;
    s_ov_active_depth = prev_ov_depth;
    g_call_unit_depth = prev_unit;
}

static int pass_fn(struct CPUState *cpu, void *user, uint32_t alpha_q16) {
    (void)alpha_q16;
    guest_frame(cpu, (const Frames *)user, 0);
    return 1;
}

static int leaky_pass_fn(struct CPUState *cpu, void *user, uint32_t alpha_q16) {
    (void)cpu; (void)user; (void)alpha_q16;
    g_call_unit_depth++;                         /* forgets its exit */
    return 1;
}

static int mod_leaky_pass_fn(struct CPUState *cpu, void *user, uint32_t alpha_q16) {
    (void)cpu; (void)user; (void)alpha_q16;
    s_mod_entry.depth++;
    s_mod_entry.plugin = user;
    return 1;
}

static int stereo_draw(struct CPUState *cpu, void *user, uint32_t eye) {
    Frames frames = {5, user && eye == PSX_MOD_EYE_RIGHT};
    CHECK(psx_mod_render_view_offset(eye ? -24 : 24, 0, 0), "view setter inside eye");
    PSXModRenderView v = {0}; v.struct_size = sizeof v;
    v.rotation_q12[2] = 4096; v.rotation_q12[4] = 4096; v.rotation_q12[6] = -4096;
    v.projection = 1; v.fx_q16 = 200 << 16; v.fy_q16 = 100 << 16;
    v.translation[0] = eye ? -24 : 24;
    CHECK(psx_mod_render_view(&v), "full scoped pose accepted");

    guest_frame(cpu, &frames, 0);
    return 1;
}

typedef struct Live {
    ModFunctionEntryContext mod_entry;
    int bb_defer, unit, dma, host, dma_ch, active, phase, precise, dispatch;
    int ov_depth;
    uint32_t ov_ip, batch, resume;
    void (*flush)(void);
    DirtyRamLoadDelay ld;
    uint64_t cycles, next_service, dev_cycles;
} Live;

static void snap(Live *l) {
    memset(l, 0, sizeof *l);
    l->mod_entry = s_mod_entry;
    l->bb_defer = g_psx_cyc_bb_defer;
    l->unit = g_call_unit_depth;
    l->dma = g_dma_exec_depth;
    l->host = g_host_store_depth;
    l->dma_ch = g_dma_cur_ch;
    l->active = g_dirty_interp_active;
    l->phase = g_exec_phase;
    l->precise = g_precise_mode;
    l->dispatch = g_psx_dispatch_depth;
    l->ov_depth = s_ov_active_depth;
    l->ov_ip = s_ov_inprogress;
    l->batch = g_psx_cyc_batch;
    l->resume = g_dirty_safe_resume_pc;
    l->flush = g_overlay_flush_pending_cycles;
    l->ld = s_ld;
    l->cycles = psx_cycle_count;
    l->next_service = psx_next_service_cycle;
    l->dev_cycles = s_dev_cycles;
}

static void check_live(const Live *a, const char *when) {
    Live b;
    char m[160];
    snap(&b);
#define SAME(field, what) do { \
        snprintf(m, sizeof m, "%s: %s restored", when, what); \
        CHECK(a->field == b.field, m); } while (0)
    SAME(mod_entry.depth, "mod callback depth");
    SAME(mod_entry.plugin, "mod callback owner");
    SAME(bb_defer, "g_psx_cyc_bb_defer");
    SAME(unit, "g_call_unit_depth");
    SAME(dma, "g_dma_exec_depth");
    SAME(host, "g_host_store_depth");
    SAME(dma_ch, "g_dma_cur_ch");
    SAME(active, "g_dirty_interp_active");
    SAME(phase, "g_exec_phase");
    SAME(precise, "g_precise_mode");
    SAME(dispatch, "g_psx_dispatch_depth");
    SAME(ov_depth, "native-shard stack depth");
    SAME(ov_ip, "native in-progress entry");
    SAME(batch, "pending cycle batch");
    SAME(resume, "interpreter resume latch");
    SAME(flush, "shard cycle-flush hook");
    SAME(ld.armed, "pending load (armed)");
    SAME(ld.val, "pending load (value)");
    SAME(cycles, "guest clock");
    SAME(next_service, "service deadline");
    SAME(dev_cycles, "device time (nothing advanced)");
#undef SAME
}

int main(void) {
    CPUState cpu;
    PSXModRenderPass pass;
    RenderPassStats st;
    Frames runaway = {5, 1}, clean = {5, 0};
    Live live;
    uint8_t ram_before[64];

#ifdef _WIN32
    _putenv_s("PSX_RENDER_PASS_VERIFY", "1");
#else
    setenv("PSX_RENDER_PASS_VERIFY", "1", 1);
#endif
    memset(&cpu, 0, sizeof cpu);
    cpu.gpr[8] = 0x55u;
    psx_cycles_resync_after_restore(NULL);
    psx_advance_cycles(50000);

    /* The live context the pass interrupts: inside two generated functions
     * (the game's VSync entry hook), a native shard's flush hook installed,
     * the interpreter's resume latch and a pending load of its own. */
    s_mod_entry.depth = 1;
    s_mod_entry.plugin = &cpu;
    g_psx_cyc_bb_defer = 2;
    g_psx_cyc_batch = 9;
    g_psx_dispatch_depth = 3;
    g_exec_phase = 3;
    g_dirty_safe_resume_pc = 0x80012344u;
    g_overlay_flush_pending_cycles = live_flush;
    s_ld.armed = 1; s_ld.rt = 3; s_ld.val = 0x11112222u; s_ld.age = 1;
    s_ov_active_depth = 1;
    s_ov_inprogress = 0x80110000u;
    memcpy(ram_before, s_ram + 0x1000, sizeof ram_before);

    memset(&pass, 0, sizeof pass);
    pass.struct_size = sizeof pass;
    pass.w = 320; pass.h = 240;
    pass.alpha_q16 = 32768u;

    /* 1. A pass that runs away inside five nested frames. */
    snap(&live);
    CHECK(psx_mod_render_pass(&cpu, &pass, pass_fn, &runaway) == 0,
          "a runaway pass is rolled back, not presented");
    render_pass_get_stats(&st);
    CHECK(st.watchdog == 1 && st.aborted == 1 && st.passes == 0,
          "the watchdog cut it off once");
    CHECK(st.nesting_repairs == 1, "the landing found and undid skipped exits");
    CHECK(strstr(st.last_abort_detail, "mod entries +5") &&
          strstr(st.last_abort_detail, "mod owner"), "TCP abort detail includes skipped mod exits");
    CHECK(!st.disabled, "one fault does not disable passes");
    CHECK(s_open_passes == 0 && s_kept == 0, "the presenter closed the pass, no image");
    CHECK(!g_psx_render_pass_active, "time is live again");
    check_live(&live, "after a watchdog abort");
    CHECK(s_shard_pending == 0,
          "the shard's unpublished pass cycles went into the frozen clock");
    CHECK(memcmp(ram_before, s_ram + 0x1000, sizeof ram_before) == 0,
          "RAM the pass wrote is restored");
    CHECK(cpu.gpr[8] == 0x55u, "CPU registers are restored");
    CHECK(st.verify_mismatch == 0,
          "PSX_RENDER_PASS_VERIFY: machine state identical after the abort");

    /* 2. The same frames, returning normally, present an image. */
    snap(&live);
    CHECK(psx_mod_render_pass(&cpu, &pass, pass_fn, &clean) == 1,
          "a pass that completes is presented");
    render_pass_get_stats(&st);
    CHECK(st.passes == 1 && s_kept == 1, "one image kept");
    CHECK(st.nesting_repairs == 1, "a normal return needs no repair");
    check_live(&live, "after a normal pass");
    CHECK(st.verify_mismatch == 0, "a balanced pass verifies clean");

    /* One complete pair, then a right eye that aborts five guest frames deep.
     * The left staging eye never replaces the previous published pair. */
    {
        PSXModStereoFrame frame = {sizeof frame, 2, 0, 0, 320, 240};
        RenderStereoStats stereo;
        uint64_t kept;
        snap(&live);
        CHECK(psx_mod_render_stereo(&cpu, &frame, stereo_draw, NULL) == 1,
              "complete stereo pair publishes");
        kept = s_stereo_published;
        CHECK(psx_mod_render_stereo(&cpu, &frame, stereo_draw, &runaway) == 0,
              "right-eye watchdog discards the entire pair");
        render_stereo_get_stats(&stereo);
        render_pass_get_stats(&st);
        CHECK(s_stereo_published == kept && s_stereo_mask == 0,
              "right-eye abort retains previous pair and clears staging");
        CHECK(stereo.last_failed_eye == 1 && stereo.retained_pair_id == kept,
              "failure producer records retained pair and right eye");
        CHECK(s_view[0] == 0 && s_view[1] == 0 && s_view[2] == 0,
              "watchdog restores render-view ambient");
        CHECK(!s_pose.struct_size && !s_pose.projection, "watchdog restores rotation/projection");
        CHECK(st.verify_mismatch == 0 && !g_psx_render_pass_active,
              "aborted eye restores machine state and time");
        check_live(&live, "after right-eye watchdog");
        CHECK(psx_mod_render_stereo(&cpu, &frame, stereo_draw, NULL) == 1,
              "a later pair recovers after the watchdog");
    }

    /* 3. Verify mode flags a pass that leaves the nesting unbalanced (the
     * restore would otherwise hide it), and still restores it. */
    snap(&live);
    (void)psx_mod_render_pass(&cpu, &pass, leaky_pass_fn, NULL);
    render_pass_get_stats(&st);
    CHECK(st.verify_mismatch == 1, "an unbalanced pass is reported in verify mode");
    check_live(&live, "after an unbalanced pass");

    snap(&live);
    (void)psx_mod_render_pass(&cpu, &pass, mod_leaky_pass_fn, NULL);
    render_pass_get_stats(&st);
    CHECK(st.verify_mismatch == 2, "mod-only imbalance is reported");
    check_live(&live, "after a mod-only unbalanced pass");

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
