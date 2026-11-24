#!/usr/bin/env python3
"""psxrecomp_add_game_runtime(... PGXP) builds the title's one runtime as the
PGXP hook flavor (docs/ENHANCEMENTS.md G1.11).

Runs the REAL psxrecomp_add_game_runtime body from runtime.cmake under
`cmake -P`, with psxrecomp_add_runtime_target and the target-scoped commands
stubbed to record what they receive. Checks that:

  1. without PGXP nothing PGXP-related is forwarded (default unchanged);
  2. PGXP is forwarded to psxrecomp_add_runtime_target, as the bare option
     that selects the hook flavor for the primary (no PGXP_CLONE, so no _pgxp
     suffix and no sibling);
  3. PGXP written after CODEGEN_SETUP_SOURCES is still the option, not a
     source file (it used to be swallowed by the multi-value argument);
  4. the setup-host path (no generated C) forwards it the same way.
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
PARTITION_CMAKE = REPO / "runtime" / "psx_partition_game_runtime_args.cmake"


def extract_function(text: str, name: str) -> str:
    m = re.search(r"^function\(%s\b.*?^endfunction\(\)" % re.escape(name),
                  text, re.S | re.M)
    if not m:
        raise SystemExit("FAIL: %s not found in runtime.cmake" % name)
    return m.group(0)


STUBS = r'''
cmake_minimum_required(VERSION 3.20)
set(CMAKE_CURRENT_SOURCE_DIR "@SRC@")
set(PSXRECOMP_ROOT "@SRC@/psxrecomp")
set(CMAKE_BUILD_TYPE Release)
set(PSX_RECOMP_UI OFF)
function(psxrecomp_add_runtime_target target)
    set_property(GLOBAL PROPERTY RT_TARGET "${target}")
    set_property(GLOBAL PROPERTY RT_ARGS "${ARGN}")
endfunction()
include("@PARTITION@")
function(target_compile_definitions)
endfunction()
function(target_include_directories)
endfunction()
'''

REPORT = r'''
get_property(_t GLOBAL PROPERTY RT_TARGET)
get_property(_a GLOBAL PROPERTY RT_ARGS)
message(STATUS "RT_TARGET=${_t}")
message(STATUS "RT_ARGS=${_a}")
'''


def run_case(cmake: str, body: str, call: str, game_c: bool):
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "generated").mkdir()
        if game_c:
            (tmp / "generated" / "GAME_dispatch.c").write_text("/* stub */\n")
            (tmp / "generated" / "GAME_full.c").write_text("/* stub */\n")
        (tmp / "VERSION").write_text("1.2.3\n")
        driver = tmp / "driver.cmake"
        driver.write_text(STUBS.replace("@SRC@", tmp.as_posix())
                          .replace("@PARTITION@", PARTITION_CMAKE.as_posix()) + body +
                          "\n" + call + "\n" + REPORT)
        r = subprocess.run([cmake, "-P", str(driver)], capture_output=True,
                           text=True)
        out = r.stdout + r.stderr
        if r.returncode != 0:
            return None, None, out
        target = re.search(r"RT_TARGET=(.*)", out).group(1).strip()
        args = re.search(r"RT_ARGS=(.*)", out).group(1).strip().split(";")
        return target, args, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", default=shutil.which("cmake") or "cmake")
    args = ap.parse_args()
    if shutil.which(args.cmake) is None and not Path(args.cmake).is_file():
        print("SKIP: no cmake executable available (%s)" % args.cmake)
        return 0
    body = extract_function(RUNTIME_CMAKE.read_text(encoding="utf-8"),
                            "psxrecomp_add_game_runtime")
    failures = 0

    def check(ok, what, out=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            failures += 1
            if out:
                print(out)

    base = ('psxrecomp_add_game_runtime(psx-runtime\n'
            '    WINDOW_TITLE "Probe"\n'
            '    GEN_MARKER "generated/GAME_dispatch.c"\n'
            '    GEN_FULL_GLOB "generated/GAME_full.c"\n'
            '    @EXTRA@)')

    t, a, out = run_case(args.cmake, body, base.replace("@EXTRA@", ""), True)
    check(a is not None, "configure without PGXP runs", out)
    if a is not None:
        check(t == "psx-runtime", "the primary target is psx-runtime")
        check("PGXP" not in a and "PGXP_CLONE" not in a,
              "without PGXP nothing PGXP-related is forwarded")

    t, a, out = run_case(args.cmake, body, base.replace("@EXTRA@", "PGXP"), True)
    check(a is not None, "configure with PGXP runs", out)
    if a is not None:
        check(t == "psx-runtime", "PGXP keeps the primary target name")
        check(a.count("PGXP") == 1, "PGXP is forwarded once")
        check("PGXP_CLONE" not in a,
              "the primary is not marked as the _pgxp clone (no suffix)")

    late = base.replace(
        "@EXTRA@",
        'CODEGEN_SETUP_SOURCES "codegen_setup.c"\n    PGXP')
    t, a, out = run_case(args.cmake, body, late, True)
    check(a is not None, "configure with PGXP after CODEGEN_SETUP_SOURCES runs",
          out)
    if a is not None:
        check("PGXP" in a,
              "PGXP after a multi-value argument is still the option")

    t, a, out = run_case(args.cmake, body, base.replace("@EXTRA@", "PGXP"),
                         False)
    check(a is not None, "setup-host configure with PGXP runs", out)
    if a is not None:
        check("PGXP" in a, "the setup host gets the same flavor")

    if failures:
        print("test_game_runtime_pgxp: %d FAILURES" % failures)
        return 1
    print("test_game_runtime_pgxp: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
