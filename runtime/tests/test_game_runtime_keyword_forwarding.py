#!/usr/bin/env python3
"""psxrecomp_add_game_runtime hands every argument to the runtime target as
what it is, whatever order the title writes them in.

Runs the REAL psxrecomp_add_game_runtime body from runtime.cmake under
`cmake -P`, with the real psx_split_keyword_tail. psxrecomp_add_runtime_target
is replaced by its own real argument parse (everything up to its
cmake_parse_arguments call), so what is checked is what the runtime target
would actually receive. The wrapper parses some list keywords itself and
forwards the rest; cmake_parse_arguments keeps collecting a list until it meets
one of the wrapper's own keywords, so a forwarded keyword written after a list
used to be swallowed into it. For PSX_RECOMP_UI OFF and ON, on the full runtime
and the setup host, and with forwarded keywords (APP_ICON, MAX_PLAYERS) written
after each of the wrapper's lists, checks that:

  1. the title's EXTRAS_SOURCES are compiled once, wherever they are written
     (after CODEGEN_SETUP_SOURCES they were dropped with recomp-ui off);
  2. the codegen setup sources, host included, are compiled only with
     recomp-ui, and are never pushed out of the call by a forwarded keyword;
  3. every forwarded keyword keeps its value and nothing is left unparsed;
  4. each list keeps exactly its own values.
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
SPLIT_CMAKE = REPO / "runtime" / "psx_split_keyword_tail.cmake"

TITLE_SOURCES = ["title/plugin.c", "title/plugin_hooks.c"]
CODEGEN_SETUP = "codegen_setup.c"
CODEGEN_HOST = "psxrecomp/host/psxrecomp_codegen_host.c"
FULL_C = "generated/GAME_full.c"
WAIT_SITE = "0x80010000"
EXCLUDED = "psx.presentation.bezel"

# Each piece of a call, so a layout is just an order.
WINDOW = 'WINDOW_TITLE "Probe"'
MARKER = 'GEN_MARKER "generated/GAME_dispatch.c"'
GLOB = 'GEN_FULL_GLOB "%s"' % FULL_C
SETUP = 'CODEGEN_SETUP_SOURCES "%s"' % CODEGEN_SETUP
EXTRAS = "EXTRAS_SOURCES " + " ".join(TITLE_SOURCES)
WAIT = "NATIVE_IDLE_WAIT_SITES " + WAIT_SITE
EXCLUDE = "EXCLUDE_BUILTIN_MODS " + EXCLUDED
FORWARDED = 'APP_ICON "icon.ico" MAX_PLAYERS 2'

LAYOUTS = {
    "EXTRAS_SOURCES after CODEGEN_SETUP_SOURCES":
        (WINDOW, MARKER, GLOB, SETUP, EXTRAS, FORWARDED),
    "EXTRAS_SOURCES before the list keywords":
        (EXTRAS, FORWARDED, WINDOW, MARKER, GLOB, SETUP),
    "forwarded keywords after CODEGEN_SETUP_SOURCES":
        (WINDOW, MARKER, GLOB, EXTRAS, SETUP, FORWARDED),
    "forwarded keywords after GEN_FULL_GLOB":
        (WINDOW, MARKER, EXTRAS, SETUP, GLOB, FORWARDED),
    "forwarded keywords after NATIVE_IDLE_WAIT_SITES":
        (WINDOW, MARKER, GLOB, SETUP, EXTRAS, WAIT, FORWARDED),
    "forwarded keywords after EXCLUDE_BUILTIN_MODS":
        (WINDOW, MARKER, GLOB, SETUP, EXTRAS, EXCLUDE, FORWARDED),
}

RECORDED = ("EXTRAS_SOURCES", "APP_ICON", "MAX_PLAYERS", "GAME_GENERATED_FULL_C",
            "NATIVE_IDLE_WAIT_SITES", "EXCLUDE_BUILTIN_MODS", "UNPARSED_ARGUMENTS")


def extract_function(text: str, name: str) -> str:
    m = re.search(r"^function\(%s\b.*?^endfunction\(\)" % re.escape(name),
                  text, re.S | re.M)
    if not m:
        raise SystemExit("FAIL: %s not found" % name)
    return m.group(0)


def runtime_target_parse(text: str) -> str:
    """psxrecomp_add_runtime_target cut after its real argument parse."""
    m = re.search(r"^function\(psxrecomp_add_runtime_target\b.*?"
                  r"^\s*cmake_parse_arguments\(PSXRT\b[^\n]*\n"
                  r".*?^\s*if\(PSXRT_UNPARSED_ARGUMENTS\).*?^\s*endif\(\)\n",
                  text, re.S | re.M)
    if not m:
        raise SystemExit("FAIL: psxrecomp_add_runtime_target's parse not found")
    record = "".join('    set_property(GLOBAL PROPERTY RT_%s "${PSXRT_%s}")\n' % (k, k)
                     for k in RECORDED)
    return m.group(0) + record + '    set_property(GLOBAL PROPERTY RT_ALL "${ARGN}")\nendfunction()\n'


STUBS = r'''
cmake_minimum_required(VERSION 3.20)
set(CMAKE_CURRENT_SOURCE_DIR "@SRC@")
set(PSXRECOMP_ROOT "@SRC@/psxrecomp")
set(CMAKE_BUILD_TYPE Release)
set(PSX_RECOMP_UI @UI@)
function(target_compile_definitions)
endfunction()
function(target_include_directories)
endfunction()
'''

REPORT = r'''
foreach(_k @KEYS@)
    get_property(_v GLOBAL PROPERTY RT_${_k})
    message(STATUS "RT_${_k}=${_v}")
endforeach()
'''.replace("@KEYS@", " ".join(RECORDED + ("ALL",)))


def run_case(cmake, functions, call, ui, game_c):
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "generated").mkdir()
        if game_c:
            (tmp / "generated" / "GAME_dispatch.c").write_text("/* stub */\n")
            (tmp / FULL_C).write_text("/* stub */\n")
        (tmp / "VERSION").write_text("1.2.3\n")
        driver = tmp / "driver.cmake"
        driver.write_text(STUBS.replace("@SRC@", tmp.as_posix()).replace("@UI@", ui)
                          + functions + "\n" + call + "\n" + REPORT)
        r = subprocess.run([cmake, "-P", str(driver)], capture_output=True, text=True)
        out = r.stdout + r.stderr
        if r.returncode != 0:
            return None, out
        got = {}
        for key in RECORDED + ("ALL",):
            value = re.search(r"RT_%s=(.*)" % key, out).group(1).strip()
            # Paths are reported relative to the fixture root for comparison.
            got[key] = [v.replace(tmp.as_posix() + "/", "")
                        for v in value.split(";") if v]
        return got, out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", default=shutil.which("cmake") or "cmake")
    args = ap.parse_args()
    if shutil.which(args.cmake) is None and not Path(args.cmake).is_file():
        print("SKIP: no cmake executable available (%s)" % args.cmake)
        return 0
    runtime = RUNTIME_CMAKE.read_text(encoding="utf-8")
    functions = "\n".join((
        extract_function(SPLIT_CMAKE.read_text(encoding="utf-8"), "psx_split_keyword_tail"),
        runtime_target_parse(runtime),
        extract_function(runtime, "psxrecomp_add_game_runtime")))
    failures = 0

    def check(ok, what, out=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + what)
        if not ok:
            failures += 1
            if out:
                print(out)

    for layout, pieces in LAYOUTS.items():
        call = "psxrecomp_add_game_runtime(psx-runtime\n    %s)" % "\n    ".join(pieces)
        for ui in ("OFF", "ON"):
            for game_c in (True, False):
                what = "%s, PSX_RECOMP_UI %s, %s" % (
                    layout, ui, "full runtime" if game_c else "setup host")
                got, out = run_case(args.cmake, functions, call, ui, game_c)
                check(got is not None, what + ": configure runs", out)
                if got is None:
                    continue
                for src in TITLE_SOURCES:
                    check(got["EXTRAS_SOURCES"].count(src) == 1 and got["ALL"].count(src) == 1,
                          "%s: %s is compiled once" % (what, src), out)
                setup = [CODEGEN_SETUP, CODEGEN_HOST] if ui == "ON" else []
                check([s for s in got["EXTRAS_SOURCES"] if s not in TITLE_SOURCES] == setup,
                      "%s: codegen setup sources, host included, only with recomp-ui" % what, out)
                check(got["APP_ICON"] == ["icon.ico"] and got["MAX_PLAYERS"] == ["2"],
                      "%s: forwarded keywords keep their values" % what, out)
                check(got["UNPARSED_ARGUMENTS"] == [],
                      "%s: nothing is left unparsed" % what, out)
                check(got["GAME_GENERATED_FULL_C"] == ([FULL_C] if game_c else []),
                      "%s: GEN_FULL_GLOB expands to just its own pattern" % what, out)
                check(got["NATIVE_IDLE_WAIT_SITES"] == ([WAIT_SITE] if WAIT in pieces else []),
                      "%s: NATIVE_IDLE_WAIT_SITES keeps just its own values" % what, out)
                check(got["EXCLUDE_BUILTIN_MODS"] == ([EXCLUDED] if EXCLUDE in pieces else []),
                      "%s: EXCLUDE_BUILTIN_MODS keeps just its own ids" % what, out)

    call = ("psxrecomp_add_game_runtime(psx-runtime\n    %s\n    APP_ICNO \"icon.ico\")"
            % "\n    ".join((WINDOW, MARKER, GLOB, SETUP)))
    got, out = run_case(args.cmake, functions, call, "ON", True)
    check(got is None and "unknown argument(s):" in out and "APP_ICNO;icon.ico" in out,
          "a misspelt keyword after a list fails configure instead of vanishing", out)

    if failures:
        print("test_game_runtime_keyword_forwarding: %d FAILURES" % failures)
        return 1
    print("test_game_runtime_keyword_forwarding: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
