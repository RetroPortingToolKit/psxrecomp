"""Every generated BIOS profile's kernel-bless table must fit the runtime.

memory.c turns kernel bless off for the whole run when the active image's
<stem>_psx_bios_kernel_body_count exceeds KBLESS_MAX_ENTRIES. Every relocated
kernel routine then runs in the dirty-RAM interpreter, and nothing at start-up
says so (the run report's "kbless" object shows it as disabled_by_overflow).

The test reads each generated <stem>_dispatch.c in the build tree and fails
when a table has more rows than the capacity, so a table that outgrows it is
seen when it is generated. It exits 77 (skipped) when no generated BIOS is
present.
"""
import argparse
import re
import sys
from pathlib import Path

CAP = re.compile(r"#define\s+KBLESS_MAX_ENTRIES\s+(\d+)u?")
COUNT = re.compile(r"enum\s*\{\s*(\w+?)_psx_bios_kernel_body_count\s*=\s*(\d+)u?\s*\};")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated", required=True, type=Path)
    parser.add_argument("--memory-c", required=True, type=Path)
    args = parser.parse_args()

    caps = CAP.findall(args.memory_c.read_text(encoding="utf-8"))
    if len(caps) != 1:
        print(f"FAIL: expected one KBLESS_MAX_ENTRIES in {args.memory_c}, found {len(caps)}")
        return 1
    cap = int(caps[0])

    found = {}
    for path in sorted(args.generated.glob("*_dispatch.c")) if args.generated.is_dir() else []:
        for stem, count in COUNT.findall(path.read_text(encoding="utf-8", errors="replace")):
            found[stem] = int(count)
    if not found:
        print(f"SKIP: no generated BIOS tables under {args.generated}")
        return 77

    failures = []
    for stem, count in sorted(found.items()):
        print(f"{stem}: {count} rows, capacity {cap}")
        if count > cap:
            failures.append(f"{stem} has {count} rows, more than the capacity {cap}: "
                            "kernel bless is off for the whole run on this profile")
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print(f"PASS: {len(found)} profile(s) examined; every table fits")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
