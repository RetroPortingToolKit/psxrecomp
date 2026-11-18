/* The entry in production traps.c that the interpreter's BREAK asks, and the
 * counters the run report prints.
 *
 * test_break_guest_vector_interp.c supplies its own entry, so the real one
 * (the in-exception test and the counters) had no test. This drives
 * psx_break_enter_guest_vector of traps.c with a guest-owned vector, then with
 * the BIOS's, and reads psx_break_guest_vector_stats. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)

#define VECTOR      0x80000080u
#define HANDLER     0x8001334Cu
#define BREAK_PC_1  0x80010160u
#define BREAK_PC_2  0x80010200u
#define BREAK_WORD(code) ((((uint32_t)(code)) << 6) | 0x0Du)

static uint32_t ram[0x20000u / 4u];
static uint32_t read_word(uint32_t a) {
    a &= 0x1FFFFFFFu;
    CHECK(a < sizeof(ram) && (a & 3u) == 0u);
    return ram[a / 4u];
}
static void write_word(uint32_t a, uint32_t v) {
    (void)a; (void)v;
    CHECK(!"entering the guest vector stores nothing in guest memory");
}

/* The one state the entry in traps.c reads besides the CPU. */
static int fx_in_exception;
int psx_get_in_exception(void) { return fx_in_exception; }

int psx_break_enter_guest_vector(CPUState *cpu, uint32_t pc);
void psx_break_guest_vector_stats(uint32_t out[3]);

static void guest_vector(void) {            /* lui k0,hi ; ori k0,k0,lo ; jr k0 */
    ram[0x80u / 4u] = 0x3C1A0000u | (HANDLER >> 16);
    ram[0x84u / 4u] = 0x375A0000u | (HANDLER & 0xFFFFu);
    ram[0x88u / 4u] = 0x03400008u;
}
static void bios_vector(void) {             /* the same shape, target in the kernel */
    ram[0x80u / 4u] = 0x3C1A0000u;
    ram[0x84u / 4u] = 0x375A0C80u;
    ram[0x88u / 4u] = 0x03400008u;
}

static void fresh(CPUState *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->read_word = read_word;
    cpu->write_word = write_word;
    cpu->cop0[12] = 0x00000401u;
}

int main(void) {
    CPUState cpu;
    uint32_t st[3];

    ram[(BREAK_PC_1 & 0x1FFFFu) / 4u] = BREAK_WORD(0xFB42u);
    ram[(BREAK_PC_2 & 0x1FFFFu) / 4u] = BREAK_WORD(0x1C00u);

    psx_break_guest_vector_stats(st);
    CHECK(st[0] == 0u && st[1] == 0u && st[2] == 0u);

    /* Under the BIOS's vector nothing is entered and nothing is counted. */
    bios_vector();
    fresh(&cpu);
    CHECK(psx_break_enter_guest_vector(&cpu, BREAK_PC_1) == 0);
    CHECK(cpu.pc == 0u && cpu.cop0[14] == 0u && cpu.cop0[12] == 0x00000401u);
    psx_break_guest_vector_stats(st);
    CHECK(st[0] == 0u && st[1] == 0u && st[2] == 0u);

    /* The guest's vector: entered, and counted with the BREAK's own PC and
     * code. */
    guest_vector();
    fresh(&cpu);
    CHECK(psx_break_enter_guest_vector(&cpu, BREAK_PC_1) == 1);
    CHECK(cpu.pc == VECTOR && cpu.cop0[14] == BREAK_PC_1);
    CHECK(((cpu.cop0[13] >> 2) & 0x1Fu) == 9u);
    psx_break_guest_vector_stats(st);
    CHECK(st[0] == 1u && st[1] == BREAK_PC_1 && st[2] == 0xFB42u);

    /* A second BREAK raises the count; the first PC and code stay. */
    fresh(&cpu);
    CHECK(psx_break_enter_guest_vector(&cpu, BREAK_PC_2) == 1);
    psx_break_guest_vector_stats(st);
    CHECK(st[0] == 2u && st[1] == BREAK_PC_1 && st[2] == 0xFB42u);

    /* Inside the handler window and with SR.BEV set the rule refuses; a
     * refusal is not counted. */
    fresh(&cpu);
    fx_in_exception = 1;
    CHECK(psx_break_enter_guest_vector(&cpu, BREAK_PC_2) == 0);
    fx_in_exception = 0;
    fresh(&cpu);
    cpu.cop0[12] |= 0x00400000u;            /* SR.BEV: the ROM vector is live */
    CHECK(psx_break_enter_guest_vector(&cpu, BREAK_PC_2) == 0);
    psx_break_guest_vector_stats(st);
    CHECK(st[0] == 2u && st[1] == BREAK_PC_1 && st[2] == 0xFB42u);
    puts("PASS: guest-vector BREAK entries are counted with the first PC and code");
    return 0;
}
