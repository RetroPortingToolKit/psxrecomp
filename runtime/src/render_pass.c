/* render_pass.c - host-timed render passes (psx_mod_render_pass_plan /
 * psx_mod_render_pass, mod_plugins.h). See docs/RENDER_PASSES.md.
 *
 * A pass is a sandboxed nested guest call. The guest-visible machine is
 * checkpointed, time is frozen (psx_cycle_freeze.h), the plugin draws an image of
 * the scene through the game's own code, the OpenGL backend captures the
 * declared display rect, and then everything is put back: CPU (with the
 * GTE), RAM, scratchpad, I-cache tags, I_STAT/I_MASK, timers, DMA and GPU
 * registers, and the VRAM rect. Stores that could escape that restore (SPU,
 * CD, timers, other DMA channels, ...) never reach their device in the first
 * place (memory.c render_pass_store).
 *
 * Nothing here runs unless a trusted plugin calls the API, and the plan
 * refuses in netplay, rollback, rewind, fast-forward, self-check
 * resimulation, or without the OpenGL presenter's flip-aware interpolation
 * (psx_mod_render_pass_status says which). */

#include "render_pass.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "dma.h"
#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "mod_plugins.h"
#include "mod_runtime.h"
#include "overlay_loader.h"
#include "psx_icache.h"
#include "timers.h"
#include "psx_video_timing.h"
#include "gte_view.h"

extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
/* Live main-RAM geometry (psx_ram_geometry.c): 2 MiB retail, or 8 MiB with
 * the opt-in 8 MB RAM mod. Latched by memory_init() once per boot, so it
 * cannot change inside a pass, but it can between sessions. */
extern uint32_t memory_get_ram_bytes(void);
extern uint32_t i_stat;
extern uint32_t i_mask;
extern uint32_t dma_snapshot_bytes(void);
extern void     dma_snapshot_write(uint8_t *p);
extern int      dma_snapshot_read(const uint8_t *p, uint32_t len);
extern uint32_t interrupts_get_cycles_since_vblank(void);
extern void     interrupts_set_cycles_since_vblank(uint32_t v);
extern void     psx_irq_refresh_cause_ip2(void);
extern int      psx_get_in_exception(void);
extern int      psx_netplay_active(void);
extern int      psx_netplay_is_resimulating(void);
extern int      psx_selfcheck_resim_active(void);
extern int      psx_rewind_is_open(void);
extern int      psx_presentation_fast_forward(void);
extern uint64_t g_guest_store_count;
extern uint64_t g_mmio_access_count;
extern uint32_t g_debug_last_store_pc;
extern uint32_t g_debug_current_func_addr;
extern int      g_psx_dispatch_depth;
extern int      g_psx_call_bail;
extern int      g_ls_mode;
extern int      g_ls_replay_active;
extern void   (*g_overlay_flush_pending_cycles)(void);
/* Host nesting that runtime frames set on entry and put back on exit. */
extern int      g_call_unit_depth;        /* overlay_loader_call_native */
extern int      g_dma_exec_depth;         /* dma.c kick / async writes */
extern int      g_host_store_depth;       /* memory.c psx_host_write_* */
extern int      g_dma_cur_ch;
extern uint32_t g_dma_cur_madr;
extern uint32_t g_dma_cur_bcr;
extern uint32_t g_dma_initiator_pc;
extern int      g_dirty_interp_active;    /* dirty-RAM interpreter */
extern int      g_exec_phase;
extern int      g_precise_mode;
extern uint32_t g_dirty_safe_resume_pc;
extern uint32_t cdrom_snapshot_bytes(void);
extern void     cdrom_snapshot_write(uint8_t *p);
extern uint32_t spu_snapshot_bytes(void);
extern void     spu_snapshot_write(uint8_t *p);
extern uint32_t sio_snapshot_bytes(void);
extern void     sio_snapshot_write(uint8_t *p);
extern uint32_t mdec_snapshot_bytes(void);
extern void     mdec_snapshot_write(uint8_t *p);

#define RP_SPAD_SIZE  1024u
/* ~8 M guest cycles is about four PS1 frames of CPU time: far beyond any
 * frame's draw code, short enough that a pass stuck on a wait loop costs the
 * host well under a frame before it is rolled back. */
#define RP_WATCHDOG_CYCLES 8000000u
/* Faults (watchdog, VRAM leaks) before passes stay off for the session. */
#define RP_FAULT_LIMIT 8u

/* Host-side nesting state. Generated functions, native overlay calls, DMA
 * kicks, the dirty-RAM interpreter and GP0 processing each set some of this
 * on entry and put it back on exit. The watchdog abort longjmps past those
 * exits, so the landing puts the interrupted code's values back from the
 * checkpoint, as the scheduler and exception landings reset theirs
 * (traps.c psx_scheduler_run, interrupts.c psx_check_interrupts). The cycle
 * deferral depth (g_psx_cyc_bb_defer) is part of PsxCycleFreeze. After a
 * pass that returns normally all of it is already balanced. */
