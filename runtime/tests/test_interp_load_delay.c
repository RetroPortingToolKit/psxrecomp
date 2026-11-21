/* R3000A load delay in the dirty-RAM interpreter, game RAM and COP2.
 *
 * Interpreter-side twin of recompiler/tests/test_cop2_delay_slot_load.py
 * (the CFG emitter). MFC2/CFC2 write their GPR one instruction late, like LW;
 * the instruction in the delay slot sees the old value. Tekken 2's (USA Rev 1)
 * fighter vertex loop (0x80072890) depends on it:
 *
 *     L:  mtc2  t0, VXY0       # must see the OLD t0 (the lw below)
 *         ...
 *         lw    t0, 0(t8)
 *         ...
 *         bgtz  t9, L
 *         mfc2  t0, SZ1        # delay slot: t0 lands after L's mtc2
 *
 * The interpreter used to model the delay only for
 * ordinary loads in BIOS code, so this loop, run interpreted (overlays,
 * PSX_FORCE_INTERP=1), fed SZ1 into VXY0 and the fighters became spikes.
 *
 * Covers: the loop shape (branch-delay-slot MFC2 into the target's first
 * instruction, and into the fall-through), in-block MFC2/CFC2/LW pairs in game
 * RAM, a branch in a load's delay slot (it sees the old value, its own slot
 * the new one), a same-register ALU write cancelling the load, LWL/LWR
 * forwarding, a load in a JAL delay slot with an interpreted callee (hidden
 * from its first instruction) and with a compiled callee (the return site is
 * not shown a stale value), SYSCALL in the slot, a register changed behind the
 * interpreter's back, and flush/discard.
 *
 * Sources are #included as in test_ws_cull_edge_interp.c; link-time dead-code
 * removal keeps the stub set small. */
#include "../src/gpu.c"
#include "../src/mod_immediate.c"
#include "../src/dirty_ram_interp.c"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- link stubs for code reachable from exec_one_fetched ------------------ */
uint64_t s_frame_count;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint32_t g_dirty_ram_code_gen;
uint64_t g_dispatch_static_hits;
uint32_t g_psx_mod_instruction_hooks;
/* The interpreter consults the optional override tier even in this isolated
 * culling fixture. No overrides are registered here. */
int (*g_psx_func_override_hook)(CPUState *, uint32_t);
uint32_t psx_mod_memory_address(const CPUState *cpu, uint32_t pc, uint32_t word, uint32_t address) {
    (void)cpu; (void)pc; (void)word; return address;
}
void psx_mod_instruction(CPUState *cpu, uint32_t pc, uint32_t instruction) {
    (void)cpu; (void)pc; (void)instruction; abort();
}
int      g_rfe_escape_pending;
int      g_exc_escape_reason;
int      g_psx_call_bail;
uint64_t g_psx_bail_first;
uint64_t g_psx_bail_resolved;
uint32_t i_stat;
uint32_t i_mask;
uint64_t psx_cycle_count;
/* Live RAM geometry (memory.c): retail 2 MiB, as the stub RAM below. */
uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;

static uint8_t test_ram[0x00200000u];

uint8_t *memory_get_ram_ptr(void) { return test_ram; }
uint8_t psx_read_byte(uint32_t a) { return test_ram[a & 0x1FFFFFu]; }
uint16_t psx_read_half(uint32_t a) {
    uint16_t v; memcpy(&v, test_ram + (a & 0x1FFFFEu), sizeof v); return v;
}
uint32_t psx_read_word(uint32_t a) {
    uint32_t v; memcpy(&v, test_ram + (a & 0x1FFFFCu), sizeof v); return v;
}
uint8_t psx_cyc_load_byte(CPUState *cpu, uint32_t a, uint32_t rt, uint32_t m) {
    (void)cpu; (void)rt; (void)m; return psx_read_byte(a);
}
uint32_t psx_cyc_lwc2_read(CPUState *cpu, uint32_t a) { (void)cpu; return psx_read_word(a); }
/* Every page is dirty (interpreted) except NONLOCAL_PAGE, which stands for
 * compiled code reached through psx_dispatch_call. */
