/* restore_edge_irq_replay_test (#636, PR #660 review).
 *
 * Executable restore-vs-live regression over the REAL scheduler (traps.c:
 * psx_scheduler_run / psx_scheduler_resume_at) and the REAL IRQ delivery
 * (interrupts.c: psx_check_interrupts*, interrupts_resync_after_restore).
 * Only psx_dispatch is the harness: a small deterministic guest whose blocks
 * follow the generated-code contract
 *
 *   compiled game entry   psx_check_interrupts_dispatch_entry(pc), body,
 *                         edge check psx_check_interrupts_at(next)
 *   BIOS ROM / CPS cont.  body, edge check psx_check_interrupts_at(next)
 *                         (no dispatch-entry check)
 *   interpreted (dirty)   body, g_dirty_safe_resume_pc = next,
 *                         psx_check_interrupts
 *
 * and an exception handler behind the RAM vector (LUI/ADDIU k0) that records
 * delivery cycle, EPC, CAUSE, SR and the interrupted registers, saves EPC and
 * registers into the current TCB (PCB at 0x108), acks I_STAT and RFEs.
 *
 * Each case runs LIVE from cold, capturing state at one block edge (just
 * before that edge's IRQ check, with the edge's resume PC — what a poll
 * inside psx_check_interrupts captures), then REPLAYS by restoring that
 * snapshot from inside guest execution the way savestate/rewind/selfcheck/
 * rollback do (interrupts_resync_after_restore + psx_scheduler_resume_at).
 * Every delivery (cycle, EPC, CAUSE, SR, I_STAT, interrupted GPRs, TCB) and
 * every block (pc, cycle, GPR hash), plus the final CPU/RAM/INTC state, must
 * match the live run from the snapshot on. */
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_scheduler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t  g_test_ram[0x200000];
extern uint64_t psx_cycle_count;
extern uint32_t i_stat, i_mask;
extern uint32_t g_dirty_safe_resume_pc;
extern int (*g_test_recover_null_pc)(CPUState *, uint32_t *);
extern uint32_t psx_read_word(uint32_t addr);
extern void psx_host_write_word(uint32_t addr, uint32_t value);

#define SR_IEC  0x00000001u
#define SR_IM2  0x00000400u
#define C0_SR   12
#define C0_CAUSE 13
#define C0_EPC  14

#define HANDLER_PC 0x80001000u
#define LOADER_PC  0x80003000u
#define PCB_ADDR   0x80000200u
#define TCB_ADDR   0x80000300u
#define STEP_ADDR  0x80060000u   /* guest step counter (RAM, snapshotted) */
#define VBCNT_ADDR 0x80060004u   /* handler-side VBlank counter            */
#define SCRATCH    0x80050000u

enum kind { K_GAME, K_ROM, K_CONT, K_INTERP };

typedef struct {
    enum kind kind;
    uint32_t base;        /* first block PC                           */
    uint32_t nblocks;     /* blocks per loop (PC = base + 0x20*i)      */
    uint32_t cost;        /* cycles per block body                     */
    uint32_t steps;       /* blocks to run before the guest exits      */
    uint32_t snap_step;   /* capture at the edge after this many blocks */
    uint64_t raise_cycle; /* device raises I_STAT.VBlank at this cycle  */
    uint32_t imask0, sr0;
    /* guest unmask/enable at this step (0 = never): sets I_MASK/SR */
    uint32_t enable_step, enable_imask, enable_sr;
    int handler_zero_cost;   /* handler takes no guest cycles (dedup case) */
} Program;

typedef struct {
    CPUState cpu;
    uint8_t  ram[0x200000];
    uint32_t istat, imask;
    uint64_t cycle;
    int      raised;
    uint32_t resume_pc;
} Snap;

/* Event log ----------------------------------------------------------- */
typedef struct {
    int      is_irq;
    uint32_t pc;          /* block pc, or EPC for an IRQ               */
    uint64_t cycle;
    uint32_t cause, sr, istat;
    uint32_t gpr_hash;
    uint32_t tcb_epc, tcb_r8, tcb_r9, tcb_ra;
} Ev;