typedef struct RenderPassNesting {
    int      call_unit_depth;
    int      dma_exec_depth, dma_cur_ch, host_store_depth;
    uint32_t dma_cur_madr, dma_cur_bcr, dma_initiator_pc;
    int      dirty_interp_active, exec_phase, precise_mode;
    uint32_t dirty_safe_resume_pc;
    DirtyRamLoadDelay ld_delay;
    int      ov_active_depth;
    uint32_t ov_inprogress;
    void   (*ov_flush)(void);
    ModFunctionEntryContext mod_entry;
    PSXModRenderView render_pose;
} RenderPassNesting;

static void nesting_save(RenderPassNesting *n) {
    n->call_unit_depth = g_call_unit_depth;
    n->dma_exec_depth = g_dma_exec_depth;
    n->host_store_depth = g_host_store_depth;
    n->dma_cur_ch = g_dma_cur_ch;
    n->dma_cur_madr = g_dma_cur_madr;
    n->dma_cur_bcr = g_dma_cur_bcr;
    n->dma_initiator_pc = g_dma_initiator_pc;
    n->dirty_interp_active = g_dirty_interp_active;
    n->exec_phase = g_exec_phase;
    n->precise_mode = g_precise_mode;
    n->dirty_safe_resume_pc = g_dirty_safe_resume_pc;
    dirty_ram_ld_delay_save(&n->ld_delay);
    overlay_loader_native_nesting(&n->ov_active_depth, &n->ov_inprogress);
    n->ov_flush = g_overlay_flush_pending_cycles;
    mod_runtime_function_entry_context_save(&n->mod_entry);
    gte_render_pose_get(&n->render_pose);
}

static void nesting_restore(const RenderPassNesting *n) {
    g_call_unit_depth = n->call_unit_depth;
    g_dma_exec_depth = n->dma_exec_depth;
    g_host_store_depth = n->host_store_depth;
    g_dma_cur_ch = n->dma_cur_ch;
    g_dma_cur_madr = n->dma_cur_madr;
    g_dma_cur_bcr = n->dma_cur_bcr;
    g_dma_initiator_pc = n->dma_initiator_pc;
    g_dirty_interp_active = n->dirty_interp_active;
    g_exec_phase = n->exec_phase;
    g_precise_mode = n->precise_mode;
    g_dirty_safe_resume_pc = n->dirty_safe_resume_pc;
    dirty_ram_ld_delay_restore(&n->ld_delay);
    overlay_loader_set_native_nesting(n->ov_active_depth, n->ov_inprogress);
    g_overlay_flush_pending_cycles = n->ov_flush;
    mod_runtime_function_entry_context_restore(&n->mod_entry);
    gte_render_pose_set(&n->render_pose);
}

/* PSX_RENDER_PASS_VERIFY: a pass that returned normally must leave the
 * nesting balanced; the restore would otherwise hide the imbalance. (The
 * deferred load is not nesting: the interpreter retires it on exit.) */
static int nesting_balanced(const RenderPassNesting *n) {
    RenderPassNesting now;
    memset(&now, 0, sizeof now);
    nesting_save(&now);
    return now.call_unit_depth == n->call_unit_depth &&
           now.dma_exec_depth == n->dma_exec_depth &&
           now.host_store_depth == n->host_store_depth &&
           now.dirty_interp_active == n->dirty_interp_active &&
           now.exec_phase == n->exec_phase &&
           now.precise_mode == n->precise_mode &&
           now.ov_active_depth == n->ov_active_depth &&
           now.ov_flush == n->ov_flush &&
           now.mod_entry.depth == n->mod_entry.depth &&
           now.mod_entry.plugin == n->mod_entry.plugin;
}

/* After a watchdog abort, before the restore: which exits the longjmp
 * skipped (for the fault log). Returns how many values it found changed. */