#define NONLOCAL_FN 0x80030000u
int dirty_ram_is_dirty(uint32_t phys) { return ((phys & 0x1FFFFFFFu) >> 12) != ((NONLOCAL_FN & 0x1FFFFFFFu) >> 12); }
int mdec_recently_active(uint32_t f) { (void)f; return 0; }
void gte_execute(CPUState *cpu, uint32_t cmd) { (void)cpu; (void)cmd; }
static int precise_sign_valid, precise_sign;
static int previous_sign_valid, previous_sign;
int gte_nclip_zero_positive(int32_t mac0, uint32_t pc) {
    (void)mac0; (void)pc; return 0;
}
int gte_nclip_native_wide_sign(int32_t mac0, int* sign) {
    (void)mac0; *sign = precise_sign; return precise_sign_valid;
}
int gte_nclip_native_wide_previous_sign(int32_t mac0, int* sign) {
    (void)mac0; *sign = previous_sign; return previous_sign_valid;
}
void gte_precision_store_word(uint32_t a, uint8_t r) { (void)a; (void)r; }
/* GTE: MFC2/CFC2 read gte_data[]/gte_ctrl[]; MTC2 writes are logged. */
static uint32_t gte_data[32], gte_ctrl[32];
static uint32_t mtc2_log[16]; static uint8_t mtc2_reg[16]; static int mtc2_n;
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t r) { (void)cpu; return gte_ctrl[r & 31u]; }
uint32_t gte_read_data(CPUState *cpu, uint8_t r) { (void)cpu; return gte_data[r & 31u]; }
void gte_write_ctrl(CPUState *cpu, uint8_t r, uint32_t v) { (void)cpu; gte_ctrl[r & 31u] = v; }
void gte_write_data(CPUState *cpu, uint8_t r, uint32_t v) {
    (void)cpu;
    if (mtc2_n < 16) { mtc2_reg[mtc2_n] = r; mtc2_log[mtc2_n++] = v; }
}
int overlay_loader_call_native(CPUState *cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
void psx_bail_record(uint32_t ra, uint32_t sp, uint32_t pc, uint32_t gsp) {
    (void)ra; (void)sp; (void)pc; (void)gsp;
}
void psx_break(CPUState *cpu, uint32_t code, uint32_t pc) { (void)cpu; (void)code; (void)pc; }
int psx_break_enter_guest_vector(CPUState *cpu, uint32_t pc) {
    (void)cpu; (void)pc; abort(); /* this fixture executes no BREAK opcode */
}
void psx_check_interrupts(struct CPUState *cpu) { (void)cpu; }
/* Compiled callee: records what it was entered with. */
static uint32_t call_target, call_a0; static int call_n;
void psx_dispatch_call(CPUState *cpu, uint32_t t, uint32_t r) {
    (void)r; call_target = t; call_a0 = cpu->gpr[4]; call_n++;
}
void psx_fatal_halt(const char *reason) { fprintf(stderr, "fatal: %s\n", reason); exit(2); }
void psx_get_freeze_diag(uint64_t *a, uint32_t *b, int *c, int *d, uint64_t *e,
                         uint64_t *f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
int psx_get_in_exception(void) { return 0; }
int psx_interrupts_checked_at_current_cycle(uint32_t pc) { (void)pc; return 1; }
void psx_pgxp_alu(struct CPUState *c, uint32_t i, uint32_t r, uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)r; (void)a; (void)b;
}
void psx_pgxp_cop2(struct CPUState *c, uint32_t i, uint32_t v, uint32_t a) {
    (void)c; (void)i; (void)v; (void)a;
}
void psx_pgxp_load(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}
void psx_pgxp_muldiv(struct CPUState *c, uint32_t i, uint32_t h, uint32_t l,
                     uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)h; (void)l; (void)a; (void)b;
}
void psx_pgxp_store(struct CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}
void psx_rfe_mark_escape(void) {}
static uint32_t syscall_a0; static int syscall_n;
int psx_syscall(CPUState *cpu, uint32_t code) {
    (void)code; syscall_a0 = cpu->gpr[4]; syscall_n++; cpu->pc += 4u; return 0;
}


/* ---- harness --------------------------------------------------------------- */
static int failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

enum { ZERO = 0, A0 = 4, T0 = 8, T1 = 9, T2 = 10, T3 = 11, T8 = 24, T9 = 25 };
enum { VXY0 = 0, SZ1 = 17, CTRL_H = 26 };
#define CODE 0x80020000u /* game RAM, outside the old BIOS-only window */
#define DATA 0x80040000u