#define LOG_CAP 4096
typedef struct { Ev ev[LOG_CAP]; int n; } Log;

static Log      g_live, g_replay, *g_log;
static int      g_logging;
static const Program *g_prog;
static int      g_raised;
static Snap    *g_snap;
static int      g_snap_taken;
static int      g_mode_replay;
static int      g_entry_prechecked;     /* checked_at_current_cycle seen at first replay entry */
static int      g_first_replay_entry;
static uint32_t g_recover_pc;
static int      g_recover_used;
static int      g_recover_log_idx;
static int      g_irq_count;

static uint32_t rd(uint32_t a) { return psx_read_word(a); }
static void     wr(uint32_t a, uint32_t v) { psx_host_write_word(a, v); }

static uint32_t hash_gprs(const CPUState *c)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 32; i++) { h ^= c->gpr[i]; h *= 16777619u; }
    h ^= c->hi; h *= 16777619u; h ^= c->lo; h *= 16777619u;
    return h;
}

static void log_ev(const Ev *e)
{
    if (!g_logging) return;
    if (g_log->n >= LOG_CAP) { fprintf(stderr, "log overflow\n"); exit(2); }
    g_log->ev[g_log->n++] = *e;
}

static uint32_t block_pc(uint32_t i) { return g_prog->base + 0x20u * (i % g_prog->nblocks); }

static void take_snapshot(CPUState *cpu, uint32_t resume_pc)
{
    memcpy(&g_snap->cpu, cpu, sizeof(*cpu));
    memcpy(g_snap->ram, g_test_ram, sizeof(g_test_ram));
    g_snap->istat = i_stat;
    g_snap->imask = i_mask;
    g_snap->cycle = psx_cycle_count;
    g_snap->raised = g_raised;
    g_snap->resume_pc = resume_pc;
    g_snap_taken = 1;
    g_logging = 1;                 /* compare everything after the edge */
}

/* The block edge: generated code's psx_check_interrupts_at(next) or the
 * dirty interpreter's pump. The capture sits where a poll inside the check
 * captures: same edge, same resume PC, before delivery is evaluated. */
static void edge_check(CPUState *cpu, uint32_t next, uint32_t step)
{
    if (!g_mode_replay && !g_snap_taken && step == g_prog->snap_step)
        take_snapshot(cpu, next);
    if (g_prog->kind == K_INTERP) {
        g_dirty_safe_resume_pc = next;
        psx_check_interrupts(cpu);
        g_dirty_safe_resume_pc = 0;
    } else {
        psx_check_interrupts_at(cpu, next);
    }
}

static void run_handler(CPUState *cpu)
{
    Ev e = {0};
    e.is_irq = 1;
    e.pc = cpu->cop0[C0_EPC];
    e.cycle = psx_cycle_count;
    e.cause = cpu->cop0[C0_CAUSE];
    e.sr = cpu->cop0[C0_SR];
    e.istat = i_stat & i_mask;
    e.gpr_hash = hash_gprs(cpu);
    /* BIOS-style save of the interrupted context into the current TCB. */
    uint32_t tcb = rd(rd(0x108u) & 0x1FFFFFFFu);
    wr(tcb + 0x88u, cpu->cop0[C0_EPC]);
    wr(tcb + 0x20u, cpu->gpr[8]);
    wr(tcb + 0x24u, cpu->gpr[9]);
    wr(tcb + 0x7Cu, cpu->gpr[31]);
    e.tcb_epc = rd(tcb + 0x88u);
    e.tcb_r8 = rd(tcb + 0x20u);
    e.tcb_r9 = rd(tcb + 0x24u);
    e.tcb_ra = rd(tcb + 0x7Cu);
    log_ev(&e);
    g_irq_count++;
    wr(VBCNT_ADDR, rd(VBCNT_ADDR) + 1u);
    i_stat = 0;                               /* ack */
    cpu->gpr[8] = 0xDEAD0008u;                /* handler clobbers */
    cpu->gpr[27] = 0xDEAD001Bu;
    if (!g_prog->handler_zero_cost) psx_cycle_count += 37;
    /* RFE: pop the SR stack, return to EPC. */
    uint32_t sr = cpu->cop0[C0_SR];
    cpu->cop0[C0_SR] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
    cpu->pc = cpu->cop0[C0_EPC];
}