static int nesting_describe(const RenderPassNesting *n, int bb_defer_ck,
                            char *out, size_t cap) {
    RenderPassNesting now;
    int count = 0;
    size_t len = 0;
    memset(&now, 0, sizeof now);
    nesting_save(&now);
    out[0] = '\0';
#define RP_NOTE(cond, ...) do { if (cond) { \
        int w_ = snprintf(out + len, cap - len, "%s", count ? ", " : ""); \
        if (w_ > 0 && (size_t)w_ < cap - len) len += (size_t)w_; \
        w_ = snprintf(out + len, cap - len, __VA_ARGS__); \
        if (w_ > 0 && (size_t)w_ < cap - len) len += (size_t)w_; \
        count++; } } while (0)
    RP_NOTE(g_psx_cyc_bb_defer != bb_defer_ck, "cycle deferral %+d",
            g_psx_cyc_bb_defer - bb_defer_ck);
    RP_NOTE(now.call_unit_depth != n->call_unit_depth, "call unit %+d",
            now.call_unit_depth - n->call_unit_depth);
    RP_NOTE(now.ov_active_depth != n->ov_active_depth, "shard stack %+d",
            now.ov_active_depth - n->ov_active_depth);
    RP_NOTE(now.ov_flush != n->ov_flush, "shard cycle hook");
    RP_NOTE(now.dma_exec_depth != n->dma_exec_depth, "DMA depth %+d",
            now.dma_exec_depth - n->dma_exec_depth);
    RP_NOTE(now.host_store_depth != n->host_store_depth, "host store depth %+d",
            now.host_store_depth - n->host_store_depth);
    RP_NOTE(now.dirty_interp_active != n->dirty_interp_active, "interpreter flag");
    RP_NOTE(now.precise_mode != n->precise_mode, "precise flag");
    RP_NOTE(now.exec_phase != n->exec_phase, "exec phase %d->%d",
            now.exec_phase, n->exec_phase);
    RP_NOTE(now.ld_delay.armed != n->ld_delay.armed, "pending load");
    RP_NOTE(now.mod_entry.depth != n->mod_entry.depth, "mod entries %+d",
            (int)now.mod_entry.depth - (int)n->mod_entry.depth);
    RP_NOTE(now.mod_entry.plugin != n->mod_entry.plugin, "mod owner");
#undef RP_NOTE
    return count;
}

typedef struct RenderPassCheckpoint {
    CPUState cpu;
    uint8_t  spad[RP_SPAD_SIZE];
    uint32_t icache[1024];
    uint32_t i_stat, i_mask, csv;
    uint16_t t_counter[3], t_target[3];
    uint32_t t_mode[3], t_frac[3];
    int32_t  t_irq[3];
    uint64_t store_count, mmio_count;
    uint32_t last_store_pc, current_func;
    int      dispatch_depth, call_bail;
    uint32_t dma_len;
    uint32_t ram_bytes;               /* live RAM geometry at the checkpoint */
    RenderPassNesting nest;
} RenderPassCheckpoint;

static uint8_t *s_ram_copy;          /* s_ram_copy_cap bytes */
static uint32_t s_ram_copy_cap;
static uint8_t *s_dma_copy;
static RenderPassCheckpoint s_ck;
static PsxCycleFreeze s_freeze;
static jmp_buf s_abort_jmp;
static volatile int s_abort_armed;
static int s_nesting;

static RenderPassStats s_stats;
static int s_verify = -1;
static int s_open_generation;        /* next pass captures frame N's image */
static uint32_t s_plan_period = 2;
/* The last pass's restore left the machine as it found it; while no guest
 * code runs (clock and store count unchanged) and the plan is the same, the
 * next pass may reuse that pass's VRAM backup. */
static uint64_t s_plan_serial, s_restored_plan;
static uint64_t s_restored_cycle, s_restored_stores;
static int s_restored_valid;
static RenderPassFailure s_attempt;
static const char *s_checkpoint_failure;
static RenderStereoStats s_stereo;
static int s_pair_busy;
static uint32_t s_pair_retry;

static int pass_refuse(const char *reason, uint32_t status) {
    s_attempt.reason = reason;
    s_attempt.status = status;
    s_stats.last_failure = s_attempt;
    return 0;
}

/* PSX_RENDER_PASS_WATCHDOG=<guest cycles> lowers the cut-off (debug), so a
 * title's rollback path can be exercised on real passes. */
static uint64_t watchdog_cycles(void) {
    static uint64_t v;
    if (!v) {
        const char *e = getenv("PSX_RENDER_PASS_WATCHDOG");
        long long n = e ? atoll(e) : 0;
        v = (n >= 1000 && n < (long long)RP_WATCHDOG_CYCLES)
            ? (uint64_t)n : (uint64_t)RP_WATCHDOG_CYCLES;
        if (v != RP_WATCHDOG_CYCLES)
            fprintf(stderr, "psxrecomp: render pass watchdog lowered to %llu "
                    "guest cycles (PSX_RENDER_PASS_WATCHDOG)\n",
                    (unsigned long long)v);
    }
    return v;
}

