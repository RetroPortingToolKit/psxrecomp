#ifndef PGXP_HOOK_EMITTER_H
#define PGXP_HOOK_EMITTER_H

#include <cstdint>
#include <string>

namespace PSXRecomp {

/* A conservative straight-line precision summary. Entry registers may carry
 * geometry; ADDIU constants and arithmetic on metadata-free values cannot.
 * LUI, shifts and bitwise exact-half producers keep their full hooks. Memory,
 * coprocessors and unsupported instructions terminate that knowledge. */
class PgxpPlainSpan {
    uint32_t plain_ = 1u;
public:
    void barrier() { plain_ = 1u; }
    bool consume_alu(uint32_t instruction, uint32_t& destination) {
        const uint32_t op = instruction >> 26;
        const uint32_t rs = (instruction >> 21) & 31u;
        const uint32_t rt = (instruction >> 16) & 31u;
        const auto plain = [this](uint32_t reg) { return (plain_ & (uint32_t{1} << reg)) != 0; };
        bool result = false;
        destination = rt;
        if (op == 0) {
            destination = (instruction >> 11) & 31u;
            switch (instruction & 63u) {
            case 0x20: case 0x21: case 0x22: case 0x23:
                result = plain(rs) && plain(rt); break;
            case 0x25: /* OR with zero is a move; other bitwise ops can
                        * originate exact-zero coordinate halves. */
                result = (rs == 0 || rt == 0) && plain(rs) && plain(rt); break;
            case 0x2A: case 0x2B:
                result = true; break;
            default:
                barrier(); return false;
            }
        } else {
            switch (op) {
            case 0x08: case 0x09:
                result = plain(rs); break;
            case 0x0A: case 0x0B:
                result = true; break;
            default:
                barrier(); return false;
            }
        }
        if (!destination) return false;
        const uint32_t bit = uint32_t{1} << destination;
        plain_ = (plain_ & ~bit) | (result ? bit : 0u);
        return result;
    }
};

/* PGXP dataflow-shadowing hook emission (docs/ENHANCEMENTS.md G1.10; runtime
 * pgxp_hooks.h). Shared by BOTH emitters — CodeGenerator (game) and
 * StrictTranslator (BIOS) — so the hook grammar can never drift between them.
 *
 * Wraps the translated statement `code` with a PGXP_*() macro invocation for
 * the instruction classes that move sub-pixel projection provenance. Source
 * operands the statement may clobber are captured into locals BEFORE it;
 * everything else is read after it. In the base build the macros preprocess
 * to ((void)0) and the optimizer erases the dead captures, so base objects
 * are unchanged; only a -DPSX_PGXP=1 TU pays.
 *
 * NOT applied to LWC2/SWC2 — those emissions capture the raw loaded/stored
 * word themselves (a masked GTE register write must validate against the
 * word as loaded, not the register's masked value).
 *
 * Deliberately unhooked: AND/XOR/NOR/SLT-family and the exotic immediates —
 * they only ever DESTROY precision, and the engine's validate-on-read drops
 * their stale shadows without help. */
/* True when `code`'s LAST line is a preprocessor directive.
 *
 * A directive owns its whole line: the preprocessor swallows anything after it
 * as "extra tokens" and silently DISCARDS it (gcc -Wendif-labels). Several
 * emissions end on a bare `#endif` -- the PSX_ENABLE_BLOCK_CYCLES muldiv
 * stall/latency blocks in both emitters (code_generator.cpp translate_mult/
 * multu/div/divu and translate_mfhi/mflo; strict_translator.cpp the same
 * shapes) -- so ANY text appended to such an emission on the same line is
 * dropped without a build failure.
 *
 * That is exactly the defect PR #171 fixed for the PGXP hooks, where 3,187
 * hooks per title were being discarded. This predicate exists so the remaining
 * same-line appenders can be guarded by construction instead of one at a time.
 *
 * Callers append "
" + indent instead of a space when this returns true. */
bool emission_ends_on_preprocessor_directive(const std::string& code);

void append_pgxp_hooks(uint32_t instr, std::string& code,
                       const std::string& memory_addr = "");

} // namespace PSXRecomp

#endif /* PGXP_HOOK_EMITTER_H */