static uint32_t i_type(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t r_type(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t fn) {
    return (rs << 21) | (rt << 16) | (rd << 11) | fn;
}
static uint32_t cop2(uint32_t op, uint32_t rt, uint32_t rd) {
    return (0x12u << 26) | (op << 21) | (rt << 16) | (rd << 11);
}
#define NOP            0u
#define MFC2(rt, rd)   cop2(0x00u, rt, rd)
#define CFC2(rt, rd)   cop2(0x02u, rt, rd)
#define MTC2(rt, rd)   cop2(0x04u, rt, rd)
#define LW(rt, off, b) i_type(0x23u, b, rt, (uint16_t)(off))
#define LWR(rt, off, b) i_type(0x26u, b, rt, (uint16_t)(off))
#define ADDIU(rt, rs, v) i_type(0x09u, rs, rt, (uint16_t)(v))
#define MOVE(rd, rs)   r_type(rs, ZERO, rd, 0x21u)    /* addu rd, rs, zero */
#define BGTZ(rs, off)  i_type(0x07u, rs, 0, (uint16_t)(off))
#define BEQ(rs, rt, off) i_type(0x04u, rs, rt, (uint16_t)(off))
#define JAL(t)         ((0x03u << 26) | (((t) >> 2) & 0x03FFFFFFu))
#define JR(rs)         r_type(rs, 0, 0, 0x08u)
#define SYSCALL        0x0000000Cu

static void put_word(uint32_t addr, uint32_t v) { memcpy(test_ram + (addr & 0x1FFFFFu), &v, 4); }
static void put_code(uint32_t addr, const uint32_t *w, int n) {
    for (int i = 0; i < n; i++) put_word(addr + 4u * (uint32_t)i, w[i]);
}

/* Steps the interpreter from `pc` like dirty_ram_dispatch's loop until pc ==
 * stop or `max` instructions; returns the final pc. */
static uint32_t run(CPUState *cpu, uint32_t pc, uint32_t stop, int max) {
    for (int i = 0; i < max && pc != stop; i++) {
        uint32_t next = 0;
        cpu->pc = pc;
        int transfer = exec_one(cpu, pc, &next);
        pc = transfer ? cpu->pc : next;
    }
    return pc;
}

static void reset(CPUState *cpu) {
    memset(cpu, 0, sizeof *cpu);
    memset(gte_data, 0, sizeof gte_data);
    memset(gte_ctrl, 0, sizeof gte_ctrl);
    mtc2_n = 0; call_n = 0; syscall_n = 0;
    dirty_ram_ld_delay_discard();
}

/* Tekken 2's vertex loop: t8 walks three packed XY words, SZ1 holds a depth. */
static void test_vertex_loop(void) {
    CPUState cpu; reset(&cpu);
    const uint32_t L = CODE;
    const uint32_t code[] = {
        MTC2(T0, VXY0),        /* L+0  sees the OLD t0                   */
        LW(T0, 0, T8),         /* L+4                                    */
        ADDIU(T8, T8, 4),      /* L+8  (the lw's delay slot)             */
        ADDIU(T9, T9, -1),     /* L+12                                   */
        BGTZ(T9, -5),          /* L+16 -> L                              */
        MFC2(T0, SZ1),         /* L+20 delay slot                        */
        MOVE(T1, T0),          /* L+24 fall-through: mfc2's delay slot   */
        MOVE(T2, T0),          /* L+28 sees SZ1                          */
        NOP,
    };
    put_code(L, code, 9);
    put_word(DATA + 0, 0x00100010u);
    put_word(DATA + 4, 0x00200020u);
    put_word(DATA + 8, 0x00300030u);
    gte_data[SZ1] = 18645u;               /* the depth from the bisect */
    cpu.gpr[T0] = 0x00080008u;
    cpu.gpr[T8] = DATA;
    cpu.gpr[T9] = 3;
    uint32_t end = run(&cpu, L, L + 32, 64);
    CHECK(end == L + 32, "loop ended at 0x%08X", end);
    CHECK(mtc2_n == 3, "%d MTC2 writes, want 3", mtc2_n);
    const uint32_t want[3] = { 0x00080008u, 0x00100010u, 0x00200020u };
    for (int i = 0; i < 3 && i < mtc2_n; i++)
        CHECK(mtc2_log[i] == want[i],
              "iteration %d: VXY0 = 0x%08X, want 0x%08X (SZ1 is %u)", i,
              mtc2_log[i], want[i], gte_data[SZ1]);
    CHECK(cpu.gpr[T1] == 0x00300030u,
          "fall-through delay slot saw t0 = 0x%08X, want the lw value 0x00300030",
          cpu.gpr[T1]);
    CHECK(cpu.gpr[T2] == 18645u, "after the delay slot t0 = %u, want SZ1",
          cpu.gpr[T2]);
    CHECK(cpu.gpr[T0] == 18645u, "t0 = %u, want SZ1", cpu.gpr[T0]);
}

/* In-block pairs in game RAM: the next instruction sees the old value, the one
 * after sees the new value. */
static void check_pair(const char *what, uint32_t load) {
    CPUState cpu; reset(&cpu);
    const uint32_t code[] = { load, MOVE(T1, T0), MOVE(T2, T0), NOP };
    put_code(CODE, code, 4);
    put_word(DATA, 0x12345678u);
    gte_data[SZ1] = 0x12345678u;
    gte_ctrl[CTRL_H] = 0x12345678u;
    cpu.gpr[T0] = 0xAAAA5555u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 12, 8);
    CHECK(cpu.gpr[T1] == 0xAAAA5555u, "%s: delay slot saw 0x%08X, want the old value",
          what, cpu.gpr[T1]);
    CHECK(cpu.gpr[T2] == 0x12345678u, "%s: next saw 0x%08X, want the loaded value",
          what, cpu.gpr[T2]);
}