static void restore_snapshot_and_resume(CPUState *cpu)
{
    memcpy(cpu, &g_snap->cpu, sizeof(*cpu));
    memcpy(g_test_ram, g_snap->ram, sizeof(g_test_ram));
    i_stat = g_snap->istat;
    i_mask = g_snap->imask;
    psx_cycle_count = g_snap->cycle;
    g_raised = g_snap->raised;
    g_logging = 1;
    g_first_replay_entry = 1;
    interrupts_resync_after_restore();
    cpu->pc = g_snap->resume_pc;
    psx_scheduler_resume_at(g_snap->resume_pc);   /* never returns */
}

static int block_index(uint32_t pc, uint32_t *idx)
{
    if (pc < g_prog->base) return 0;
    uint32_t off = pc - g_prog->base;
    if ((off & 0x1Fu) || off / 0x20u >= g_prog->nblocks) return 0;
    *idx = off / 0x20u;
    return 1;
}

void psx_dispatch(CPUState *cpu, uint32_t pc)
{
    for (;;) {
        cpu->pc = pc;
        if (pc == 0u) return;
        if (pc == HANDLER_PC) { run_handler(cpu); return; }
        if (pc == LOADER_PC) { restore_snapshot_and_resume(cpu); return; }
        uint32_t idx;
        if (!block_index(pc, &idx)) {
            fprintf(stderr, "harness: dispatch to unknown pc %08X\n", pc);
            exit(2);
        }
        if (g_prog->kind == K_GAME) {
            if (g_first_replay_entry)
                g_entry_prechecked = psx_interrupts_checked_at_current_cycle(pc);
            psx_check_interrupts_dispatch_entry(cpu, pc);
        }
        g_first_replay_entry = 0;
        /* Block body: deterministic function of the guest state. */
        uint32_t step = rd(STEP_ADDR) + 1u;
        wr(STEP_ADDR, step);
        cpu->gpr[8] = cpu->gpr[8] * 31u + pc + (uint32_t)psx_cycle_count;
        cpu->gpr[9] ^= cpu->gpr[8] + step;
        cpu->gpr[31] = pc + 8u;
        wr(SCRATCH + (step & 15u) * 4u, cpu->gpr[8]);
        if (g_prog->enable_step && step == g_prog->enable_step) {
            i_mask |= g_prog->enable_imask;
            cpu->cop0[C0_SR] |= g_prog->enable_sr;
        }
        psx_cycle_count += g_prog->cost;
        if (!g_raised && psx_cycle_count >= g_prog->raise_cycle) {
            i_stat |= 1u;                        /* VBlank */
            g_raised = 1;
        }
        Ev e = {0};
        e.pc = pc; e.cycle = psx_cycle_count; e.gpr_hash = hash_gprs(cpu);
        e.istat = i_stat; e.sr = cpu->cop0[C0_SR];
        log_ev(&e);
        if (step == g_prog->steps || step == g_prog->steps + 10u) {   /* guest exit */
            cpu->pc = 0;
            return;
        }
        uint32_t next = block_pc(idx + 1u);
        edge_check(cpu, next, step);
        pc = next;
    }
}

static void reset_machine(CPUState *cpu, const Program *p)
{
    memset(cpu, 0, sizeof(*cpu));
    memset(g_test_ram, 0, sizeof(g_test_ram));
    cpu->read_word = psx_read_word;
    cpu->write_word = psx_host_write_word;
    /* RAM exception vector: lui k0,0x8000 ; addiu k0,k0,0x1000 */
    wr(0x80000080u, 0x3C1A8000u);
    wr(0x80000084u, 0x275A1000u);
    wr(0x108u, PCB_ADDR);
    wr(PCB_ADDR, TCB_ADDR);
    cpu->cop0[C0_SR] = p->sr0;
    for (int i = 1; i < 32; i++) cpu->gpr[i] = 0x01010101u * (uint32_t)i;
    psx_cycle_count = 1000;
    i_stat = 0;
    i_mask = p->imask0;
    g_raised = 0;
    g_dirty_safe_resume_pc = 0;
    g_first_replay_entry = 0;
    interrupts_resync_after_restore();
    psx_scheduler_top_level_resume_clear();
}