static int verify_on(void) {
    if (s_verify < 0) {
        const char *e = getenv("PSX_RENDER_PASS_VERIFY");
        s_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_verify;
}

void render_pass_get_stats(RenderPassStats *out) {
    if (!out) return;
    *out = s_stats;
    for (int i = 0; i < RENDER_PASS_DROP_CLASSES; i++)
        out->dropped[i] = g_render_pass_dropped_writes[i];
}

void render_pass_reset_session(void) {
    memset(&s_stats, 0, sizeof s_stats);
    memset(g_render_pass_dropped_writes, 0, sizeof g_render_pass_dropped_writes);
    s_open_generation = 0;
    s_restored_valid = 0;
    memset(&s_attempt, 0, sizeof s_attempt);
    memset(&s_stereo, 0, sizeof s_stereo);
    s_stereo.last_eye = -1;
    s_pair_busy = 0;
    s_pair_retry = 0;
    gl_renderer_stereo_reset();
}

/* Everything that must hold before guest code may run frozen, as a
 * PSX_MOD_RENDER_PASS_* reason (READY = all of it holds). */
static uint32_t transaction_status(int stereo) {
    uint32_t gl;
    if (s_stats.disabled) return PSX_MOD_RENDER_PASS_DISABLED;
    if (psx_netplay_active() || psx_netplay_is_resimulating() ||
        psx_selfcheck_resim_active() || psx_rewind_is_open() ||
        g_ls_mode || g_ls_replay_active)
        return PSX_MOD_RENDER_PASS_SESSION;
    if (psx_presentation_fast_forward()) return PSX_MOD_RENDER_PASS_FAST_FORWARD;
    gl = stereo ? gl_renderer_stereo_unavailable() : gl_renderer_pass_unavailable();
    if (gl != PSX_MOD_RENDER_PASS_READY) return gl;
    if (s_nesting || g_psx_render_pass_active || psx_get_in_exception() ||
        dma_gpu_linked_list_active())
        return PSX_MOD_RENDER_PASS_BUSY;
    return PSX_MOD_RENDER_PASS_READY;
}

static uint32_t pass_status(void) { return transaction_status(0); }
uint32_t psx_mod_render_stereo_status(void) {
    return s_pair_busy ? PSX_MOD_RENDER_PASS_BUSY : transaction_status(1);
}
void render_stereo_get_stats(RenderStereoStats *out) { if (out) *out = s_stereo; }
int psx_mod_set_stereo_presentation(uint32_t mode) {
    return mode <= 1u ? gl_renderer_stereo_set_presentation(mode) : 0;
}
int psx_mod_render_view_offset(int32_t x, int32_t y, int32_t z) {
    int32_t xyz[3] = {x, y, z};
    if (!g_psx_render_pass_active) return 0;
    gte_render_view_set(xyz);
    return 1;
}


int psx_mod_render_view(const PSXModRenderView *view) {
    if (!g_psx_render_pass_active || !view || view->struct_size < sizeof *view ||
        view->projection > 1u || view->projection_h_ref > 65535u) return 0;
    /* Bound products well inside int64. Rigid matrices are supplied by caller. */
    for (int i = 0; i < 9; ++i)
        if (view->rotation_q12[i] < -4096 || view->rotation_q12[i] > 4096) return 0;
    for (int i = 0; i < 3; ++i)
        if (view->translation[i] < -65536 || view->translation[i] > 65536) return 0;
    if (view->projection && (view->fx_q16 <= 0 || view->fy_q16 <= 0)) return 0;
    gte_render_pose_set(view);
    return 1;
}

static int passes_allowed(void) {
    return pass_status() == PSX_MOD_RENDER_PASS_READY;
}

uint32_t psx_mod_render_pass_status(void) {
    return pass_status();
}

uint32_t psx_mod_render_pass_plan(uint32_t period_vblanks,
                                  uint32_t shown_after_vblanks,
                                  uint32_t *alpha_q16, uint32_t max) {
    uint32_t wanted = 0, n;
    s_open_generation = 0;
    if (!alpha_q16 || max == 0 || period_vblanks == 0 || period_vblanks > 8 ||
        shown_after_vblanks > 8)
        return 0;
    if (!passes_allowed()) return 0;
    n = gl_renderer_pass_plan(period_vblanks, shown_after_vblanks,
                              alpha_q16, max, &wanted);
    s_stats.wanted += wanted;
    if (!n) {
        if (wanted) s_stats.refused++;
        return 0;
    }
    s_stats.plans++;
    s_stats.planned += n;
    s_plan_serial++;
    s_open_generation = 1;
    s_plan_period = period_vblanks;
    return n;
}

/* FNV-1a step over 64-bit words, then the tail bytes. Each step is a
 * bijection of h, so a change confined to one word always changes the hash.
 * Word steps take an eighth of the time of byte steps, which keeps
 * PSX_RENDER_PASS_VERIFY affordable with 8 MiB of RAM live. */
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        uint64_t w;
        memcpy(&w, b + i, sizeof w);
        h = (h ^ w) * 1099511628211ULL;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}

