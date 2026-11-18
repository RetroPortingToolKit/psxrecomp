#!/usr/bin/env python3
"""Guard that the run report carries the silent-read counters.

test_cdrom_silent_read_counter.c proves what the drive counts. This proves the
numbers reach psx_last_run_report.json, which is the only place a release
build shows CD state: the report writer links into the game runtime only, so
its wiring is checked in the source. An optional argument names another
repository root to check.
"""

import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
REPORT = (ROOT / "runtime" / "src" / "crash_trace.c").read_text(encoding="utf-8")
CDROM = (ROOT / "runtime" / "src" / "cdrom.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "cdrom.h").read_text(encoding="utf-8-sig")

assert "void cdrom_silent_read_stats(uint32_t *starts, uint32_t *silent, uint32_t *longest_run);" in HEADER, (
    "cdrom.h has no cdrom_silent_read_stats"
)

call = REPORT.index("cdrom_silent_read_stats(&starts, &silent, &longest_run);")
fmt = re.search(
    r'"  \\"cdrom\\": \{\\"read_starts\\": %u, \\"silent_reads\\": %u, "\s*'
    r'"\\"silent_run_max\\": %u\},\\n",\s*starts, silent, longest_run\);',
    REPORT[call:],
)
assert fmt, 'the report must write "cdrom": {"read_starts", "silent_reads", "silent_run_max"} from the counters'

# The counters are fed where a stream starts and where one ends.
assert re.search(
    r"cd_stream_account_end\(0\);[^\n]*\n\s*s_cd_stream_open_seq = s_cd_timing_total;\s*\n\s*s_cd_stream_starts\+\+;",
    CDROM,
), "start_read_stream must close the previous stream and open the new one"
assert re.search(r"static void stop_read_stream\(void\) \{\s*cd_stream_account_end\(0\);", CDROM), (
    "stop_read_stream must account the stream it ends"
)


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    depth = 0
    for i in range(text.index("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise AssertionError(f"unterminated body: {signature}")


# Outside their declaration, the accounting function, the getter and the one
# increment at a read start, no code reads or writes the counters: they must
# not steer the drive.
rest = CDROM
for part in (
    body(CDROM, "static void cd_stream_account_end(int delivered) {"),
    body(CDROM, "void cdrom_silent_read_stats("),
    "static uint32_t s_cd_stream_starts, s_cd_silent_reads, s_cd_silent_run, s_cd_silent_run_max;",
    "s_cd_stream_starts++;",
):
    assert rest.count(part) == 1, part[:60]
    rest = rest.replace(part, "")
for name in ("s_cd_silent_reads", "s_cd_silent_run_max", "s_cd_silent_run", "s_cd_stream_starts"):
    assert not re.search(name, rest), f"{name} is used outside the accounting"

print("run report cdrom counters: ok")
