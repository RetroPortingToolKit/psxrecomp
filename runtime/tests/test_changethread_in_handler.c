/* A ChangeThread called inside an interrupt handler switches threads
 * at the call, as the kernel does.
 *
 * V-Rally 2 (SLUS-01003) runs its track set-up in a second thread. Its DMA
 * interrupt handler preempts that thread: it copies the interrupted context
 * out of the thread's control block and calls ChangeThread(main). Later the
 * main thread writes the copy back and calls ChangeThread to the set-up
 * thread, which continues where the interrupt took it. The runtime returned
 * from the handler's call into the interrupted thread instead, the thread ran
 * on, and the main thread's restore ran its work a second time.
 *
 * The kernel's steps for SYS(03h), in a handler or out of one (SCPH-1001:
 * exception entry 0xC80, syscall handler 0x1A00, function 3 at 0x1AAC,
 * ReturnFromException 0xF40):
 *   1. the registers at the SYSCALL go into the block the current-thread
 *      pointer names, over what the interrupt saved there;
 *   2. that block's saved EPC is the SYSCALL's address plus 4;
 *   3. that block's saved v0 becomes 1, and the pointer takes a1;
 *   4. the return loads every register from the block the pointer now names.
 * Function 3 does not look at the target's state word.
 *
 * Production interrupts.c and traps.c deliver the interrupt, handle the
 * syscall and run the scheduler; rfe_guest_fixture.h is the guest. The driver
 * builds this file three times:
 *
 *   as it is: four interrupts hit the worker thread. The first handler
 *     returns plainly. The second calls ChangeThread to the main thread: the
 *     four steps are checked in the main thread, the worker must not have
 *     done another pass, and after the main thread writes the copy back the
 *     worker continues with the interrupted registers and does each pass
 *     once. The third calls ChangeThread to the worker itself, the current
 *     thread, and the fourth names an address outside the thread table: both
 *     stay a return to the interrupted code.
 *
 *   CHANGETHREAD_NO_RESTORE: the main thread switches back WITHOUT writing
 *     the copy back. The old block holds the handler's context, so the
 *     handler continues after its call, on its own stack, with v0 = 1.
 *
 *   CHANGETHREAD_NOT_RUNNABLE: the target block's state word says closed.
 *     The switch is made all the same.
 *
 * CHANGETHREAD_CONTROL is for sources from before the fix (no counters). */
#include <stdio.h>
#include <stdlib.h>

/* A failed check reports on stdout and leaves at once. On glibc the test
 * helper's link makes a write to stderr crash (rc -11), and a
 * control that fails must still print which check failed. */
#define FX_FAIL(...) do { \
    printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); puts(""); \
    fflush(stdout); _Exit(1); \
} while (0)
#include "rfe_guest_fixture.h"

#define WORK_ENTRY   0x80010000u /* the worker thread's function */
#define WORK_LOOP    0x80010020u /* its loop head, a block leader */
#define HANDLER_PC   0x80020000u /* the game's interrupt handler */
#define MAIN_RESUME  0x80030000u /* the main thread, after its own ChangeThread */
#define EXIT_PC      0x80040000u
#define SYSCALL_PC   0x80000650u /* the SYSCALL of the kernel's ChangeThread */
#define CT_RETURN    0x80002104u /* its return address: where the code that
                                  * called ChangeThread continues later */

#define TCB_WORKER    FX_TCB
#define TCB_MAIN      (FX_TCB + 0xC0u)
#define TCB_REG(t, i) ((t) + 0x08u + 4u * (uint32_t)(i))
#define TCB_EPC(t)    ((t) + 0x88u)
#define TCB_SR(t)     ((t) + 0x94u)
#define TCB_CAUSE(t)  ((t) + 0x98u)
#define NOT_A_BLOCK   0x00070000u /* an address outside the thread table */

#define WORK_STACK    0x801471F0u
#define MAIN_STACK    0x801FFDF8u
#define HANDLER_STACK 0x800BB9B8u
#define SAVE_AREA     0x00070000u /* the game's copy of the interrupted context */
#define WORK_COUNT    0x00078000u /* passes the worker has done: not repeatable */

#define PASSES         12u
#define IRQ_PLAIN      1u
#define IRQ_SWITCH     2u
#define IRQ_SELF       3u
#define IRQ_OUTSIDE    4u
#define TEST_FRAME     77u

