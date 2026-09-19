#!/usr/bin/env python3
"""Keep SDL3's single-header entry-point implementation in main.cpp only."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
SDL_MAIN_HEADER = "<SDL3/SDL_main.h>"


def main() -> int:
    runtime = ROOT / "runtime"
    owners = []
    for pattern in ("*.c", "*.cpp", "*.h"):
        for path in runtime.rglob(pattern):
            source = path.read_text(encoding="utf-8")
            if SDL_MAIN_HEADER in source:
                owners.append(path.relative_to(ROOT).as_posix())

    if owners != ["runtime/src/app/main.cpp"]:
        raise AssertionError(
            "SDL_main.h must be included exactly once by runtime/src/app/main.cpp; "
            f"found {owners}"
        )

    main_cpp = (runtime / "src/main.cpp").read_text(encoding="utf-8")
    guard = (
        "#if defined(PSX_SDL3)\n"
        "/*\n"
        " * SDL_main.h is a single-header implementation"
    )
    if guard not in main_cpp:
        raise AssertionError("SDL_main.h include is no longer guarded by PSX_SDL3")

    print("PASS: SDL3 entry-point implementation has one owner")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