/* Guest-visible state hash for PSX_RENDER_PASS_VERIFY. */
static uint64_t state_hash(const CPUState *cpu) {
    uint64_t h = 1469598103934665603ULL;
    uint16_t tc[3], tt[3];
    uint32_t tm[3], tf[3];
    int32_t ti[3];
    uint32_t csv = interrupts_get_cycles_since_vblank();
    uint64_t cyc = psx_cycle_count;
    h = fnv(h, cpu->gpr, sizeof cpu->gpr);
    h = fnv(h, &cpu->hi, sizeof cpu->hi);
    h = fnv(h, &cpu->lo, sizeof cpu->lo);
    h = fnv(h, cpu->cop0, sizeof cpu->cop0);
    h = fnv(h, cpu->gte_data, sizeof cpu->gte_data);
    h = fnv(h, cpu->gte_ctrl, sizeof cpu->gte_ctrl);
    h = fnv(h, memory_get_ram_ptr(), memory_get_ram_bytes());
    h = fnv(h, memory_get_scratchpad_ptr(), RP_SPAD_SIZE);
    h = fnv(h, g_psx_icache_tv, sizeof g_psx_icache_tv);
    h = fnv(h, &i_stat, sizeof i_stat);
    h = fnv(h, &i_mask, sizeof i_mask);
    h = fnv(h, &csv, sizeof csv);
    h = fnv(h, &cyc, sizeof cyc);
    timers_get_snapshot(tc, tm, tt, ti, tf);
    h = fnv(h, tc, sizeof tc); h = fnv(h, tm, sizeof tm);
    h = fnv(h, tt, sizeof tt); h = fnv(h, ti, sizeof ti); h = fnv(h, tf, sizeof tf);
    if (s_dma_copy) {
        uint32_t n = dma_snapshot_bytes();
        uint8_t *tmp = (uint8_t *)malloc(n);
        if (tmp) { dma_snapshot_write(tmp); h = fnv(h, tmp, n); free(tmp); }
    }
    {
        uint64_t g = gpu_pass_state_hash();
        h = fnv(h, &g, sizeof g);
    }
    /* Devices a pass must never touch (their stores are dropped, their
     * clocks frozen): prove it. */
    {
        uint32_t (*bytes[4])(void) = { cdrom_snapshot_bytes, spu_snapshot_bytes,
                                       sio_snapshot_bytes, mdec_snapshot_bytes };
        void (*write[4])(uint8_t *) = { cdrom_snapshot_write, spu_snapshot_write,
                                        sio_snapshot_write, mdec_snapshot_write };
        for (int i = 0; i < 4; i++) {
            uint32_t n = bytes[i]();
            uint8_t *tmp = n ? (uint8_t *)malloc(n) : NULL;
            if (!tmp) continue;
            write[i](tmp);
            h = fnv(h, tmp, n);
            free(tmp);
        }
    }
    return h;
}

static int checkpoint_save(const CPUState *cpu) {
    uint32_t dma_len = dma_snapshot_bytes();
    /* The whole live RAM: a pass's stores fold through the same geometry
     * (render_pass_store_to), so every byte one can reach is in here. */
    uint32_t ram_bytes = memory_get_ram_bytes();
    s_checkpoint_failure = NULL;
    if (s_ram_copy_cap < ram_bytes) {
        uint8_t *p = (uint8_t *)realloc(s_ram_copy, ram_bytes);
        if (!p) { s_checkpoint_failure = "checkpoint_ram"; return 0; }
        s_ram_copy = p;
        s_ram_copy_cap = ram_bytes;
    }
    if (!s_dma_copy) {
        s_dma_copy = (uint8_t *)malloc(dma_len);
        if (!s_dma_copy) { s_checkpoint_failure = "checkpoint_dma"; return 0; }
    }
    if (!gpu_pass_checkpoint_save()) {
        s_checkpoint_failure = "checkpoint_gpu";
        return 0;
    }
    s_ck.cpu = *cpu;
    s_ck.ram_bytes = ram_bytes;
    memcpy(s_ram_copy, memory_get_ram_ptr(), ram_bytes);
    memcpy(s_ck.spad, memory_get_scratchpad_ptr(), RP_SPAD_SIZE);
    memcpy(s_ck.icache, g_psx_icache_tv, sizeof s_ck.icache);
    s_ck.i_stat = i_stat;
    s_ck.i_mask = i_mask;
    s_ck.csv = interrupts_get_cycles_since_vblank();
    timers_get_snapshot(s_ck.t_counter, s_ck.t_mode, s_ck.t_target,
                        s_ck.t_irq, s_ck.t_frac);
    s_ck.dma_len = dma_len;
    dma_snapshot_write(s_dma_copy);
    s_ck.store_count = g_guest_store_count;
    s_ck.mmio_count = g_mmio_access_count;
    s_ck.last_store_pc = g_debug_last_store_pc;
    s_ck.current_func = g_debug_current_func_addr;
    s_ck.dispatch_depth = g_psx_dispatch_depth;
    s_ck.call_bail = g_psx_call_bail;
    memset(&s_ck.nest, 0, sizeof s_ck.nest);
    nesting_save(&s_ck.nest);
    return 1;
}

static void checkpoint_restore(CPUState *cpu) {
    gpu_pass_checkpoint_restore();
    (void)dma_snapshot_read(s_dma_copy, s_ck.dma_len);
    timers_set_snapshot(s_ck.t_counter, s_ck.t_mode, s_ck.t_target,
                        s_ck.t_irq, s_ck.t_frac);
    interrupts_set_cycles_since_vblank(s_ck.csv);
    i_stat = s_ck.i_stat;
    i_mask = s_ck.i_mask;
    memcpy(memory_get_ram_ptr(), s_ram_copy, s_ck.ram_bytes);
    memcpy(memory_get_scratchpad_ptr(), s_ck.spad, RP_SPAD_SIZE);
    memcpy(g_psx_icache_tv, s_ck.icache, sizeof s_ck.icache);
    *cpu = s_ck.cpu;
    psx_irq_refresh_cause_ip2();
    g_guest_store_count = s_ck.store_count;
    g_mmio_access_count = s_ck.mmio_count;
    g_debug_last_store_pc = s_ck.last_store_pc;
    g_debug_current_func_addr = s_ck.current_func;
    g_psx_dispatch_depth = s_ck.dispatch_depth;
    g_psx_call_bail = s_ck.call_bail;
    nesting_restore(&s_ck.nest);
}