typedef struct { uint32_t gpr[32], hi, lo, sr; } Context;

static Context interrupted;       /* at the poll that may take the interrupt */
static int interrupted_live;      /* the block after that poll has not run yet */
static uint64_t entries_at_poll;
static unsigned polls, irqs, plain_resumes, self_resumes, outside_resumes;
static unsigned main_runs, main_afters, worker_resumes, handler_continued, exits;
static uint32_t count_at_switch;  /* WORK_COUNT when the handler called ChangeThread */

void psx_changethread_in_handler_stats(uint32_t out[6]);
int psx_syscall(CPUState *cpu, uint32_t code);
#ifdef CHANGETHREAD_CONTROL
/* It must fail on what the guest sees, not at the link. */
void psx_changethread_in_handler_stats(uint32_t out[6]) { memset(out, 0, 6 * sizeof(out[0])); }
#endif

static void capture(const CPUState *cpu, Context *c) {
    memcpy(c->gpr, cpu->gpr, sizeof(c->gpr));
    c->hi = cpu->hi;
    c->lo = cpu->lo;
    c->sr = cpu->cop0[12];
}

/* Every register a return restores; k0 and k1 belong to the kernel. */
static void check_context(const CPUState *cpu, const Context *want, const char *what) {
    for (int i = 1; i < 32; i++) {
        if (i == 26 || i == 27) continue;
        if (cpu->gpr[i] != want->gpr[i])
            FX_FAIL("%s: GPR %d is 0x%08X, want 0x%08X", what, i,
                    (unsigned)cpu->gpr[i], (unsigned)want->gpr[i]);
    }
    if (cpu->hi != want->hi || cpu->lo != want->lo)
        FX_FAIL("%s: HI/LO %08X/%08X, want %08X/%08X", what, (unsigned)cpu->hi,
                (unsigned)cpu->lo, (unsigned)want->hi, (unsigned)want->lo);
    if (cpu->cop0[12] != want->sr)
        FX_FAIL("%s: SR is 0x%08X, want 0x%08X", what,
                (unsigned)cpu->cop0[12], (unsigned)want->sr);
}

/* The worker: PASSES passes, each counted once in guest memory. */
static void g_worker(CPUState *cpu) {
    uint32_t entry = cpu->pc;
    cpu->pc = 0;
    if (entry == WORK_LOOP) {
        /* Entered by dispatch: the main thread wrote the interrupted context
         * back and switched to this thread. */
        FX_CHECK(interrupted_live);
        check_context(cpu, &interrupted, "worker after the switch back");
        interrupted_live = 0;
        worker_resumes++;
        goto after_poll;
    }
    FX_CHECK(entry == WORK_ENTRY);
    for (int i = 1; i < 26; i++) cpu->gpr[i] = 0x51000000u + (uint32_t)i;
    cpu->gpr[28] = 0x8006BEF0u;
    cpu->gpr[30] = 0x801FFFF8u;
    cpu->hi = 0x12345678u;
    cpu->lo = 0x9ABCDEF0u;

block_work_loop:
    /* The device: one DMA interrupt every third pass. */
    psx_cycle_count += 1000u;
    if (++polls % 3u == 0u) i_stat |= 8u;
    cpu->gpr[8] = 0x77000000u + polls;       /* a register that changes per pass */
    capture(cpu, &interrupted);
    interrupted_live = 1;
    entries_at_poll = fx_exception_entries;
    psx_check_interrupts_at(cpu, WORK_LOOP);
    /* The block continues here only on the thread that was interrupted. */
    check_context(cpu, &interrupted, "interrupted block");
    interrupted_live = 0;
    if (fx_exception_entries != entries_at_poll) {
        if (irqs == IRQ_SWITCH)
            FX_FAIL("the handler's ChangeThread did not switch: the interrupted "
                    "thread ran on at once; irqs=%u polls=%u entries=%llu", irqs,
                    polls, (unsigned long long)fx_exception_entries);
        if (irqs == IRQ_SELF) self_resumes++;
        else if (irqs == IRQ_OUTSIDE) outside_resumes++;
        else plain_resumes++;
    }
after_poll:
    fx_write(WORK_COUNT, fx_read(WORK_COUNT) + 1u);
    if (fx_read(WORK_COUNT) < PASSES) goto block_work_loop;
    cpu->pc = cpu->gpr[31];
}

