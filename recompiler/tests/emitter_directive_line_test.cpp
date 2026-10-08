/*
 * A preprocessor directive owns its whole line. Anything appended after it on
 * that line is swallowed as "extra tokens" and silently DISCARDED -- no build
 * failure, just a -Wendif-labels warning nobody reads. PR #171 was exactly
 * this: `append_pgxp_hooks` space-joined its PGXP_*() call onto emissions that
 * end on a bare `#endif` (the PSX_ENABLE_BLOCK_CYCLES muldiv stall/latency
 * blocks), so 3,187 hooks per title were thrown away.
 *
 * This pins the predicate that now guards the remaining same-line appenders,
 * so the hazard cannot come back by construction.
 *
 * Build/run: ctest -R emitter_directive_line_test
 */

#include "pgxp_hook_emitter.h"

#include <cstdio>
#include <string>

static int failures;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
        else         { std::printf("ok:   %s\n", (msg)); }                      \
    } while (0)

using PSXRecomp::emission_ends_on_preprocessor_directive;

int main() {
    /* The shape that caused PR #171: translate_mult's real emission. */
    const std::string muldiv =
        "{ int64_t result = (int64_t)(int32_t)cpu->gpr[4] * (int64_t)(int32_t)cpu->gpr[5];"
        " cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }"
        "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_set(cpu, 13u);\n#endif";
    CHECK(emission_ends_on_preprocessor_directive(muldiv),
          "a muldiv emission ending on a bare #endif is detected");

    /* translate_mfhi with rd==$zero: the stall block with no statement. */
    CHECK(emission_ends_on_preprocessor_directive(
              "/* nop: write to $zero */"
              "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_stall(cpu);\n#endif"),
          "a zero-reg mfhi/mflo emission ending on #endif is detected");

    /* Leading whitespace is legal before a directive. */
    CHECK(emission_ends_on_preprocessor_directive("foo();\n    #endif"),
          "an indented directive still owns its line");
    CHECK(emission_ends_on_preprocessor_directive("foo();\n\t#else"),
          "a tab-indented directive still owns its line");
    CHECK(emission_ends_on_preprocessor_directive("#endif"),
          "a single-line emission that is only a directive is detected");

    /* Ordinary statements must NOT be flagged -- a false positive here would
     * inject stray newlines into every instruction's output. */
    CHECK(!emission_ends_on_preprocessor_directive("cpu->gpr[4] = cpu->hi;"),
          "a plain statement is not a directive line");
    CHECK(!emission_ends_on_preprocessor_directive(
              "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_gte_stall(cpu);\n#endif\n    "),
          "an emission whose directive block is followed by a fresh indented "
          "line is safe to append to");
    CHECK(!emission_ends_on_preprocessor_directive(
              "{ uint32_t _pgxa = cpu->gpr[4]; foo();\n    PGXP_LOAD(0u, _pgxa, x); }"),
          "an already-fixed hook emission is not flagged");
    CHECK(!emission_ends_on_preprocessor_directive(""),
          "an empty emission is not flagged");
    CHECK(!emission_ends_on_preprocessor_directive("x = 1; /* #endif in a comment */"),
          "a '#' that is not the line's first token is not a directive");

    /* COP2 register transfers all carry their PGXP_COP2 hook - the control
     * forms too, so the engine can follow vertex halves packed into a matrix
     * row (CTC2) through MVMVA and back (CFC2). A transfer to $zero writes no
     * register and stays unhooked. */
    {
        const auto hooked = [](uint32_t instr) {
            std::string code = "stmt;";
            PSXRecomp::append_pgxp_hooks(instr, code);
            return code;
        };
        const uint32_t mfc2 = 0x4805C800u;   /* mfc2 a1, $25   */
        const uint32_t cfc2 = 0x4845C800u;   /* cfc2 a1, $25   */
        const uint32_t mtc2 = 0x48994800u;   /* mtc2 t9, $9    */
        const uint32_t ctc2 = 0x48C34000u;   /* ctc2 v1, $8    */
        const uint32_t cfc2_zero = 0x4840C800u;   /* cfc2 zero, $25 */
        CHECK(hooked(mfc2).find("PGXP_COP2(0x4805C800u, cpu->gpr[5], 0u);") != std::string::npos,
              "MFC2 carries PGXP_COP2 with its destination value");
        CHECK(hooked(cfc2).find("PGXP_COP2(0x4845C800u, cpu->gpr[5], 0u);") != std::string::npos,
              "CFC2 carries PGXP_COP2 with its destination value");
        CHECK(hooked(mtc2).find("PGXP_COP2(0x48994800u, cpu->gpr[25], 0u);") != std::string::npos,
              "MTC2 carries PGXP_COP2 with its source value");
        CHECK(hooked(ctc2).find("PGXP_COP2(0x48C34000u, cpu->gpr[3], 0u);") != std::string::npos,
              "CTC2 carries PGXP_COP2 with its source value");
        CHECK(hooked(cfc2_zero) == "stmt;",
              "a CFC2 to $zero writes no register and is not hooked");
    }

    /* Coprocessor reads into a GPR replace its value like any ALU op. The
     * interpreters hook them; the compiled code must too, or the register
     * keeps the shadow of the vertex it held before. MFC0's emission ends on
     * the block-cycles #endif, so its hook also has to land on a new line. */
    {
        const uint32_t mfc0 = (0x10u << 26) | (0x00u << 21) | (3u << 16) | (12u << 11);
        std::string code =
            "cpu->gpr[3] = cpu->cop0[12];"
            "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    cpu->ld_absorb = 0u; cpu->ld_which_t = 3u;\n#endif";
        PSXRecomp::append_pgxp_hooks(mfc0, code);
        CHECK(code.find("\n    PGXP_ALU(0x40036000u, cpu->gpr[3], 0u, 0u);") != std::string::npos,
              "MFC0 resets the destination shadow on its own line");

        const uint32_t cfc2 = (0x12u << 26) | (0x02u << 21) | (3u << 16) | (31u << 11);
        code = "cpu->gpr[3] = gte_read_ctrl(cpu, 31);";
        PSXRecomp::append_pgxp_hooks(cfc2, code);
        CHECK(code.find("PGXP_COP2(0x4843F800u, cpu->gpr[3], 0u);") != std::string::npos,
              "CFC2 resets the destination shadow");

        code = "(void)cpu->cop0[12];";
        PSXRecomp::append_pgxp_hooks(mfc0 & ~(31u << 16), code);
        CHECK(code.find("PGXP_") == std::string::npos, "MFC0 to $zero is not hooked");
        code = "(void)gte_read_ctrl(cpu, 31);";
        PSXRecomp::append_pgxp_hooks(cfc2 & ~(31u << 16), code);
        CHECK(code.find("PGXP_") == std::string::npos, "CFC2 to $zero is not hooked");
    }

    std::printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
