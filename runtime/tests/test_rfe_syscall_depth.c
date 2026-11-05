/* Repeated synthetic handler-driven returns must unwind host continuations while retaining the guest TCB context. No BIOS or media is used. */
#include "rfe_guest_fixture.h"

#define YIELD_ENTRY  0x80010000u
#define YIELD_LOOP   0x80010020u /* loop head, a block leader */
#define SYSCALL_PC   0x80010028u /* the `syscall` instruction */
#define YIELD_NEXT   0x8001002Cu /* EPC + 4 */
#define HANDLER_PC   0x80020000u
#define EXIT_PC      0x80040000u

#define MAIN_STACK   0x80140F5Cu
#define ROUNDS       10000u

static unsigned rounds, exits, resumes;
static uintptr_t host_lo = UINTPTR_MAX, host_hi;
static uint32_t entry_ra;

static void g_yield(CPUState *cpu) {
    uint32_t entry = cpu->pc;
    cpu->pc = 0;
    if (entry == YIELD_NEXT) {
        /* The return from exception: EPC + 4, with the loop's own registers. */
        FX_CHECK(cpu->gpr[29] == MAIN_STACK && cpu->gpr[16] == 0x51000010u);
        FX_CHECK(cpu->gpr[31] == entry_ra && cpu->cop0[12] == 0x00000401u);
        resumes++;
        goto block_yield_loop; /* j YIELD_LOOP */
    }
    FX_CHECK(entry == YIELD_ENTRY);
    entry_ra = cpu->gpr[31];
    cpu->gpr[16] = 0x51000010u;

block_yield_loop:
    psx_cycle_count += 100u;
    psx_check_interrupts_at(cpu, YIELD_LOOP);
    if (rounds >= ROUNDS) {
        cpu->pc = cpu->gpr[31];
        return;
    }
    cpu->gpr[4] = 0u; /* not a kernel service number: the handler owns it */
    cpu->pc = SYSCALL_PC; if (psx_syscall(cpu, 0x500u)) return;
    FX_FAIL("the syscall returned without an exception");
}

/* The game's exception handler: claim its own syscall, do the work, step the
 * saved EPC past the instruction and return from the exception. */
static void g_handler(CPUState *cpu) {
    volatile char here;
    uintptr_t at = (uintptr_t)&here;
    cpu->pc = 0;
    if (at < host_lo) host_lo = at;
    if (at > host_hi) host_hi = at;
    FX_CHECK((fx_read(FX_TCB_CAUSE) & 0x7Cu) == 0x20u); /* a syscall */
    FX_CHECK(fx_read(FX_TCB_EPC) == SYSCALL_PC);
    for (int i = 1; i < 32; i++) cpu->gpr[i] = 0xDEAD0000u + (uint32_t)i;
    rounds++;
    fx_write(FX_TCB_EPC, fx_read(FX_TCB_EPC) + 4u);
    cpu->gpr[31] = HANDLER_PC + 8u;
    cpu->pc = FX_K_RFE; /* ReturnFromException(); it does not come back */
}

static void g_exit(CPUState *cpu) {
    cpu->pc = 0;
    exits++;
}

static const FxEntry entries[] = {
    { FX_VECTOR, fx_k_exception }, { FX_K_RET, fx_k_ret },
    { FX_K_RFE, fx_k_return_from_exception },
    { YIELD_ENTRY, g_yield }, { YIELD_LOOP, g_yield }, { YIELD_NEXT, g_yield },
    { HANDLER_PC, g_handler }, { EXIT_PC, g_exit },
};

int main(void) {
    CPUState cpu;
    fx_boot(&cpu, entries, sizeof(entries) / sizeof(entries[0]),
            HANDLER_PC, YIELD_ENTRY, MAIN_STACK, EXIT_PC);

    psx_scheduler_run(&cpu);

    FX_CHECK(exits == 1u && rounds == ROUNDS && resumes == ROUNDS);
    FX_CHECK(fx_exception_entries == ROUNDS);
    /* Flat: the exception entry and the kernel's handler call, nothing more. */
    FX_CHECK(fx_max_dispatch_depth <= 3);
    FX_CHECK(host_hi - host_lo < 256u);
    printf("PASS: %u handler-driven returns, dispatch depth %d, host stack spread %u bytes\n",
           rounds, fx_max_dispatch_depth, (unsigned)(host_hi - host_lo));
    return 0;
}