static void test_pairs(void) {
    check_pair("mfc2", MFC2(T0, SZ1));
    check_pair("cfc2", CFC2(T0, CTRL_H));
    check_pair("lw", LW(T0, 0, T8));
}

/* lw t0; beq t0,zero,+2; move t1,t0: the branch reads the old t0 (0, taken),
 * its own delay slot the loaded one. */
static void test_branch_in_slot(void) {
    CPUState cpu; reset(&cpu);
    const uint32_t code[] = { LW(T0, 0, T8), BEQ(T0, ZERO, 2), MOVE(T1, T0),
                              ADDIU(T3, ZERO, 1), NOP, NOP };
    put_code(CODE, code, 6);
    put_word(DATA, 7u);
    cpu.gpr[T8] = DATA;
    uint32_t end = run(&cpu, CODE, CODE + 16, 8);
    CHECK(end == CODE + 16, "beq on the old t0 (0) should be taken, ended 0x%08X", end);
    CHECK(cpu.gpr[T3] == 0u, "beq fell through");
    CHECK(cpu.gpr[T1] == 7u, "branch delay slot saw t0 = %u, want 7", cpu.gpr[T1]);
}

/* SCPH-5501 hardware results (05DLY): the immediate write wins; two loads
 * preserve the original old operand in the second load's delay slot.
 * https://psx.wiki/tests/raw/20260928T0000Z-5501a-card-h/cpu/05DLY.txt */
static void test_conflicts(void) {
    CPUState cpu; reset(&cpu);
    const uint32_t code[] = { LW(T0, 0, T8), ADDIU(T0, ZERO, 5), MOVE(T1, T0), NOP };
    put_code(CODE, code, 4);
    put_word(DATA, 0x1000u);
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 12, 8);
    CHECK(cpu.gpr[T1] == 5u && cpu.gpr[T0] == 5u,
          "load vs addiu in its slot: t0 = 0x%X, want the write (5)", cpu.gpr[T0]);

    reset(&cpu);
    const uint32_t chain[] = { LW(T0, 0, T8), LW(T0, 4, T8),
                                MOVE(T1, T0), MOVE(T2, T0), NOP };
    put_code(CODE, chain, 5);
    put_word(DATA, 2u); put_word(DATA + 4, 3u);
    cpu.gpr[T0] = 1u; cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 16, 8);
    CHECK(cpu.gpr[T1] == 1u && cpu.gpr[T2] == 3u && cpu.gpr[T0] == 3u,
          "two loads: delay=%u after=%u final=%u, want 1/3/3",
          cpu.gpr[T1], cpu.gpr[T2], cpu.gpr[T0]);

    /* lw t0 then lwr t0 at a partial offset: the merge base must be the lw's
     * value (forwarded), not the old t0. */
    reset(&cpu);
    const uint32_t lwlr2[] = { LW(T0, 0, T8), LWR(T0, 5, T8),
                                MOVE(T1, T0), MOVE(T2, T0), NOP };
    put_code(CODE, lwlr2, 5);
    put_word(DATA, 0x11223344u);
    put_word(DATA + 4, 0xAABBCCDDu);
    cpu.gpr[T0] = 0x99999999u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 16, 8);
    CHECK(cpu.gpr[T0] == 0x11AABBCCu && cpu.gpr[T1] == 0x99999999u &&
          cpu.gpr[T2] == 0x11AABBCCu,
          "lwr after lw merged into 0x%08X, want 0x11AABBCC (forwarded base)",
          cpu.gpr[T0]);
}

