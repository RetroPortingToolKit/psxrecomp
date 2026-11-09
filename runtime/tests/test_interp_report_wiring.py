#!/usr/bin/env python3
"""Guard that the run report's "interp_detail" object gets its data (PS1B-391).

runtime/tests/test_interp_report.c proves the builder: given tables, it writes
the 32 hottest interpreted addresses with their place, the text guard's fields
and the loader's misses by reason. This proves the three places that feed it:

  1. every miss exit of overlay_loader_dispatch counts under a reason. A bare
     DISP_INTERP() there would raise the total without a reason, and the
     reasons would no longer add up to disp_interp;
  2. every reason of the enum has a name of its own;
  3. the report writer fills the builder's input from the live counters and
     writes the object, and the runtime's source list builds the unit.

Reading source rather than running a binary is deliberate: the loader's
dispatch and the report writer need a running game.
"""

import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
ROOT = parser.parse_args().root
WRITER = (ROOT / "runtime" / "src" / "crash_trace.c").read_text(encoding="utf-8")
assert "disp_interp_after_game_entry" in WRITER, "run report omits the post-game-entry interpreter field"
LOADER = (ROOT / "runtime" / "src" / "overlay_loader.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "interp_report.h").read_text(encoding="utf-8")
BUILDER = (ROOT / "runtime" / "src" / "interp_report.c").read_text(encoding="utf-8")
SOURCES = (ROOT / "runtime" / "runtime.cmake").read_text(encoding="utf-8")

# ---- 1. every miss exit of the dispatch has a reason ------------------------
start = LOADER.index("static int overlay_loader_dispatch_seg(CPUState *cpu, uint32_t addr) {")
end = LOADER.index("/* ---- Self-modification", start)
dispatch = LOADER[start:end]
exits = re.findall(r"record_interp_miss\(phys,([^;]+)\);\s*s_disp_interp\+\+;", dispatch)
assert len(exits) == dispatch.count("s_disp_interp++"), "every original total increment has exactly one reason"
assert len(exits) == 14, f"expected fourteen public dispatch miss exits, found {len(exits)}"
reasons = re.findall(r"^\s*(PSX_MISS_[A-Z_]+)\b", HEADER[HEADER.index("enum {"):HEADER.index("PSX_INTERP_MISS_REASONS")],
                     flags=re.M)
assert len(reasons) == 10, reasons
for text in exits:
    named = re.findall(r"PSX_MISS_[A-Z_]+", text)
    assert named and all(n in reasons for n in named), f"a miss exit names no known reason: {text!r}"
used = set(re.findall(r"PSX_MISS_[A-Z_]+", " ".join(exits)))
assert used == set(reasons), f"reasons never counted: {sorted(set(reasons) - used)}"
assert "s_disp_miss[phys < DIRTY_RAM_KERNEL_WINDOW_END ? 1 : 0][reason]++;" in LOADER
assert "static uint64_t s_disp_miss[2][PSX_INTERP_MISS_REASONS];" in LOADER
assert re.search(r"void overlay_loader_get_miss_reasons\(uint64_t above_kernel\[PSX_INTERP_MISS_REASONS\],\s*"
                 r"uint64_t kernel\[PSX_INTERP_MISS_REASONS\]\)", LOADER), "the report reads the reasons through this getter"

# ---- 2. every reason has its own name ---------------------------------------
names = dict(re.findall(r"case (PSX_MISS_[A-Z_]+):\s+return \"([a-z_]+)\";", BUILDER))
assert sorted(names) == sorted(reasons), f"a reason without a name: {sorted(set(reasons) - set(names))}"
assert len(set(names.values())) == len(names), "two reasons share a name"

# ---- 3. the writer fills the input and writes the object --------------------
block = WRITER[WRITER.index("PsxInterpReportInput in;"):]
block = block[:block.index('append_str(buf, sizeof(buf), &pos, ",\\n");')]
for needle in (
    "dirty_ram_text_image_range(&in.guard_lo, &in.guard_hi)",
    "dirty_ram_text_native_blocked()",
    "dirty_ram_text_diverged_pages()",
    "dirty_ram_text_exact_mismatch_stats(&in.exact_mismatches, in.exact_last)",
    "g_text_image_lo",
    "g_overlay_region_floor",
    "g_dirty_ram_blocks_run",
    "g_dirty_ram_pc_table",
    "DIRTY_RAM_PC_TABLE_SIZE",
    "overlay_loader_get_miss_reasons(in.miss[0], in.miss[1])",
    "psx_interp_report_json(id_report, (int)sizeof(id_report), &in)",
    '"  \\"interp_detail\\": "',
):
    assert needle in block, f"the report writer no longer has: {needle}"
assert "runtime/src/interp_report.c" in SOURCES, "runtime.cmake must build interp_report.c"

print("interp_report wiring: 14 public miss exits, 10 reasons, writer and source list OK")
