/* exception-context regression: an exception return resumes the context the TCB holds.
 *
 * Discworld II (SCUS-94605) waits in VSync() with a live stack frame. Its
 * VBlank handler copies the saved context out of the TCB, rewrites the TCB's
 * EPC and sp to start a task on another stack, and calls B0(17h)
 * ReturnFromException. The task later puts the saved context back and calls
 * ReturnFromException again, outside any exception. On hardware each return
 * resumes at the TCB's EPC with every register from the TCB (PSX-SPX, BIOS
 * exception handling). The runtime took the registers but kept running the
 * interrupted compiled block, so VSync read a frame it never built, timed out
 * and returned to PC 0.
 *
 * Production interrupts.c and traps.c deliver the interrupt and run the
 * scheduler; rfe_guest_fixture.h is the guest. Two cases, in one run:
 *   - a plain handler: the interrupted block continues with every register
 *     it had (the interrupted context);
 *   - a redirecting handler: the task runs at the new EPC on the new stack
 *     with the other registers of the interrupted context, the interrupted
 *     block does not run meanwhile, and the second return resumes it on its
 *     own frame. */
#include "rfe_guest_fixture.h"

#define WAIT_ENTRY   0x80010000u /* the waiting function */
#define WAIT_LOOP    0x80010020u /* its loop head, a block leader */
#define HANDLER_PC   0x80020000u /* the game's interrupt handler */
#define TASK_PC      0x80030000u /* started by the redirecting handler */
#define EXIT_PC      0x80040000u

#define MAIN_STACK    0x801FFF38u
#define HANDLER_STACK 0x8006ACB4u
#define TASK_STACK    0x801FE7E0u
#define SAVE_AREA     0x00070000u /* the game's copy of the saved context */
#define VCOUNT        0x00078000u /* frames the handler counted */

#define WAIT_FRAMES    8u
#define WAIT_TIMEOUT   0x8000u
#define REDIRECT_FIRST 3u /* the handler redirects on these interrupts */
#define REDIRECT_AGAIN 6u

typedef struct { uint32_t gpr[32], hi, lo, sr; } Context;

static Context interrupted;       /* at the poll that may take the interrupt */
static int interrupted_live;      /* the block after that poll has not run yet */
static uint64_t entries_at_poll;
static unsigned polls, irqs, plain_resumes, task_runs, task_resumes, exits;
static uint32_t wait_frame;       /* sp inside the waiting function */

static void capture(const CPUState *cpu, Context *c) {
    memcpy(c->gpr, cpu->gpr, sizeof(c->gpr));
    c->hi = cpu->hi;
    c->lo = cpu->lo;
    c->sr = cpu->cop0[12];
}

/* Every register an exception return restores. k0 and k1 belong to the
 * kernel and are not part of a thread's context. */
static void check_resumed(const CPUState *cpu, const Context *want,
                          const char *what, uint32_t want_sp) {
    for (int i = 1; i < 32; i++) {
        uint32_t expect = (i == 29) ? want_sp : want->gpr[i];
        if (i == 26 || i == 27) continue;
        if (cpu->gpr[i] != expect)
            FX_FAIL("%s: GPR %d is 0x%08X, the saved context has 0x%08X",
                    what, i, (unsigned)cpu->gpr[i], (unsigned)expect);
    }
    /* The authored kernel's RFE explicitly writes k0 with its resume PC.
     * It is not a user register for the runtime's same-thread restore. */
    if(cpu->gpr[26]!=fx_read(FX_TCB_EPC))
        FX_FAIL("kernel scratch k0=%08X lost guest RFE value %08X",cpu->gpr[26],fx_read(FX_TCB_EPC));
    if (cpu->hi != want->hi || cpu->lo != want->lo)
        FX_FAIL("%s: HI/LO %08X/%08X, the saved context has %08X/%08X", what,
                (unsigned)cpu->hi, (unsigned)cpu->lo, (unsigned)want->hi, (unsigned)want->lo);
    if (cpu->cop0[12] != want->sr)
        FX_FAIL("%s: SR is 0x%08X, want 0x%08X", what,
                (unsigned)cpu->cop0[12], (unsigned)want->sr);
}

/* VSync-style wait: count down a timeout on the stack until the handler has
 * counted WAIT_FRAMES. ra and the timeout live in the frame. */
