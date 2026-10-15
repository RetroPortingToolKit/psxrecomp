#!/usr/bin/env python3
"""Wiring guard for the call-unit IRQ policy (overlay_unit_defer.h).

Both overlay CI wrappers must route the call-unit deferral through
overlay_unit_irq_must_defer(depth, overlay_unit_irq_defer_enabled()), and the
env knob must default to OVERLAY_UNIT_IRQ_DEFER_DEFAULT. A raw
`g_call_unit_depth > 0 -> return` in either wrapper restores the unconditional
deferral that wedged Legend of Dragoon on "Checking MEMORY CARD" (libcard
MemCardSync polling for an IRQ-set flag from a native unit entered by an
interpreted caller; call_unit_depth stuck at 1, ci_skip_unit in the tens of
millions).

Usage: python runtime/tests/test_overlay_unit_defer_wiring.py   Exit 0 = PASS.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / "runtime" / "src" / "overlay_loader.c").read_text(encoding="utf-8")


def body(name: str) -> str:
    m = re.search(rf"\bstatic\s+(?:int|void)\s+{re.escape(name)}\s*\([^;]*?\)\s*\{{", SRC, re.S)
    assert m, f"missing {name}"
    depth, i = 1, m.end()
    while depth:
        c = SRC[i]
        depth += (c == "{") - (c == "}")
        i += 1
    return SRC[m.end():i]


def main() -> int:
    for fn in ("overlay_ci_wrapper", "overlay_ci_at_wrapper"):
        b = body(fn)
        assert "overlay_unit_irq_must_defer(g_call_unit_depth, overlay_unit_irq_defer_enabled())" in b, \
            f"{fn} must gate the unit deferral through the policy helper"
        assert not re.search(r"if\s*\(\s*g_call_unit_depth\s*>\s*0", b), \
            f"{fn} must not skip interrupt checks on g_call_unit_depth directly"
    knob = body("overlay_unit_irq_defer_enabled")
    assert "PSX_OVERLAY_UNIT_IRQ_DEFER" in knob and "OVERLAY_UNIT_IRQ_DEFER_DEFAULT" in knob
    print("overlay_unit_defer_wiring: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