/* The kernel's ChangeThread as a guest reaches it: a0 = 3, a1 = the block. */
static void change_thread(CPUState *cpu, uint32_t tcb) {
    cpu->gpr[4] = 3u;
    cpu->gpr[5] = tcb;
    cpu->gpr[31] = CT_RETURN;
    cpu->pc = SYSCALL_PC;
    (void)psx_syscall(cpu, 0u);
}

/* The game's handler. It runs on its own stack and keeps no register. */
static void g_handler(CPUState *cpu) {
    cpu->pc = 0;
    FX_CHECK((fx_read(TCB_CAUSE(TCB_WORKER)) & 0x7Cu) == 0u); /* an interrupt */
    for (int i = 1; i < 32; i++) cpu->gpr[i] = 0xDEAD0000u + (uint32_t)i;
    cpu->hi = 0xDEAD1111u;
    cpu->lo = 0xDEAD2222u;
    cpu->gpr[29] = HANDLER_STACK;
    i_stat &= ~8u; /* acknowledge */
    irqs++;
    if (irqs == IRQ_SWITCH) {
        /* Keep the interrupted context, then give the processor to main. */
        for (uint32_t off = 0x08u; off <= 0x98u; off += 4u)
            fx_write(SAVE_AREA + 0x100u + off, fx_read(TCB_WORKER + off));
        count_at_switch = fx_read(WORK_COUNT);
        change_thread(cpu, TCB_MAIN);
        FX_FAIL("ChangeThread inside the handler came back to the handler at once");
    }
    if (irqs == IRQ_SELF) {
        change_thread(cpu, TCB_WORKER);
        FX_FAIL("ChangeThread to the current thread came back to the handler");
    }
    if (irqs == IRQ_OUTSIDE) {
        change_thread(cpu, NOT_A_BLOCK);
        FX_FAIL("ChangeThread to an address outside the table came back to the handler");
    }
    cpu->gpr[31] = HANDLER_PC + 8u;
    cpu->pc = FX_K_RFE; /* ReturnFromException() */
}

/* The main thread, entered at the address its own block holds. */
static void g_main(CPUState *cpu) {
    uint32_t entry = cpu->pc;
    cpu->pc = 0;
    FX_CHECK(entry == MAIN_RESUME);
    main_runs++;

    /* Step 4: every register comes from the block the pointer now names. */
    for (int i = 1; i < 32; i++) {
        uint32_t want = (i == 29) ? MAIN_STACK : (i == 2) ? 1u : 0x3A000000u + (uint32_t)i;
        if (i == 26 || i == 27) continue;
        if (cpu->gpr[i] != want)
            FX_FAIL("step 4: main's GPR %d is 0x%08X, its block has 0x%08X", i,
                    (unsigned)cpu->gpr[i], (unsigned)want);
    }
    if (cpu->hi != 0x3A00AAAAu || cpu->lo != 0x3A00BBBBu)
        FX_FAIL("step 4: main's HI/LO are %08X/%08X", (unsigned)cpu->hi, (unsigned)cpu->lo);
    if (cpu->cop0[12] != 0x00000401u)
        FX_FAIL("step 4: SR is 0x%08X after the return, want the popped 0x401",
                (unsigned)cpu->cop0[12]);

    /* Step 3: the pointer names main; the old block's saved v0 is 1. */
    if (fx_read(FX_PCB) != TCB_MAIN)
        FX_FAIL("step 3: the current-thread pointer is 0x%08X, want main's block",
                (unsigned)fx_read(FX_PCB));
    if (fx_read(TCB_REG(TCB_WORKER, 2)) != 1u)
        FX_FAIL("step 3: the old block's saved v0 is 0x%08X, want 1",
                (unsigned)fx_read(TCB_REG(TCB_WORKER, 2)));

    /* Step 2: the old block's EPC is past the SYSCALL. */
    if (fx_read(TCB_EPC(TCB_WORKER)) != SYSCALL_PC + 4u)
        FX_FAIL("step 2: the old block's EPC is 0x%08X, want 0x%08X",
                (unsigned)fx_read(TCB_EPC(TCB_WORKER)), (unsigned)(SYSCALL_PC + 4u));

    /* Step 1: the old block holds the registers at the SYSCALL, the
     * handler's, not the context the interrupt saved. */
    for (int i = 1; i < 32; i++) {
        uint32_t want = 0xDEAD0000u + (uint32_t)i;
        if (i == 2 || i == 26) continue;      /* v0: step 3; k0: the kernel's */
        if (i == 4) want = 3u;
        if (i == 5) want = TCB_MAIN;
        if (i == 29) want = HANDLER_STACK;
        if (i == 31) want = CT_RETURN;
        if (fx_read(TCB_REG(TCB_WORKER, i)) != want)
            FX_FAIL("step 1: the old block's GPR %d is 0x%08X, want the handler's 0x%08X",
                    i, (unsigned)fx_read(TCB_REG(TCB_WORKER, i)), (unsigned)want);
    }
    if (fx_read(TCB_WORKER + 0x8Cu) != 0xDEAD1111u || fx_read(TCB_WORKER + 0x90u) != 0xDEAD2222u)
        FX_FAIL("step 1: the old block's HI/LO are not the handler's");
    if ((fx_read(TCB_CAUSE(TCB_WORKER)) & 0x7Cu) != (8u << 2))
        FX_FAIL("step 1: the old block's Cause is 0x%08X, want exception code 8",
                (unsigned)fx_read(TCB_CAUSE(TCB_WORKER)));

    /* The switch was taken at the call: the worker did not do another pass. */
    FX_CHECK(interrupted_live);
    if (fx_read(WORK_COUNT) != count_at_switch)
        FX_FAIL("the interrupted thread ran %u more pass(es) before the switch",
                (unsigned)(fx_read(WORK_COUNT) - count_at_switch));

#ifdef CHANGETHREAD_NOT_RUNNABLE
    return; /* the closed block was taken; the start ends here */
#else
#ifndef CHANGETHREAD_NO_RESTORE
    /* The game's side: put the interrupted context back, then resume the worker. */
    for (uint32_t off = 0x08u; off <= 0x98u; off += 4u)
        fx_write(TCB_WORKER + off, fx_read(SAVE_AREA + 0x100u + off));
#endif
    cpu->gpr[29] = MAIN_STACK;
    change_thread(cpu, TCB_WORKER);          /* outside any exception */
    FX_FAIL("main's ChangeThread to the worker returned without a switch");
#endif
}

