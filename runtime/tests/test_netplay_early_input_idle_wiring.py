#!/usr/bin/env python3
"""Guard that a netplay peer feeds an idle pad for the first ticks of a session.

An input misprediction in the first ticks of a session, before an interval
snapshot is hash-confirmed, made the rollback engine load a tip snapshot the
other peer had already dropped. The peers then reloaded different snapshots
and never agreed again. main.cpp keeps the pad idle that early, so nothing is
mispredicted there. A match needs sockets and a second peer, so the wiring is
checked in the source. An optional argument names another main.cpp to check.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "runtime" / "src" / "main.cpp"
MAIN = SRC.read_text(encoding="utf-8")

signature = "static void netplay_early_input_idle(PsxNetPad* pad) {"
start = MAIN.find(signature)
assert start >= 0, "main.cpp has no netplay_early_input_idle"
body = MAIN[start : MAIN.index("\n}\n", start) + 3]

# The window covers the first interval snapshots: one at tick 16, the next at
# 32, each confirmed some ticks later. It must not shrink below that.
ticks = re.search(r"#define NETPLAY_EARLY_IDLE_TICKS (\d+)u", MAIN)
assert ticks, "NETPLAY_EARLY_IDLE_TICKS is not defined"
assert int(ticks.group(1)) >= 48, "the idle window must outlast the first confirmed interval snapshots"
assert int(ticks.group(1)) <= 120, "the idle window must stay inside the BIOS boot: about two seconds at most"

# Inside the window the pad is idle: no button, sticks centred. Outside it the pad is untouched.
assert "if (psx_netplay_sim_tick() >= NETPLAY_EARLY_IDLE_TICKS) return;" in body, (
    "the rule must follow the session's sim tick and leave later pads alone"
)
assert "pad->buttons = 0xFFFFu;" in body, "an idle pad has every button released (active low)"
assert "pad->lx = pad->ly = pad->rx = pad->ry = 0x80u;" in body, "an idle pad has centred sticks"
assert "analog" not in body and "connected" not in body, "the pad type and connection must be kept"

# Every source passes through it, at the one place the local pad is staged,
# after the title's input transform so the staged pad itself is idle.
call = "netplay_early_input_idle(&local);"
calls = [m.start() for m in re.finditer(re.escape(call), MAIN)]
assert len(calls) == 1, "the rule must be applied in exactly one place"
stages = [m.start() for m in re.finditer(r"psx_netplay_stage_local\(&local\);", MAIN)]
assert len(stages) == 1, "expected one place that stages the local pad"
between = MAIN[calls[0] + len(call) : stages[0]]
assert calls[0] < stages[0] and re.fullmatch(r"\s*(/\*.*?\*/)?\s*", between, re.S), (
    "the rule must sit directly before the pad is staged"
)
before = MAIN[calls[0] - 600 : calls[0]]
assert "capture_local_human_pad(&local);" in before, "the rule must come after the pad is captured"
transform = before.rfind("netplay_transform_local_pad(&local, override >= 0);")
assert transform >= 0, "the rule must come after the local pad transform"
assert before[:transform].rstrip().endswith("}"), (
    "the rule must apply to every source, not to one branch of the capture"
)

print("netplay early input idle wiring test: PASS")
