#!/usr/bin/env python3
"""Guard architectural JALR link-register and non-$ra transfer handling."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    source = (root / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")

    start = source.index("case 0x09: { /* JALR rd, rs */")
    end = source.index("case 0x0C: /* SYSCALL */", start)
    jalr = source[start:end]

    if "if (rd != 0) cpu->gpr[rd] = return_pc;" not in jalr:
        raise AssertionError("JALR must write the encoded nonzero link register")
    if "cpu->gpr[rd ? rd : 31]" in jalr:
        raise AssertionError("JALR rd=0 must not be rewritten as an implicit $ra link")

    chain = jalr.index("if (rd != 31)")
    compiled = jalr.index("interp_enter_compiled(cpu, target)")
    nonlocal_call = jalr.index("dispatch_nonlocal_call(cpu, target")
    if not chain < compiled < nonlocal_call:
        raise AssertionError(
            "non-$ra JALR must pc-chain before any call-unit optimization"
        )
    non_ra = jalr[chain:compiled]
    if "cpu->pc = target;" not in non_ra or "CRET(CRES_PCCHAIN, 1);" not in non_ra:
        raise AssertionError("non-$ra JALR is missing its faithful pc-chain handoff")

    print("JALR link-register guards: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