/* SYSCALL_PC + 4: the rest of the kernel's ChangeThread, `jr ra`. A thread
 * whose block still holds the context of its call resumes here. */
static void g_ct_tail(CPUState *cpu) { cpu->pc = cpu->gpr[31]; }

/* CT_RETURN: the code that called ChangeThread continues. */
static void g_ct_return(CPUState *cpu) {
    cpu->pc = 0;
    if (fx_read(FX_PCB) == TCB_MAIN) { /* the other thread has ended */
        main_afters++;
        return;
    }
    /* The worker's block held the handler's context at the call: the handler
     * continues after its call, with ChangeThread's result. */
#ifndef CHANGETHREAD_NO_RESTORE
    FX_FAIL("the worker resumed inside its handler though main wrote the copy back");
#endif
    if (cpu->gpr[2] != 1u)
        FX_FAIL("the handler continues with v0 = 0x%08X, want 1", (unsigned)cpu->gpr[2]);
    if (cpu->gpr[29] != HANDLER_STACK)
        FX_FAIL("the handler continues on sp 0x%08X, want its own stack",
                (unsigned)cpu->gpr[29]);
    for (int i = 6; i < 26; i++)
        if (cpu->gpr[i] != 0xDEAD0000u + (uint32_t)i)
            FX_FAIL("the handler continues with GPR %d = 0x%08X, want 0x%08X", i,
                    (unsigned)cpu->gpr[i], (unsigned)(0xDEAD0000u + (uint32_t)i));
    FX_CHECK(cpu->hi == 0xDEAD1111u && cpu->lo == 0xDEAD2222u);
    FX_CHECK(interrupted_live && fx_read(WORK_COUNT) == count_at_switch);
    handler_continued++;
}

static void g_exit(CPUState *cpu) {
    cpu->pc = 0;
    exits++;
}

static const FxEntry entries[] = {
    { FX_VECTOR, fx_k_exception }, { FX_K_RET, fx_k_ret },
    { FX_K_RFE, fx_k_return_from_exception },
    { WORK_ENTRY, g_worker }, { WORK_LOOP, g_worker },
    { HANDLER_PC, g_handler }, { MAIN_RESUME, g_main },
    { SYSCALL_PC + 4u, g_ct_tail }, { CT_RETURN, g_ct_return },
    { EXIT_PC, g_exit },
};