typedef struct {
    CPUState cpu;
    uint32_t istat, imask;
    uint64_t cycle;
} Final;

static void capture_final(Final *f, const CPUState *c)
{
    memcpy(&f->cpu, c, sizeof(*c));
    f->istat = i_stat; f->imask = i_mask; f->cycle = psx_cycle_count;
}

static Snap   g_snap_storage;
static uint8_t g_live_ram[0x200000];

static int compare_runs(const char *name, const Final *lf, const Final *rf)
{
    int n = g_live.n < g_replay.n ? g_live.n : g_replay.n;
    for (int i = 0; i < n; i++) {
        const Ev *a = &g_live.ev[i], *b = &g_replay.ev[i];
        if (memcmp(a, b, sizeof(*a)) != 0) {
            fprintf(stderr,
                "%s: event %d differs: live %s pc=%08X cyc=%llu sr=%08X tcb_epc=%08X | "
                "replay %s pc=%08X cyc=%llu sr=%08X tcb_epc=%08X\n", name, i,
                a->is_irq ? "IRQ" : "blk", a->pc, (unsigned long long)a->cycle, a->sr, a->tcb_epc,
                b->is_irq ? "IRQ" : "blk", b->pc, (unsigned long long)b->cycle, b->sr, b->tcb_epc);
            return 1;
        }
    }
    if (g_live.n != g_replay.n) {
        fprintf(stderr, "%s: event count live=%d replay=%d\n", name, g_live.n, g_replay.n);
        return 1;
    }
    if (memcmp(lf->cpu.gpr, rf->cpu.gpr, sizeof(lf->cpu.gpr)) ||
        memcmp(lf->cpu.cop0, rf->cpu.cop0, sizeof(lf->cpu.cop0)) ||
        lf->cpu.hi != rf->cpu.hi || lf->cpu.lo != rf->cpu.lo ||
        lf->istat != rf->istat || lf->imask != rf->imask || lf->cycle != rf->cycle) {
        fprintf(stderr, "%s: final CPU/INTC state differs (cycle live=%llu replay=%llu)\n",
                name, (unsigned long long)lf->cycle, (unsigned long long)rf->cycle);
        return 1;
    }
    if (memcmp(g_live_ram, g_test_ram, sizeof(g_test_ram)) != 0) {
        fprintf(stderr, "%s: final RAM differs\n", name);
        return 1;
    }
    return 0;
}

static int count_irqs(const Log *l)
{
    int n = 0;
    for (int i = 0; i < l->n; i++) n += l->ev[i].is_irq;
    return n;
}

/* Live run from cold, then replay from the captured edge. Returns 0 on match. */
static int live_vs_replay(const char *name, const Program *p, int expect_irq_at_edge)
{
    static CPUState cpu;
    Final lf, rf;
    g_prog = p;
    g_snap = &g_snap_storage;

    /* LIVE */
    reset_machine(&cpu, p);
    g_mode_replay = 0; g_snap_taken = 0; g_logging = 0;
    g_live.n = 0; g_log = &g_live;
    cpu.pc = p->base;
    psx_scheduler_run(&cpu);
    if (!g_snap_taken) { fprintf(stderr, "%s: snapshot edge never reached\n", name); return 1; }
    capture_final(&lf, &cpu);
    memcpy(g_live_ram, g_test_ram, sizeof(g_test_ram));

    /* The live edge must (or must not) deliver right at the snapshot edge. */
    int live_irq_at_edge = g_live.n > 0 && g_live.ev[0].is_irq &&
                           g_live.ev[0].pc == g_snap->resume_pc &&
                           g_live.ev[0].cycle == g_snap->cycle;
    if (live_irq_at_edge != expect_irq_at_edge) {
        fprintf(stderr, "%s: fixture: live delivery at snapshot edge = %d, expected %d\n",
                name, live_irq_at_edge, expect_irq_at_edge);
        return 1;
    }

    /* REPLAY: the load is polled from inside guest execution and resumes
     * through the scheduler, like every restore consumer. */
    reset_machine(&cpu, p);
    g_mode_replay = 1; g_logging = 0;
    g_replay.n = 0; g_log = &g_replay;
    cpu.pc = LOADER_PC;
    psx_scheduler_run(&cpu);
    capture_final(&rf, &cpu);
    if (compare_runs(name, &lf, &rf)) return 1;
    printf("  %-34s OK  (%d events, %d IRQs, edge pc=%08X cyc=%llu)\n", name,
           g_live.n, count_irqs(&g_live), g_snap->resume_pc,
           (unsigned long long)g_snap->cycle);
    return 0;
}