static double s_ms_per_tick = 0.0;
static double ema_ms(double cur, uint64_t ticks) {
    double ms = (double)ticks * s_ms_per_tick;
    return cur > 0.0 ? cur * 0.9 + ms * 0.1 : ms;
}

static void watchdog_overrun(void) {
    s_stats.watchdog++;
    s_stats.watchdog_flag = 1;
    if (s_abort_armed) {
        s_abort_armed = 0;
        longjmp(s_abort_jmp, 1);
    }
}

static char s_abort_detail[192];

static void note_fault(const char *what) {
    s_stats.aborted++;
    if (s_stats.aborted <= 4)
        fprintf(stderr, "psxrecomp: render pass rolled back (%s%s%s)\n", what,
                s_abort_detail[0] ? "; restored skipped exits: " : "",
                s_abort_detail);
    s_abort_detail[0] = '\0';
    if (s_stats.watchdog + s_stats.vram_leaks >= RP_FAULT_LIMIT &&
        !s_stats.disabled) {
        s_stats.disabled = 1;
        fprintf(stderr, "psxrecomp: render passes disabled for this session "
                "after %u faults\n", (unsigned)RP_FAULT_LIMIT);
    }
}

static int transaction_end(int eye, uint32_t alpha, int keep) {
    if (eye >= 0) return gl_renderer_stereo_end((uint32_t)eye, keep);
    gl_renderer_pass_end(alpha, keep);
    return 1;
}