int main(void) {
    CPUState cpu;
    uint32_t st[6];
    fx_boot(&cpu, entries, sizeof(entries) / sizeof(entries[0]),
            HANDLER_PC, WORK_ENTRY, WORK_STACK, EXIT_PC);
    /* A second thread: main, suspended in its own ChangeThread. */
    fx_write(0x114u, 2u * FX_TCB_BYTES);
#ifdef CHANGETHREAD_NOT_RUNNABLE
    fx_write(TCB_MAIN, 0x1000u);
#else
    fx_write(TCB_MAIN, 0x4000u);
#endif
    for (int i = 1; i < 32; i++) fx_write(TCB_REG(TCB_MAIN, i), 0x3A000000u + (uint32_t)i);
    fx_write(TCB_REG(TCB_MAIN, 2), 1u);
    fx_write(TCB_REG(TCB_MAIN, 29), MAIN_STACK);
    fx_write(TCB_EPC(TCB_MAIN), MAIN_RESUME);
    fx_write(TCB_MAIN + 0x8Cu, 0x3A00AAAAu);
    fx_write(TCB_MAIN + 0x90u, 0x3A00BBBBu);
    fx_write(TCB_SR(TCB_MAIN), 0x00000404u);
    i_mask = 8u; /* DMA */
    s_frame_count = TEST_FRAME;

    psx_changethread_in_handler_stats(st);
    for (int i = 0; i < 6; i++) FX_CHECK(st[i] == 0u);

    psx_scheduler_run(&cpu);

    printf("handler coverage: irqs=%u polls=%u entries=%llu worker_passes=%u "
           "main_runs=%u\n", irqs, polls, (unsigned long long)fx_exception_entries,
           (unsigned)fx_read(WORK_COUNT), main_runs);

    if (main_runs != 1u)
        FX_FAIL("the main thread ran %u time(s) after the handler's ChangeThread, want 1",
                main_runs);
    psx_changethread_in_handler_stats(st);
#if defined(CHANGETHREAD_NOT_RUNNABLE)
    FX_CHECK(irqs == 2u && handler_continued == 0u && worker_resumes == 0u);
    FX_CHECK(fx_read(WORK_COUNT) == count_at_switch);
    FX_CHECK(st[0] == 1u && st[1] == 0u && st[2] == 0u);
    printf("PASS: a block whose state word says closed is taken at the call\n");
#elif defined(CHANGETHREAD_NO_RESTORE)
    if (handler_continued != 1u)
        FX_FAIL("the handler continued %u time(s) after its call, want 1", handler_continued);
    FX_CHECK(irqs == 2u && worker_resumes == 0u && main_afters == 1u);
    FX_CHECK(fx_read(WORK_COUNT) == count_at_switch);
    FX_CHECK(st[0] == 1u && st[1] == 0u && st[2] == 0u);
    printf("PASS: without the copy written back, the handler continues after its call with v0 = 1\n");
#else
    FX_CHECK(worker_resumes == 1u);            /* resumed once, from the copy */
    if (fx_read(WORK_COUNT) != PASSES)
        FX_FAIL("the worker did %u passes, want %u: part of its work ran twice",
                (unsigned)fx_read(WORK_COUNT), (unsigned)PASSES);
    FX_CHECK(exits == 1u && main_afters == 1u && handler_continued == 0u);
    FX_CHECK(irqs == 4u && plain_resumes == 1u && self_resumes == 1u && outside_resumes == 1u);
#ifndef CHANGETHREAD_CONTROL
    if (st[0] != 1u || st[1] != 1u || st[2] != 1u || st[3] != WORK_LOOP ||
        st[4] != TCB_MAIN || st[5] != TEST_FRAME)
        FX_FAIL("counters: switches %u, same thread %u, not taken %u, first EPC 0x%08X, "
                "first target 0x%08X, first frame %u", (unsigned)st[0], (unsigned)st[1],
                (unsigned)st[2], (unsigned)st[3], (unsigned)st[4], (unsigned)st[5]);
#endif
    printf("PASS: %u interrupts; the handler's ChangeThread switched at the call, "
           "the worker did %u passes once each\n", irqs, (unsigned)fx_read(WORK_COUNT));
#endif
    return 0;
}