static void g_wait(CPUState *cpu) {
    uint32_t entry = cpu->pc;
    cpu->pc = 0;
    if (entry == WAIT_LOOP) {
        /* Entered by dispatch: a return from exception resumed the loop. */
        FX_CHECK(interrupted_live);
        check_resumed(cpu, &interrupted, "second return", interrupted.gpr[29]);
        interrupted_live = 0;
        task_resumes++;
        goto after_poll;
    }
    FX_CHECK(entry == WAIT_ENTRY);
    cpu->gpr[29] -= 0x20u;
    wait_frame = cpu->gpr[29];
    fx_write(cpu->gpr[29] + 16u, WAIT_TIMEOUT);
    fx_write(cpu->gpr[29] + 24u, cpu->gpr[31]);
    for (int i = 1; i < 26; i++) cpu->gpr[i] = 0x51000000u + (uint32_t)i;
    cpu->gpr[28] = 0x8006BEF0u;
    cpu->gpr[30] = 0x801FFFF8u;
    cpu->gpr[31] = 0x8004B210u;
    cpu->hi = 0x12345678u;
    cpu->lo = 0x9ABCDEF0u;

block_wait_loop:
    /* The device: one vertical blank every third pass, 1000 cycles a pass. */
    psx_cycle_count += 1000u;
    if (++polls % 3u == 0u) i_stat |= 1u;
    capture(cpu, &interrupted);
    interrupted_live = 1;
    entries_at_poll = fx_exception_entries;
    psx_check_interrupts_at(cpu, WAIT_LOOP);
    /* The block continues here only if the interrupted context is back. */
    check_resumed(cpu, &interrupted, "interrupted block", interrupted.gpr[29]);
    interrupted_live = 0;
    if (fx_exception_entries != entries_at_poll) plain_resumes++;
after_poll:
    FX_CHECK(cpu->gpr[29] == wait_frame);
    {
        uint32_t timeout = fx_read(cpu->gpr[29] + 16u);
        fx_write(cpu->gpr[29] + 16u, timeout - 1u);
        if (timeout == 0u) FX_FAIL("the wait timed out: it ran on a frame it never built");
    }
    if (fx_read(VCOUNT) < WAIT_FRAMES) goto block_wait_loop;

    cpu->gpr[31] = fx_read(cpu->gpr[29] + 24u);
    cpu->gpr[29] += 0x20u;
    cpu->pc = cpu->gpr[31];
}

/* The game's handler. It runs on its own stack and keeps no register. */
static void g_handler(CPUState *cpu) {
    cpu->pc = 0;
    FX_CHECK((fx_read(FX_TCB_CAUSE) & 0x7Cu) == 0u); /* an interrupt */
    for (int i = 1; i < 32; i++) cpu->gpr[i] = 0xDEAD0000u + (uint32_t)i;
    cpu->hi = cpu->lo = 0xDEADDEADu;
    cpu->gpr[29] = HANDLER_STACK;
    i_stat &= ~1u; /* acknowledge */
    fx_write(VCOUNT, fx_read(VCOUNT) + 1u);
    irqs++;
    if (irqs == REDIRECT_FIRST || irqs == REDIRECT_AGAIN) {
        /* Keep the interrupted context and send the thread to the task. */
        for (uint32_t off = 0x08u; off <= 0x98u; off += 4u)
            fx_write(SAVE_AREA + off, fx_read(FX_TCB + off));
        fx_write(FX_TCB_EPC, TASK_PC);
        fx_write(FX_TCB_REG(29), TASK_STACK);
    }
    cpu->gpr[31] = HANDLER_PC + 8u;
    cpu->pc = FX_K_RFE; /* ReturnFromException(); it does not come back */
}

/* The task: runs on the interrupted thread, on its own stack. It restores the
 * saved context and returns to it with ReturnFromException. */
static void g_task(CPUState *cpu) {
    Context saved;
    cpu->pc = 0;
    FX_CHECK(interrupted_live); /* the interrupted block must not have run */
    for (int i = 0; i < 32; i++) saved.gpr[i] = fx_read(SAVE_AREA + 0x08u + 4u * (uint32_t)i);
    saved.hi = fx_read(SAVE_AREA + 0x8Cu);
    saved.lo = fx_read(SAVE_AREA + 0x90u);
    saved.sr = interrupted.sr;
    check_resumed(cpu, &saved, "redirected return", TASK_STACK);
    for (int i = 1; i < 32; i++)
        if (i != 26 && i != 27) FX_CHECK(saved.gpr[i] == interrupted.gpr[i]);
    task_runs++;
    fx_write(cpu->gpr[29] - 8u, 0x7A5C0000u + task_runs); /* work on its own stack */
    for (uint32_t off = 0x08u; off <= 0x98u; off += 4u)
        fx_write(FX_TCB + off, fx_read(SAVE_AREA + off));
    cpu->gpr[31] = TASK_PC + 8u;
    cpu->pc = FX_K_RFE;
}

static void g_exit(CPUState *cpu) {
    cpu->pc = 0;
    exits++;
}

static const FxEntry entries[] = {
    { FX_VECTOR, fx_k_exception }, { FX_K_RET, fx_k_ret },
    { FX_K_RFE, fx_k_return_from_exception },
    { WAIT_ENTRY, g_wait }, { WAIT_LOOP, g_wait },
    { HANDLER_PC, g_handler }, { TASK_PC, g_task }, { EXIT_PC, g_exit },
};

int main(void) {
    CPUState cpu;
    fx_boot(&cpu, entries, sizeof(entries) / sizeof(entries[0]),
            HANDLER_PC, WAIT_ENTRY, MAIN_STACK, EXIT_PC);
    i_mask = 1u; /* vertical blank */

    psx_scheduler_run(&cpu);

    FX_CHECK(exits == 1u);                    /* the wait returned to its caller */
    FX_CHECK(cpu.gpr[29] == MAIN_STACK);      /* through its own epilogue */
    FX_CHECK(fx_read(VCOUNT) == WAIT_FRAMES && irqs == WAIT_FRAMES);
    FX_CHECK(task_runs == 2u && task_resumes == 2u);
    FX_CHECK(plain_resumes == WAIT_FRAMES - 2u);
    FX_CHECK(fx_read(TASK_STACK - 8u) == 0x7A5C0002u);
    FX_CHECK(fx_max_dispatch_depth <= 3);
    printf("PASS: %u interrupts, %u plain resumes, %u redirected returns, depth %d\n",
           irqs, plain_resumes, task_runs, fx_max_dispatch_depth);
    return 0;
}