static int recover_hook(CPUState *c, uint32_t *out_pc)
{
    (void)c;
    if (g_recover_used) return 0;
    g_recover_used = 1;
    g_recover_log_idx = g_log->n;
    *out_pc = g_recover_pc;
    return 1;
}

int main(void)
{
    int fail = 0;
    const uint32_t SR_ON = SR_IEC | SR_IM2;

    /* Snapshot edge after block 40 at 1000 + 40*50 = 3000; VBlank raised by
     * the block that ends there, so it is pending at the edge. */
    Program game = { K_GAME, 0x80010000u, 7, 50, 120, 40, 3000, 1u, SR_ON, 0, 0, 0, 0 };
    Program rom  = game; rom.kind = K_ROM;    rom.base = 0xBFC07600u;
    Program cont = game; cont.kind = K_CONT;  cont.base = 0x80030000u;
    Program intp = game; intp.kind = K_INTERP; intp.base = 0x80040000u;

    puts("restore-edge replay (pending IRQ at the restore edge):");
    fail |= live_vs_replay("compiled game resume", &game, 1);
    if (count_irqs(&g_replay) != 1) {
        fprintf(stderr, "compiled game resume: %d deliveries, expected 1\n", count_irqs(&g_replay));
        fail = 1;
    }
    /* Same-edge dedup: with a handler that takes no guest cycles, the edge
     * delivery leaves cycle+PC stamped, so the compiled game dispatch entry
     * that follows at the same cycle+PC is a no-op - live (after the edge
     * check) and on replay (after the scheduler's restore-edge check). One
     * delivery, not two, and the entry saw the edge as already checked. */
    {
        Program p = game;
        p.handler_zero_cost = 1;
        g_entry_prechecked = 0;
        fail |= live_vs_replay("compiled game, same-edge dedup", &p, 1);
        int at_edge = 0;
        for (int i = 0; i < g_replay.n; i++)
            at_edge += g_replay.ev[i].is_irq && g_replay.ev[i].pc == g_snap->resume_pc &&
                       g_replay.ev[i].cycle == g_snap->cycle;
        if (!g_entry_prechecked || at_edge != 1) {
            fprintf(stderr, "same-edge dedup: entry prechecked=%d, deliveries at edge=%d "
                            "(expected 1, 1)\n", g_entry_prechecked, at_edge);
            fail = 1;
        }
    }
    fail |= live_vs_replay("BIOS ROM resume", &rom, 1);
    fail |= live_vs_replay("CPS continuation resume", &cont, 1);
    fail |= live_vs_replay("interpreted (dirty) resume", &intp, 1);

    puts("no pending IRQ at the restore edge:");
    {
        Program p;
        p = game; p.raise_cycle = 4000;
        fail |= live_vs_replay("game, VBlank later", &p, 0);
        p = rom;  p.raise_cycle = 4000;  fail |= live_vs_replay("ROM, VBlank later", &p, 0);
        p = intp; p.raise_cycle = 4000;  fail |= live_vs_replay("interp, VBlank later", &p, 0);
        p = rom;  p.raise_cycle = ~0ull; fail |= live_vs_replay("ROM, no IRQ at all", &p, 0);
    }

    puts("interrupt masking at the restore edge (guest unmasks at step 45):");
    {
        Program p;
        p = rom; p.imask0 = 0; p.enable_step = 45; p.enable_imask = 1u;
        fail |= live_vs_replay("ROM, I_MASK clear", &p, 0);
        p = rom; p.sr0 = SR_IM2; p.enable_step = 45; p.enable_sr = SR_IEC;
        fail |= live_vs_replay("ROM, SR.IEc clear", &p, 0);
        p = rom; p.sr0 = SR_IEC; p.enable_step = 45; p.enable_sr = SR_IM2;
        fail |= live_vs_replay("ROM, SR.IM2 clear", &p, 0);
        p = game; p.imask0 = 0; p.enable_step = 45; p.enable_imask = 1u;
        fail |= live_vs_replay("game, I_MASK clear", &p, 0);
        p = intp; p.sr0 = SR_IM2; p.enable_step = 45; p.enable_sr = SR_IEC;
        fail |= live_vs_replay("interp, SR.IEc clear", &p, 0);
    }

    /* Recovery path: after a restore has consumed its one-shot check, the
     * guest's top-level dispatch returns PC 0 and the null-PC recovery
     * resumes a ROM block via RESUME_CURRENT with a VBlank pending. It is not
     * a restore edge: the block body runs first and the IRQ is taken at the
     * block's own edge (pre-#660 behaviour), not at the recovery PC. */
    puts("recovery path (scheduler escape without resume_at):");
    {
        static CPUState cpu;
        Program p = rom;
        p.steps = 60; p.raise_cycle = ~0ull;
        g_prog = &p; g_snap = &g_snap_storage;
        reset_machine(&cpu, &p);
        g_mode_replay = 0; g_snap_taken = 0; g_logging = 0; g_log = &g_live; g_live.n = 0;
        cpu.pc = p.base;
        psx_scheduler_run(&cpu);               /* live run to take a snapshot */
        reset_machine(&cpu, &p);
        g_mode_replay = 1; g_log = &g_replay; g_replay.n = 0;
        g_recover_pc = block_pc(3);
        g_recover_used = 0;
        g_test_recover_null_pc = recover_hook;
        /* VBlank is raised by the block the guest exits after (step 60), so
         * it is pending, undelivered, when the recovery resumes. */
        p.raise_cycle = 1000 + 60u * p.cost;
        g_irq_count = 0;
        cpu.pc = LOADER_PC;
        psx_scheduler_run(&cpu);   /* restore -> run to exit -> recover -> exit */
        g_test_recover_null_pc = NULL;
        const Ev *irq = NULL;
        int recov_idx = g_recover_used ? g_recover_log_idx : -1;
        for (int i = recov_idx < 0 ? 0 : recov_idx; i < g_replay.n; i++)
            if (g_replay.ev[i].is_irq) { irq = &g_replay.ev[i]; break; }
        /* Expected after recovery: [block g_recover_pc] [IRQ, EPC = next block]. */
        if (!g_recover_used || !irq || recov_idx >= g_replay.n) {
            fprintf(stderr, "recovery: fixture did not recover with a pending IRQ "
                            "(used=%d irq=%d)\n", g_recover_used, irq != NULL);
            fail = 1;
        } else if (g_replay.ev[recov_idx].is_irq || g_replay.ev[recov_idx].pc != g_recover_pc ||
                   irq->pc != block_pc(4) || (irq - g_replay.ev) != recov_idx + 1) {
            fprintf(stderr, "recovery: after resuming at %08X the first event is %s %08X and "
                            "the IRQ EPC is %08X; expected the block body first and the IRQ "
                            "at its edge %08X (the recovery inherited the restore-edge check)\n",
                    g_recover_pc, g_replay.ev[recov_idx].is_irq ? "IRQ" : "block",
                    g_replay.ev[recov_idx].pc, irq->pc, block_pc(4));
            fail = 1;
        } else {
            printf("  %-34s OK  (recovered at %08X, IRQ at its edge %08X)\n",
                   "null-PC recovery after restore", g_recover_pc, irq->pc);
        }
    }

    if (fail) { puts("restore_edge_irq_replay_test: FAIL"); return 1; }
    puts("restore_edge_irq_replay_test: PASS");
    return 0;
}