/* jal f; lw a0 in the slot. Interpreted callee: its first instruction sees the
 * old a0, its second the loaded one. Compiled callee: the call returns inline
 * and the return site must not be shown the old a0. */
static void test_calls(void) {
    CPUState cpu; reset(&cpu);
    const uint32_t F = CODE + 0x100u;
    const uint32_t caller[] = { JAL(F), LW(A0, 0, T8), MOVE(T3, A0), NOP };
    const uint32_t callee[] = { MOVE(T1, A0), MOVE(T2, A0), JR(31), NOP };
    put_code(CODE, caller, 4);
    put_code(F, callee, 4);
    put_word(DATA, 0x4444u);
    cpu.gpr[A0] = 0x1111u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 12, 16);
    CHECK(cpu.gpr[T1] == 0x1111u, "interpreted callee's first instruction saw a0 = 0x%X, want 0x1111",
          cpu.gpr[T1]);
    CHECK(cpu.gpr[T2] == 0x4444u, "callee's second instruction saw a0 = 0x%X", cpu.gpr[T2]);
    CHECK(cpu.gpr[T3] == 0x4444u, "return site saw a0 = 0x%X", cpu.gpr[T3]);

    reset(&cpu);
    const uint32_t caller2[] = { JAL(NONLOCAL_FN), LW(A0, 0, T8), MOVE(T3, A0), NOP };
    put_code(CODE, caller2, 4);
    cpu.gpr[A0] = 0x1111u;
    cpu.gpr[T8] = DATA;
    uint32_t end = run(&cpu, CODE, CODE + 12, 8);
    CHECK(call_n == 1 && call_target == NONLOCAL_FN, "compiled callee not called (%d)", call_n);
    CHECK(call_a0 == 0x4444u, "compiled callee entered with a0 = 0x%X, want the loaded value",
          call_a0);
    CHECK(end == CODE + 12 && cpu.gpr[T3] == 0x4444u,
          "return site saw a0 = 0x%X (stale delay-slot value?)", cpu.gpr[T3]);
}

/* SYSCALL in a load's delay slot: the exception drains the pipeline. */
static void test_syscall_in_slot(void) {
    CPUState cpu; reset(&cpu);
    const uint32_t code[] = { LW(A0, 0, T8), SYSCALL, NOP };
    put_code(CODE, code, 3);
    put_word(DATA, 0x5555u);
    cpu.gpr[A0] = 0x1u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 8, 2);
    CHECK(syscall_n == 1 && syscall_a0 == 0x5555u,
          "syscall in the delay slot saw a0 = 0x%X, want 0x5555", syscall_a0);
}

/* A register changed by something else (compiled code, a handler) before the
 * slot runs: no stale value is put back. Flush and discard end the window. */
