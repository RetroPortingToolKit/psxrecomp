#!/usr/bin/env python3
"""psxrecomp_add_game_runtime(... MEMCARD_SLOTS n): runs the REAL
psxrt_resolve_memcard_slots from runtime.cmake under `cmake -P`. Unset keeps
both slots, 1 and 2 pass through, anything else fails the configure.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RUNTIME_CMAKE = REPO / "runtime" / "runtime.cmake"


def extract_function(text: str, name: str) -> str:
    m = re.search(r"^function\(%s\b.*?^endfunction\(\)" % re.escape(name),
                  text, re.S | re.M)
    if not m:
        raise SystemExit("FAIL: %s not found in runtime.cmake" % name)
    return m.group(0)


def resolve(cmake: str, body: str, value: str):
    with tempfile.TemporaryDirectory() as td:
        driver = Path(td) / "driver.cmake"
        driver.write_text(body + '\npsxrt_resolve_memcard_slots(_n "%s")\n'
                          'message(STATUS "SLOTS=${_n}")\n' % value)
        r = subprocess.run([cmake, "-P", str(driver)], capture_output=True, text=True)
        m = re.search(r"SLOTS=(\S+)", r.stdout + r.stderr)
        return (m.group(1) if r.returncode == 0 and m else None)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", default=shutil.which("cmake") or "cmake")
    args = ap.parse_args()
    if shutil.which(args.cmake) is None and not Path(args.cmake).is_file():
        print("SKIP: no cmake executable available (%s)" % args.cmake)
        return 0
    body = extract_function(RUNTIME_CMAKE.read_text(encoding="utf-8"),
                            "psxrt_resolve_memcard_slots")
    cases = (("", "2", "unset keeps both slots"),
             ("1", "1", "a port-1-only title gets one slot"),
             ("2", "2", "two slots pass through"),
             ("3", None, "three slots fail the configure"),
             ("0", None, "zero slots fail the configure"),
             ("1.5", None, "fractional slots fail the configure"),
             ("one", None, "non-numeric slots fail the configure"),
             ("1;2", None, "lists fail the configure"))
    failures = 0
    for value, want, what in cases:
        ok = resolve(args.cmake, body, value) == want
        print(("ok   " if ok else "FAIL ") + what)
        failures += not ok
    if failures:
        print("test_game_runtime_memcard_slots: %d FAILURES" % failures)
        return 1
    print("test_game_runtime_memcard_slots: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
