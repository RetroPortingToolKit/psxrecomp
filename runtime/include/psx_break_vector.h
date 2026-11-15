#ifndef PSX_BREAK_VECTOR_H
#define PSX_BREAK_VECTOR_H

/* A BREAK that the guest handles itself.
 *
 * PSX-SPX "COP0 - Exception Handling": `break` raises exception code 09h (Bp)
 * through the general vector. Some programs install their own stub at
 * 0x80000080, execute `break`, and read the value their handler leaves once it
 * returns to EPC+4 with `rfe`. F1 2000's STUB.EXE does this with
 * `break 0xFB42` and checks t0 afterwards.
 *
 * Only a vector that the guest has pointed at its own code is entered. The
 * BIOS's handler has no route for code 09h but SystemErrorUnresolvedException,
 * so under it a BREAK keeps the fatal report of psx_break: the report names
 * the PC, and a console stops there too. One stub shape is accepted:
 * `lui rN,hi`, `ori|addiu rN,rN,lo`, `jr rN`, with a target in RAM at or above
 * 0x10000. The BIOS's own target is in the kernel below that. Any other
 * vector, a plain `j handler` included, is not entered.
 *
 * Only the interpreter calls this, and not for a BREAK in a branch delay slot
 * (EPC and BD would need the branch). A BREAK in statically compiled code
 * keeps the fatal report: its handler would return to EPC+4 in the middle of
 * a compiled function, which is neither an entry nor a call return.
 *
 * Returns 1 with the exception entered: EPC is the BREAK's own PC, Cause
 * holds code 9 with BD clear, the SR mode bits are pushed, and cpu->pc is the
 * vector for the flat dispatcher. Returns 0, with nothing changed, inside the
 * synchronous handler window, with SR.BEV set, and for any other vector. */

#include <stdint.h>
#include "cpu_state.h"

static inline int psx_break_vector_enter(CPUState *cpu, uint32_t pc, int in_exception) {
    uint32_t sr = cpu->cop0[12];
    if ((sr & 0x00400000u) || in_exception) return 0;
    uint32_t w0 = cpu->read_word(0x80000080u);
    uint32_t w1 = cpu->read_word(0x80000084u);
    uint32_t w2 = cpu->read_word(0x80000088u);
    uint32_t reg = (w0 >> 16) & 0x1Fu;
    uint32_t target = (w0 & 0xFFFFu) << 16;
    if ((w0 >> 26) != 0x0Fu) return 0;                       /* lui rN, hi */
    if (((w1 >> 21) & 0x1Fu) != reg || ((w1 >> 16) & 0x1Fu) != reg) return 0;
    if ((w1 >> 26) == 0x0Du) target |= w1 & 0xFFFFu;         /* ori */
    else if ((w1 >> 26) == 0x09u)                            /* addiu */
        target += (uint32_t)(int32_t)(int16_t)(w1 & 0xFFFFu);
    else return 0;
    if ((w2 & 0xFC1FFFFFu) != 0x00000008u || ((w2 >> 21) & 0x1Fu) != reg)
        return 0;                                            /* jr rN */
    target &= 0x1FFFFFFFu;
    if (target < 0x00010000u || target >= 0x00200000u) return 0;
    cpu->cop0[14] = pc;
    cpu->cop0[13] = (cpu->cop0[13] & ~(0x80000000u | 0x7Cu)) | (9u << 2);
    cpu->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);
    cpu->pc = 0x80000080u;
    return 1;
}

#endif /* PSX_BREAK_VECTOR_H */
