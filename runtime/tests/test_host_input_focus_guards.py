#!/usr/bin/env python3
"""Every read of SDL's cached keyboard array in main.cpp checks window focus.

SDL_GetKeyboardState() returns a cached array. A key pressed while another
application had focus can stay set in it, so a reader that skips the
host_hotkey_input_focused() check lets an inactive window drive the guest or
the host menus. host_hotkey_input_focused() reports focus when there is no
window, so headless runs keep reading the keys.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
LINES = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8").splitlines()
SIGNATURE = re.compile(r'^(static |extern "C" )?[A-Za-z_][\w :<>*&]*\b(\w+)\(')
GUARD = "host_hotkey_input_focused()"


def enclosing(line_no: int) -> tuple[str, list[str]]:
    start = line_no
    while start >= 0 and not (SIGNATURE.match(LINES[start]) and not LINES[start].endswith(";")):
        start -= 1
    end = line_no
    while end < len(LINES) and LINES[end] != "}":
        end += 1
    return SIGNATURE.match(LINES[start]).group(2), LINES[start:end + 1]


def body_of(name: str) -> str:
    for i, line in enumerate(LINES):
        m = SIGNATURE.match(line)
        if m and m.group(2) == name and not line.endswith(";"):
            return "\n".join(enclosing(i + 1)[1])
    raise AssertionError(f"{name} not found")


def main() -> int:
    failures = []
    readers = []
    for i, line in enumerate(LINES):
        code = line.split("//")[0]
        if "SDL_GetKeyboardState(" not in code or code.lstrip().startswith(("*", "/*")):
            continue
        name, body = enclosing(i)
        readers.append(name)
        if not any(GUARD in b for b in body):
            failures.append(f"{name} (line {i + 1}) reads the key array without a focus check")

    # Readers whose whole result comes from the key array: the check comes first.
    for name in ("pad_from_keyboard", "pad_sticks_for"):
        text = body_of(name)
        if GUARD in text and text.index(GUARD) > text.index("SDL_GetKeyboardState("):
            failures.append(f"{name} checks focus after reading the key array")

    # Readers that mix direct keys with pad input: every direct key term is gated.
    for name in ("savestate_menu_poll_nav", "rewind_poll_nav"):
        text = body_of(name)
        ungated = [m.group(0) for line in text.splitlines() if "kb &&" not in line
                   for m in re.finditer(r"keys\[SDL_SCANCODE_\w+\]", line)]
        if ungated:
            failures.append(f"{name} reads {', '.join(ungated)} without the focus check")

    if len(readers) < 10:
        failures.append(f"expected at least 10 key-array readers, found {len(readers)}: {readers}")
    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    print(f"host input focus guards: {len(readers)} key-array readers check focus")
    return 0


if __name__ == "__main__":
    sys.exit(main())
