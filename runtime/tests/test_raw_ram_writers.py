"""Every writer of guest RAM keeps the kernel-bless table honest.

Kernel bless runs a relocated kernel routine's compiled body only while its
bytes in RAM still equal the ROM source. The row state follows RAM writes:

  * a store through psx_write_word/half/byte reports itself: the three store
    functions call dirty_ram_mark_kernel_write(phys) before they write;
  * every other writer of guest RAM calls psx_kernel_bless_note_range next to
    the write (or, at boot, psx_kernel_bless_reset_for_boot).

Marking a range executable (dirty_ram_mark_executable_range) is not a write
and must not reset the table: the generated BIOS dispatch does it on every
RAM-alias dispatch of an exception-handler key, and resetting there re-verified
the whole table about a million times in a two-minute start.

This test reads the runtime sources and lists every statement that writes
through a guest-RAM pointer without psx_write_*. Each one must be a known
writer below and must sit next to its notification. A new raw writer fails the
test until it is added here with its notification, and a listed writer that
disappears fails too, so the list cannot go stale.

It finds the idioms the runtime uses (an indexed store, or memcpy/memset/
memmove/fread into the pointer). It is a guard on those, not a proof: the
run-time proof is PSX_KERNEL_BLESS_PARANOID=1, which must count 0 stale rows.
"""
import argparse
import re
import sys
from pathlib import Path

# (file, text on the writing line, rule, why)
#   hook  : dirty_ram_mark_kernel_write(phys) within the 12 lines before
#   note  : psx_kernel_bless_note_range( on the line or within 6 lines after
#   reset : psx_kernel_bless_reset_for_boot( within 20 lines after
WRITERS = [
    ("memory.c", "ram[phys]     = (uint8_t)(val);", "hook", "psx_write_word / psx_write_half"),
    ("memory.c", "ram[phys + 1] = (uint8_t)(val >> 8);", "hook", "psx_write_word / psx_write_half"),
    ("memory.c", "ram[phys + 2] = (uint8_t)(val >> 16);", "hook", "psx_write_word"),
    ("memory.c", "ram[phys + 3] = (uint8_t)(val >> 24);", "hook", "psx_write_word"),
    ("memory.c", "ram[phys] = val;", "hook", "psx_write_byte"),
    ("memory.c", "memset(ram, 0, 0x10u);", "note", "low boot scratch clear at game entry"),
    ("memory.c", "memset(ram, 0, sizeof(ram));", "reset", "memory_init"),
    ("boot_state.c", "memcpy(memory_get_ram_ptr(), p, memory_get_ram_bytes());", "note", "state restore"),
    ("overlay_loader.c", "memcpy(ram,  s_ram0,  SHADOW_RAM_SIZE);", "note", "shadow diff: rewind to the entry state"),
    ("overlay_loader.c", "memcpy(ram,  s_ramI,  SHADOW_RAM_SIZE);", "note", "shadow diff: restore the interpreter result"),
    ("text_xlate.cpp", "ram[pa] = v;", "note", "translation patches (gwb)"),
    ("cosim_state.c", "ram[s_inj_ram_phys] ^= s_inj_ram_xor;", "note", "cosim gate-4 injection"),
    ("render_pass.c", "memcpy(memory_get_ram_ptr(), s_ram_copy, s_ck.ram_bytes);", "note", "render-pass checkpoint restore"),
]

BIND = re.compile(r"(\w+)\s*=\s*(?:\([^)]*\)\s*)?(?:memory_get_ram_ptr\s*\(\s*\)|g_psx_ram\b)")
ASSIGN = r"\s*\[[^\]]*\]\s*(?:=[^=]|\^=|\|=|&=|\+=|-=|<<=|>>=)"
BULK = r"\b(?:memcpy|memset|memmove|fread)\s*\(\s*&?\s*\(?\s*(?:\(\s*\w+\s*\*\s*\)\s*)?"


def raw_writes(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    if not re.search(r"memory_get_ram_ptr|g_psx_ram", text):
        return [], text.splitlines()
    # "ram" is the name every user gives the pointer, including as a parameter
    # (text_xlate.cpp); memory.c owns the array itself under that name.
    names = {m.group(1) for m in BIND.finditer(text)} | {"g_psx_ram", "ram"}
    names -= {"result", "value", "word", "crc", "h"}
    alt = "|".join(sorted(re.escape(n) for n in names))
    store = re.compile(r"(?<![\w>.])(?:" + alt + r")" + ASSIGN)
    bulk = re.compile(BULK + r"(?:(?:" + alt + r")\b(?!\s*(?:->|\.))|memory_get_ram_ptr\s*\(\s*\))")
    lines = text.splitlines()
    hits = []
    for number, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith(("//", "*", "/*")):
            continue
        if store.search(line) or bulk.search(line):
            hits.append(number)
    return hits, lines


def rule_holds(rule, lines, number):
    if rule == "hook":
        return any("dirty_ram_mark_kernel_write(phys)" in l for l in lines[max(0, number - 12):number])
    if rule == "note":
        return any("psx_kernel_bless_note_range(" in l for l in lines[number:number + 7])
    if rule == "reset":
        return any("psx_kernel_bless_reset_for_boot(" in l for l in lines[number:number + 21])
    raise ValueError(rule)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", required=True, type=Path)
    parser.add_argument("--include", required=True, type=Path)
    args = parser.parse_args()

    files = sorted(p for root in (args.src, args.include)
                   for ext in ("*.c", "*.cpp", "*.h") for p in root.rglob(ext))
    failures, seen, found = [], set(), 0
    for path in files:
        hits, lines = raw_writes(path)
        for number in hits:
            found += 1
            line = lines[number]
            entry = next((w for w in WRITERS if w[0] == path.name and w[1] in line), None)
            where = f"{path.name}:{number + 1}"
            if entry is None:
                failures.append(f"{where}: unlisted raw write to guest RAM: {line.strip()}")
                continue
            seen.add(entry)
            if not rule_holds(entry[2], lines, number):
                failures.append(f"{where}: {entry[3]}: missing its kernel-bless "
                                f"notification (rule '{entry[2]}')")
    for entry in WRITERS:
        if entry not in seen:
            failures.append(f"{entry[0]}: listed writer not found: {entry[1]} ({entry[3]})")

    # The marker itself must stay a non-writer.
    memory = (args.src / "memory.c").read_text(encoding="utf-8", errors="replace")
    body = re.search(r"\nvoid dirty_ram_mark_executable_range\([^)]*\)\s*\{(.*?)\n\}", memory, re.S)
    if not body:
        failures.append("memory.c: dirty_ram_mark_executable_range not found")
    elif "psx_kernel_bless" in body.group(1):
        failures.append("memory.c: dirty_ram_mark_executable_range resets the kernel-bless "
                        "table again; it writes no RAM and must not")

    print(f"files scanned: {len(files)}; raw guest-RAM write statements: {found}; "
          f"listed writers: {len(WRITERS)}")
    for entry in WRITERS:
        print(f"  {entry[0]:18s} {entry[2]:5s} {entry[3]}")
    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("PASS: every raw guest-RAM writer notifies the kernel-bless table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