static void test_stale_and_flush(void) {
    CPUState cpu; reset(&cpu);
    const uint32_t code[] = { LW(T0, 0, T8), MOVE(T1, T0), NOP };
    put_code(CODE, code, 3);
    put_word(DATA, 0x2222u);
    cpu.gpr[T0] = 0x1u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 4, 1);
    CHECK(cpu.gpr[T0] == 0x2222u, "the load's register is written at once (0x%X)", cpu.gpr[T0]);
    cpu.gpr[T0] = 0x3333u;                      /* changed behind our back */
    run(&cpu, CODE + 4, CODE + 8, 1);
    CHECK(cpu.gpr[T1] == 0x3333u && cpu.gpr[T0] == 0x3333u,
          "stale window: t1 = 0x%X t0 = 0x%X, want 0x3333", cpu.gpr[T1], cpu.gpr[T0]);

    reset(&cpu);
    cpu.gpr[T0] = 0x1u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 4, 1);
    dirty_ram_ld_delay_flush(&cpu);             /* hand-off to compiled code */
    run(&cpu, CODE + 4, CODE + 8, 1);
    CHECK(cpu.gpr[T1] == 0x2222u, "after flush the slot saw 0x%X", cpu.gpr[T1]);

    reset(&cpu);
    cpu.gpr[T0] = 0x1u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 4, 1);
    dirty_ram_ld_delay_discard();               /* savestate restore */
    run(&cpu, CODE + 4, CODE + 8, 1);
    CHECK(cpu.gpr[T1] == 0x2222u, "after discard the slot saw 0x%X", cpu.gpr[T1]);

    /* Entering at a different PC than the delay slot: no window. */
    reset(&cpu);
    const uint32_t code2[] = { LW(T0, 0, T8), NOP, MOVE(T1, T0), NOP };
    put_code(CODE, code2, 4);
    cpu.gpr[T0] = 0x1u;
    cpu.gpr[T8] = DATA;
    run(&cpu, CODE, CODE + 4, 1);
    run(&cpu, CODE + 8, CODE + 12, 1);
    CHECK(cpu.gpr[T1] == 0x2222u, "a jump past the slot saw 0x%X", cpu.gpr[T1]);
}

#ifndef LOAD_PROVIDER_CONTROL
static void test_shared_provider(void) {
    CPUState cpu, saved; DirtyRamLoadDelay window;
    reset(&cpu);
    const uint32_t code[] = { MOVE(T1, T0), MOVE(T2, T0), NOP };
    put_code(CODE, code, 3);
    cpu.gpr[T0] = 1u;
    dirty_ram_ld_delay_stage(&cpu, T0, 2u, CODE);
    CHECK(cpu.gpr[T0] == 2u, "host boundary must see the loaded GPR");
    run(&cpu, CODE, CODE + 8, 4);
    CHECK(cpu.gpr[T1] == 1u && cpu.gpr[T2] == 2u,
          "compiled stage to interpreter: delay=%u after=%u", cpu.gpr[T1], cpu.gpr[T2]);

    reset(&cpu); cpu.gpr[T0] = 1u;
    dirty_ram_ld_delay_stage(&cpu, T0, 2u, CODE);
    saved = cpu; dirty_ram_ld_delay_save(&window);
    uint64_t cookie = dirty_ram_ld_delay_begin(&cpu, CODE, code[0]);
    CHECK(cookie && cpu.gpr[T0] == 1u, "native successor sees the old operand");
    cpu.gpr[T1] = cpu.gpr[T0];
    dirty_ram_ld_delay_end(&cpu, cookie, code[0]);
    CHECK(cpu.gpr[T1] == 1u && cpu.gpr[T0] == 2u, "native successor retires the load");

    cpu = saved; dirty_ram_ld_delay_restore_from_state(&window);
    dirty_ram_ld_delay_discard(); /* normal post-load resync keeps restored data */
    dirty_ram_ld_delay_end(&cpu, cookie, ADDIU(T0, ZERO, 3));
    cookie = dirty_ram_ld_delay_begin(&cpu, CODE, code[0]);
    CHECK(cookie && cpu.gpr[T0] == 1u, "stale host cookie must not consume a restored window");
    dirty_ram_ld_delay_end(&cpu, cookie, code[0]);
    CHECK(cpu.gpr[T0] == 2u, "restored window retires on its own stream");

    reset(&cpu); cpu.gpr[T0] = 1u;
    dirty_ram_ld_delay_stage(&cpu, T0, 2u, CODE);
    dirty_ram_ld_delay_flush(&cpu); /* actual IRQ/thread delivery owns this flush */
    cookie = dirty_ram_ld_delay_begin(&cpu, CODE, code[0]);
    CHECK(!cookie && cpu.gpr[T0] == 2u, "a drained pipeline exposes the loaded value");
}
#endif

int main(void) {
    test_vertex_loop();
    test_pairs();
    test_branch_in_slot();
    test_conflicts();
    test_calls();
    test_syscall_in_slot();
    test_stale_and_flush();
#ifndef LOAD_PROVIDER_CONTROL
    test_shared_provider();
#endif
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS interpreter load delay: MFC2/CFC2/LW in game RAM, branch delay "
         "slots into targets and fall-through, calls, conflicts, flush");
    return 0;
}