static int render_transaction(struct CPUState *cpu, const PSXModRenderPass *pass,
                              PSXModRenderPassFn fn, void *user, int eye) {
    uint64_t t0, t1, tb, tg, te, tr, hash_before = 0, hash_after = 0;
    uint64_t cycles_before;
    uint32_t leaks;
    int ok = 0, open, reuse;
    static uint32_t s_leaks_before;
    uint32_t status;

    memset(&s_attempt, 0, sizeof s_attempt);
    s_attempt.attempt = ++s_stats.pass_attempts;
    s_attempt.plan = s_plan_serial;
    s_attempt.guest_cycle = psx_cycle_count;
    if (pass) {
        s_attempt.struct_size = pass->struct_size;
        if (pass->struct_size >= sizeof *pass) {
            s_attempt.alpha_q16 = pass->alpha_q16;
            s_attempt.x = pass->x; s_attempt.y = pass->y;
            s_attempt.w = pass->w; s_attempt.h = pass->h;
        }
    }

    if (!cpu || !pass || !fn || pass->struct_size < sizeof *pass ||
        pass->w == 0 || pass->h == 0 || (eye < 0 &&
        (pass->alpha_q16 == 0 || pass->alpha_q16 >= 65536u))) {
        s_stats.argument_refused++;
        return pass_refuse("arguments", transaction_status(eye >= 0));
    }
    status = transaction_status(eye >= 0);
    if (status != PSX_MOD_RENDER_PASS_READY) {
        s_stats.status_refused++;
        return pass_refuse("status", status);
    }

    /* An overlay DLL may still hold cycles it has not published. They belong
     * to the live timeline: publish them now, or the pass's first store would
     * publish them into the frozen clock and the restore would drop them. */
    if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();

    s_attempt.guest_cycle = psx_cycle_count;
    t0 = gl_renderer_perf_ticks();
    open = eye < 0 ? s_open_generation : 0;
    {
        static double inv = 0.0;
        if (inv == 0.0) inv = 1000.0 / (double)gl_renderer_perf_frequency();
        s_ms_per_tick = inv;
    }
    reuse = !open && s_restored_valid && s_restored_plan == s_plan_serial &&
            s_restored_cycle == psx_cycle_count &&
            s_restored_stores == g_guest_store_count;
    /* Frame N's own image is captured by the first pass after a plan. */
    if (!(eye >= 0 ? gl_renderer_stereo_begin(pass->x, pass->y, pass->w,
                                               pass->h, reuse)
                    : gl_renderer_pass_begin(pass->x, pass->y, pass->w,
                                               pass->h, open, s_plan_period, reuse))) {
        gl_renderer_pass_begin_diag(&s_attempt.gl);
        s_stats.begin_refused++;
        return pass_refuse(s_attempt.gl.reason ? s_attempt.gl.reason : "gl_begin",
                           s_attempt.gl.status);
    }
    s_open_generation = 0;
    s_leaks_before = gl_renderer_pass_leaks();

    if (!checkpoint_save(cpu)) {
        gl_renderer_pass_begin_diag(&s_attempt.gl);
        s_stats.checkpoint_refused++;
        /* Capture the rejection status before rolling the GL transaction back. */
        status = transaction_status(eye >= 0);
        transaction_end(eye, 0, 0);
        return pass_refuse(s_checkpoint_failure, status);
    }
    if (verify_on()) hash_before = state_hash(cpu);

    tb = gl_renderer_perf_ticks();
    s_nesting = 1;
    cycles_before = psx_cycle_count;
    if (eye >= 0) {
        s_stereo.eye_cycle[eye] = cycles_before;
        s_stereo.eye_hash[eye] = verify_on() ? hash_before : 0;
    }
    (void)psx_cycle_freeze_begin(&s_freeze, watchdog_cycles(),
                                 watchdog_overrun);
    if (setjmp(s_abort_jmp) == 0) {
        s_abort_armed = 1;
        ok = fn(cpu, user, pass->alpha_q16) ? 1 : 0;
        s_abort_armed = 0;
        if (!ok) s_stats.discarded++;
        if (verify_on() && !nesting_balanced(&s_ck.nest)) {
            s_stats.verify_mismatch++;
            if (eye >= 0) ok = 0;
            fprintf(stderr, "psxrecomp: RENDER PASS VERIFY mismatch "
                    "(host nesting not balanced after the pass)\n");
        }
    } else {
        /* Watchdog abort from inside guest code. An overlay shard may still
         * hold cycles it has not published: publish them now, into the
         * frozen clock the restore discards, never into the live one. The
         * nesting the longjmp skipped is put back by checkpoint_restore and
         * psx_cycle_freeze_end below. */
        if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();
        if (nesting_describe(&s_ck.nest, s_freeze.bb_defer, s_abort_detail,
                             sizeof s_abort_detail))
            s_stats.nesting_repairs++;
        snprintf(s_stats.last_abort_detail, sizeof s_stats.last_abort_detail,
                 "%s", s_abort_detail);
        ok = 0;
    }
    s_stats.guest_cycles_last = psx_cycle_count - cycles_before;
    if (eye >= 0) gte_render_view_get(s_stereo.eye_view[eye]);
    tg = gl_renderer_perf_ticks();

    leaks = gl_renderer_pass_leaks() - s_leaks_before;
    if (leaks) {
        s_stats.vram_leaks += leaks;
        ok = 0;
    }
    /* Capture (when kept), then roll the VRAM rect and renderer back. */
    if (!transaction_end(eye, ok ? pass->alpha_q16 : 0, ok)) {
        ok = 0;
        if (eye >= 0) s_stereo.last_failure = "eye_capture";
    }
    te = gl_renderer_perf_ticks();
    checkpoint_restore(cpu);
    psx_cycle_freeze_end(&s_freeze);
    s_nesting = 0;
    s_restored_valid = 1;
    s_restored_plan = s_plan_serial;
    s_restored_cycle = psx_cycle_count;
    s_restored_stores = g_guest_store_count;
    tr = gl_renderer_perf_ticks();
    s_stats.avg_begin_ms = ema_ms(s_stats.avg_begin_ms, tb - t0);
    s_stats.avg_guest_ms = ema_ms(s_stats.avg_guest_ms, tg - tb);
    s_stats.avg_end_ms = ema_ms(s_stats.avg_end_ms, te - tg);
    s_stats.avg_restore_ms = ema_ms(s_stats.avg_restore_ms, tr - te);

    if (verify_on()) {
        hash_after = state_hash(cpu);
        s_stats.verify_checks++;
        if (hash_after != hash_before || !gl_renderer_pass_verify_vram()) {
            s_stats.verify_mismatch++;
            if (eye >= 0) { ok = 0; s_stereo.last_failure = "restore_verify"; }
            if (s_stats.verify_mismatch <= 8)
                fprintf(stderr, "psxrecomp: RENDER PASS VERIFY mismatch "
                        "(state %016llx -> %016llx)\n",
                        (unsigned long long)hash_before,
                        (unsigned long long)hash_after);
        }
    }

    t1 = gl_renderer_perf_ticks();
    {
        double ms = (double)(t1 - t0) * 1000.0 /
                    (double)gl_renderer_perf_frequency();
        s_stats.last_pass_ms = ms;
        s_stats.avg_pass_ms = s_stats.avg_pass_ms > 0.0
            ? s_stats.avg_pass_ms * 0.9 + ms * 0.1 : ms;
    }
    if (eye < 0) gl_renderer_pass_note_cost(t1 - t0);
    if (ok) s_stats.passes++;
    else if (leaks) note_fault("VRAM write outside the pass rect");
    else if (s_stats.watchdog_flag) note_fault("guest-cycle watchdog");
    s_stats.watchdog_flag = 0;
    /* Present anything that fell due while the pass ran. */
    if (eye < 0) gl_renderer_pass_service_presents();
    return ok;
}

