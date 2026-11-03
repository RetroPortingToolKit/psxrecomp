#!/usr/bin/env python3
"""Guard: the frame-trace RECORD path must not allocate or touch files.

frame_trace.c exists so a visible VR hitch can be correlated with a stage
without changing the thing being measured. A malloc(), a stdio file write, or a
formatted print added to the per-frame path would perturb the frame it is
supposed to observe, and no functional test would notice. This check reads the
source and fails if such a call appears in a record-path function.

Export functions (to_json/to_csv/to_chrome_json) are explicitly allowed to use
vsnprintf, and are never called from the record path; they are listed as
exceptions by name so the intent is not mistaken for an oversight.

ctest -R frame_trace_guard_test
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "runtime" / "src" / "frame_trace.c"

# Functions that run per frame. Their bodies must stay allocation- and IO-free.
RECORD_PATH = [
    "psx_frame_trace_open",
    "psx_frame_trace_commit",
    "psx_frame_trace_add",
    "psx_frame_trace_ids",
    "psx_frame_trace_attempt",
    "psx_frame_trace_result",
    "psx_frame_trace_source",
    "psx_frame_trace_draw",
    "psx_frame_trace_eye_cycles",
    "psx_frame_trace_gpu",
    "psx_frame_trace_flag",
    "psx_frame_trace_note",
    "psx_frame_trace_io",
    "psx_frame_trace_reset",
    "psx_frame_trace_enabled",
    "stamp_now",
    "open_record",
]

# Forbidden call patterns in a record-path body. Word boundaries matter:
# "printf" must not match "snprintf"/"vsnprintf", which write into the record's
# own fixed buffers and touch neither heap nor files.
FORBIDDEN = [
    r"(?<![A-Za-z_])printf\s*\(",
    r"(?<![A-Za-z_])puts\s*\(",
    r"(?<![A-Za-z_])malloc\s*\(",
    r"(?<![A-Za-z_])calloc\s*\(",
    r"(?<![A-Za-z_])realloc\s*\(",
    r"(?<![A-Za-z_])free\s*\(",
    r"(?<![A-Za-z_])strdup\s*\(",
    r"(?<![A-Za-z_])fopen\s*\(",
    r"(?<![A-Za-z_])fclose\s*\(",
    r"(?<![A-Za-z_])fwrite\s*\(",
    r"(?<![A-Za-z_])fread\s*\(",
    r"(?<![A-Za-z_])fprintf\s*\(",
    r"(?<![A-Za-z_])fflush\s*\(",
    r"(?<![A-Za-z_])fputs\s*\(",
    r"(?<![A-Za-z_])fputc\s*\(",
    r"(?<![A-Za-z_])fseek\s*\(",
]


def function_body(text: str, name: str) -> str | None:
    """The definition of `name` (top-level, so the signature starts at column 0)."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if re.match(r"^[A-Za-z_][^\n]*\b" + re.escape(name) + r"\s*\(", line):
            body = [line]
            for j in range(i + 1, len(lines)):
                body.append(lines[j])
                if lines[j].rstrip() == "}":
                    break
            return "\n".join(body)
    return None


def main() -> int:
    if not SRC.exists():
        print(f"frame_trace_guard_test: SKIP: {SRC} not found", file=sys.stderr)
        return 77
    text = SRC.read_text(encoding="utf-8", errors="replace")

    failures: list[str] = []

    if not re.search(r"static\s+PSXFrameTraceRecord\s+s_ring\s*\[", text):
        failures.append("frame_trace.c: the ring is not a static array "
                        "(the record path must not own heap)")

    for name in RECORD_PATH:
        body = function_body(text, name)
        if body is None:
            failures.append(f"frame_trace.c: record-path function {name}() not found")
            continue
        for pattern in FORBIDDEN:
            if re.search(pattern, body):
                failures.append(f"frame_trace.c: {name}() matches {pattern} "
                                "(the per-frame path must not allocate or do IO)")

    if failures:
        print("frame_trace_guard_test: FAIL", file=sys.stderr)
        for f in failures:
            print("  " + f, file=sys.stderr)
        return 1

    print(f"frame_trace_guard_test: {len(RECORD_PATH)} record-path functions are "
          "allocation- and IO-free")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
