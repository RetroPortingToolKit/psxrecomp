/* A BREAK enters the exception vector only when the guest owns that vector.
 * The same checks must reject four wrong implementations, so a pass
 * is a statement about results, not about the function being present. */
#include <stdio.h>
#include <string.h>
#include "psx_break_vector.h"

typedef int (*EnterFn)(CPUState *, uint32_t, int);

static uint32_t vector[3];
static unsigned stray_reads;
static uint32_t read_vector(uint32_t addr) {
    if (addr < 0x80000080u || addr > 0x80000088u || (addr & 3u)) { ++stray_reads; return 0; }
    return vector[(addr - 0x80000080u) >> 2];
}
static void set_vector(uint32_t w0, uint32_t w1, uint32_t w2) {
    vector[0] = w0; vector[1] = w1; vector[2] = w2;
}

static CPUState state(void) {
    CPUState c;
    memset(&c, 0, sizeof c);
    for (unsigned i = 0; i < 32; i++) c.gpr[i] = 0x1000 + i;
    c.pc = 0x8001015Cu; c.read_word = read_vector;
    c.cop0[12] = 0x40000401; c.cop0[13] = 0xf000057c; c.cop0[14] = 0x1234;
    return c;
}

/* 0 when the call refuses and leaves the CPU state alone. */
static int refused(EnterFn enter, int in_exception, uint32_t extra_sr) {
    CPUState c = state(), e;
    c.cop0[12] |= extra_sr; e = c;
    return enter(&c, 0x80010160u, in_exception) != 0 || memcmp(&c, &e, sizeof c);
}

/* 0 when the call enters the vector with exactly the expected state. */
static int entered(EnterFn enter, uint32_t cause) {
    CPUState c = state(), e = c;
    e.pc = 0x80000080u; e.cop0[14] = 0x80010160u;
    e.cop0[12] = 0x40000404; e.cop0[13] = cause;
    return enter(&c, 0x80010160u, 0) != 1 || memcmp(&c, &e, sizeof c);
}

/* 0 when every expectation holds, else the number of the first that fails. */
static int verdict(EnterFn enter) {
    /* The guest's stub: lui k0,0x8001 ; ori k0,k0,0x334c ; jr k0. */
    set_vector(0x3c1a8001u, 0x375a334cu, 0x03400008u);
    if (entered(enter, 0x70000524u)) return 1;      /* code 9, BD clear, rest kept */
    if (refused(enter, 1, 0)) return 2;             /* inside the handler window */
    if (refused(enter, 0, 0x00400000u)) return 3;   /* SR.BEV */
    /* The same target built with addiu: lui at,0x8002 ; addiu at,at,-0x7000. */
    set_vector(0x3c018002u, 0x24219000u, 0x00200008u);
    if (entered(enter, 0x70000524u)) return 4;
    /* The BIOS's stub: lui k0,0 ; addiu k0,k0,0xc80 ; jr k0. */
    set_vector(0x3c1a0000u, 0x275a0c80u, 0x03400008u);
    if (refused(enter, 0, 0)) return 5;
    /* No vector, a ROM target, a jump through another register, a plain j. */
    set_vector(0u, 0u, 0u);
    if (refused(enter, 0, 0)) return 6;
    set_vector(0x3c1abfc0u, 0x375a0180u, 0x03400008u);
    if (refused(enter, 0, 0)) return 7;
    set_vector(0x3c1a8001u, 0x375a334cu, 0x03600008u);
    if (refused(enter, 0, 0)) return 8;
    set_vector(0x08004cd3u, 0u, 0u);
    if (refused(enter, 0, 0)) return 9;
    return 0;
}

/* Wrong implementations. Each must fail one numbered expectation above. */
static int old_exit_only(CPUState *c, uint32_t pc, int in_exc) {
    (void)c; (void)pc; (void)in_exc;
    return 0;                                   /* the behaviour before this change */
}
static int wrong_epc(CPUState *c, uint32_t pc, int in_exc) {
    return psx_break_vector_enter(c, pc + 4u, in_exc);
}
static int syscall_code(CPUState *c, uint32_t pc, int in_exc) {
    int r = psx_break_vector_enter(c, pc, in_exc);
    if (r) c->cop0[13] = (c->cop0[13] & ~0x7Cu) | (8u << 2);
    return r;
}
static int any_vector(CPUState *c, uint32_t pc, int in_exc) {
    uint32_t sr = c->cop0[12];
    if ((sr & 0x00400000u) || in_exc) return 0;
    if (!psx_break_vector_enter(c, pc, in_exc)) {
        c->cop0[14] = pc;
        c->cop0[13] = (c->cop0[13] & ~(0x80000000u | 0x7Cu)) | (9u << 2);
        c->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);
        c->pc = 0x80000080u;
    }
    return 1;
}

static int expect(const char *name, EnterFn enter, int want) {
    int got = verdict(enter);
    if (got == want) return 0;
    printf("FAIL: %s: first failed expectation %d, wanted %d\n", name, got, want);
    return 1;
}

int main(void) {
    int bad = expect("psx_break_vector_enter", psx_break_vector_enter, 0)
            + expect("old exit only", old_exit_only, 1)
            + expect("wrong EPC", wrong_epc, 1)
            + expect("syscall code", syscall_code, 1)
            + expect("any vector", any_vector, 5);
    if (stray_reads) { printf("FAIL: %u reads outside the three vector words\n", stray_reads); bad++; }
    if (bad) return 1;
    puts("PASS: a BREAK enters only a vector the guest owns; four wrong results are rejected");
    return 0;
}