int psx_mod_render_pass(struct CPUState *cpu, const PSXModRenderPass *pass,
                        PSXModRenderPassFn fn, void *user) {
    return render_transaction(cpu, pass, fn, user, -1);
}

typedef struct StereoCallback {
    PSXModStereoFn fn;
    void *user;
    uint32_t eye;
} StereoCallback;
static int stereo_eye_draw(CPUState *cpu, void *user, uint32_t unused) {
    StereoCallback *c = (StereoCallback *)user;
    (void)unused;
    return c->fn(cpu, c->user, c->eye);
}

int psx_mod_render_stereo(CPUState *cpu, const PSXModStereoFrame *frame,
                          PSXModStereoFn fn, void *user) {
    PSXModRenderPass rect;
    StereoCallback call;
    uint64_t t0, cycle, stores, mismatches, aborted;
    double budget_ms, elapsed;
    uint32_t status;
    int ok = 1;
    /* Refuse re-entry without changing the outer pair's counters/selector. */
    if (s_pair_busy) return 0;
    s_stereo.attempts++;
    s_stereo.last_eye = -1;
    if (!cpu || !frame || !fn || frame->struct_size < sizeof *frame ||
        !frame->w || !frame->h || !frame->period_vblanks ||
        frame->period_vblanks > 8u || (uint32_t)frame->x + frame->w > 1024u ||
        (uint32_t)frame->y + frame->h > 512u) {
        s_stereo.refused++; s_stereo.last_failure = "arguments"; return 0;
    }
    status = transaction_status(1);
    if (status != PSX_MOD_RENDER_PASS_READY) {
        s_stereo.refused++; s_stereo.last_failure = "status"; return 0;
    }
    budget_ms = frame->period_vblanks * (double)g_psx_vblank_cycles /
                33868.8 * 0.8;
    if (s_stereo.avg_pair_ms > budget_ms && ++s_pair_retry < 30u) {
        s_stereo.shed++; return 0;
    }
    s_pair_retry = 0;
    if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();
    cycle = psx_cycle_count; stores = g_guest_store_count;
    mismatches = s_stats.verify_mismatch; aborted = s_stats.aborted;
    s_stereo.last_failure = NULL;
    s_stereo.eye_cycle[0] = s_stereo.eye_cycle[1] = 0;
    s_stereo.eye_hash[0] = s_stereo.eye_hash[1] = 0;
    gl_renderer_stereo_stage_reset();
    memset(&rect, 0, sizeof rect);
    rect.struct_size = sizeof rect;
    rect.x = frame->x; rect.y = frame->y; rect.w = frame->w; rect.h = frame->h;
    call.fn = fn; call.user = user;
    s_pair_busy = 1;
    s_plan_serial++; /* separate the backup lease from any temporal plan */
    t0 = gl_renderer_perf_ticks();
    for (call.eye = 0; call.eye < 2u; call.eye++) {
        s_stereo.last_eye = (int)call.eye;
        if (psx_cycle_count != cycle || g_guest_store_count != stores) {
            ok = 0; s_stereo.last_failure = "checkpoint_changed"; break;
        }
        if (!render_transaction(cpu, &rect, stereo_eye_draw, &call, (int)call.eye)) {
            ok = 0;
            if (!s_stereo.last_failure) s_stereo.last_failure =
                s_stats.aborted != aborted ? "eye_abort" : "eye_refused_or_discarded";
            break;
        }
    }
    if (s_stats.verify_mismatch != mismatches ||
        s_stereo.eye_cycle[0] != s_stereo.eye_cycle[1] ||
        (verify_on() && s_stereo.eye_hash[0] != s_stereo.eye_hash[1])) {
        ok = 0;
        if (!s_stereo.last_failure) s_stereo.last_failure = "eye_checkpoint_mismatch";
    }
    elapsed = (double)(gl_renderer_perf_ticks() - t0) * 1000.0 /
              (double)gl_renderer_perf_frequency();
    s_stereo.last_pair_ms = elapsed;
    s_stereo.avg_pair_ms = s_stereo.avg_pair_ms > 0.0
        ? s_stereo.avg_pair_ms * 0.9 + elapsed * 0.1 : elapsed;
    if (ok && !gl_renderer_stereo_publish(s_stereo.attempts, cycle, s_stereo.eye_view)) {
        ok = 0; s_stereo.last_failure = "pair_publish";
    }
    if (ok) {
        s_stereo.pairs++;
        s_stereo.last_pair_id = s_stereo.attempts;
        s_stereo.last_guest_cycle = cycle;
    } else {
        s_stereo.failed++;
        s_stereo.last_failed_attempt = s_stereo.attempts;
        s_stereo.last_failed_eye = s_stereo.last_eye;
        s_stereo.last_failed_reason = s_stereo.last_failure;
        s_stereo.retained_pair_id = s_stereo.last_pair_id;
        gl_renderer_stereo_stage_reset();
    }
    s_pair_busy = 0;
    return ok;
}
