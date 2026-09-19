#!/usr/bin/env python3
"""Structural guards for the high-frequency FMV trace fast paths."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def main():
    debug = (ROOT / "runtime/src/debug/debug_server.c").read_text(encoding="utf-8")
    mdec = (ROOT / "runtime/src/mdec/mdec.c").read_text(encoding="utf-8")
    required = (
        (debug, "if (s_fmv_quiet) return;\n    ls_suppress_begin();",
         "FMV quiet mode does not gate function-entry trace work"),
        (mdec, "#ifdef PSX_NO_DEBUG_TOOLS\n    (void)kind;",
         "release MDEC events still write the diagnostic ring"),
        (mdec, "if (debug_server_fmv_quiet()) return;",
         "FMV quiet mode does not gate MDEC trace events"),
    )
    failures = [message for source, needle, message in required if needle not in source]
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: FMV quiet mode gates high-frequency diagnostic traces.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
