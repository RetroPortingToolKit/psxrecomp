"""Pin the dirty-RAM interpreter's [[mod_immediate.sites]] call site: the word
substitution must follow the guarded instruction hook (which compares the
original word) and precede every use of `insn` for decode and execution."""
import pathlib
import re

SRC = pathlib.Path(__file__).resolve().parents[1] / "src" / "dirty_ram_interp.c"


def test_interp_substitutes_after_instruction_hook():
    text = SRC.read_text(encoding="utf-8")
    start = text.index("static int exec_one_fetched(CPUState *cpu, uint32_t pc, uint32_t insn,\n"
                       "                            uint32_t *next_pc_out) {")
    body = text[start:text.index("static int exec_one_fetched_inner(CPUState *cpu, uint32_t pc, uint32_t insn,", start + 10)]
    patch = "if (g_psx_mod_immediate_live) insn = psx_mod_immediate_patch(pc, insn);"
    hook = "if (g_psx_mod_instruction_hooks) psx_mod_instruction(cpu, pc, insn);"
    assert body.count(patch) == 1
    assert body.index(hook) < body.index(patch), "hook guards the original word"
    # decode of the load/store fields happens after the substitution
    assert body.index(patch) < body.index("const uint32_t ld_op = op_field(insn);")


def test_interp_includes_header():
    text = SRC.read_text(encoding="utf-8")
    assert re.search(r'#include "mod_immediate\.h"', text)
