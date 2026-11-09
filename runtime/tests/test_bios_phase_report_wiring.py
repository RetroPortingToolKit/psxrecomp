#!/usr/bin/env python3
"""Guard that the BIOS-phase line reads the table at game entry (PS1B-105).

runtime/tests/test_bios_phase_report.c proves the module: given a per-address
table it sums the interpreted dispatches by place and writes the line, and the
hook prints once. This proves the three places that connect it:

  1. fntrace_mark_game_started calls the game-entry hook right after the latch
     goes to 1 and before every side effect of the entry, so nothing has moved
     or cleared the interpreter's table when the hook reads it;
  2. main.cpp installs the module at start-up, next to the crash handlers;
  3. the runtime's source list builds the module.

Reading source rather than running a binary is deliberate: game entry needs a
running BIOS and a disc. Standard library only.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FNTRACE = (ROOT / "runtime" / "src" / "fntrace.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "fntrace.h").read_text(encoding="utf-8")
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
SOURCES = (ROOT / "runtime" / "runtime.cmake").read_text(encoding="utf-8")

# ---- 1. the hook is the first thing at game entry ---------------------------
start = FNTRACE.index("void fntrace_mark_game_started(CPUState* cpu) {")
end = FNTRACE.index("\n}\n", start)
body = FNTRACE[start:end]
latch = body.index("s_game_started = 1;")
hook = body.index("if (g_fntrace_game_entry_hook) g_fntrace_game_entry_hook();")
assert latch < hook, "the hook must run after the latch is set (a re-entry from the hook returns at once)"
CALL = r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\("
after_hook = re.findall(CALL, body[hook:], flags=re.M)
before_hook = re.findall(CALL, body[latch:hook], flags=re.M)
assert before_hook == [], f"nothing may run between the latch and the hook, found {before_hook}"
assert "dirty_ram_clear_image_baseline" in after_hook, "the entry's side effects follow the hook"
assert "extern void (*g_fntrace_game_entry_hook)(void);" in HEADER, "fntrace.h declares the hook"
assert re.search(r"^void \(\*g_fntrace_game_entry_hook\)\(void\) = 0;", FNTRACE, flags=re.M), (
    "fntrace.c defines the hook, NULL by default, so a build without the module does nothing")

# ---- 2. main.cpp installs the module ----------------------------------------
crash = MAIN.index("psx_crash_trace_install_handlers();")
install = MAIN.index("bios_phase_report_install();")
assert crash < install < crash + 400, "bios_phase_report_install() follows the crash handlers' install"
assert '#include "bios_phase_report.h"' in MAIN, "main.cpp includes the module's header"

# ---- 3. the runtime builds the module ---------------------------------------
assert "runtime/src/bios_phase_report.c" in SOURCES, "runtime.cmake must build bios_phase_report.c"

print("bios_phase_report wiring: ok")
