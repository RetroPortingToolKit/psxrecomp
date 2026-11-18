#!/usr/bin/env python3
"""No two emitted function bodies may cover the same guest instruction.

Usage: python test_no_overlapping_bodies.py [--recompiler <psxrecomp-game.exe>]

The mid-function split pre-pass in CodeGenerator::generate_file turns branch
targets that no function owns into new entry points. `func_starts` is a snapshot
taken before the pass creates anything, so every "gap" target used to take the
SAME end -- the next pre-existing function start. Two targets in one gap then
produced two bodies that both ran to the end of the gap, and n targets in one
gap re-emitted the gap's tail n times.

The shape is a staircase: nested starts, one shared terminus. On Galerians
(SLES_023.28) it put 66% of functions in an overlap and cost 440 MB of
duplicated bodies against 17 MB of real code -- a 27x amplification -- with
2,972 entries starting inside func_80198B44 alone.

Alias entries are a DIFFERENT and legitimate overlap: an alias deliberately
covers its host's range and is emitted as a thin wrapper around one shared
`psx_alias_body_<host>`, so no code is duplicated. They are excluded here, and
are identifiable straight from the manifest: ControlFlowAnalyzer::analyze_function
walks an alias from `alias_walk_lo` (its HOST's start), so an alias's lowest
emitted range begins strictly below its own entry address. A real entry's
lowest range always begins at the entry itself.
"""
import argparse
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOAD = 0x80010000
SIZE = 0x400

NOP = 0x00000000
JR_RA = 0x03E00008
PROLOGUE = 0x27BDFFE0  # addiu $sp,$sp,-0x20
ADDIU_V0 = 0x24420001  # addiu $v0,$v0,1 -- filler: not a return, not a prologue

# Two branch targets in one unowned gap. Before the fix both bodies ran to the
# image end; they must now partition the gap at GAP_B.
GAP_A = LOAD + 0x200
GAP_B = LOAD + 0x240


def put32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def make_psxexe():
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    put32(header, 0x10, LOAD)   # entry point
    put32(header, 0x18, LOAD)   # load address
    put32(header, 0x1C, SIZE)
    text = bytearray(SIZE)

    # Entry function: calls the second function, then jumps into the gap.
    for i, word in enumerate((PROLOGUE, jal(LOAD + 0x100), NOP,
                              j(GAP_A), NOP, JR_RA, NOP)):
        put32(text, i * 4, word)

    # Second function (reached by the direct JAL) jumps to the other gap target.
    for i, word in enumerate((PROLOGUE, j(GAP_B), NOP, JR_RA, NOP)):
        put32(text, 0x100 + i * 4, word)

    # The gap itself: straight-line filler with no return and no prologue, so
    # boundary detection never mints a function here and both targets land in
    # one unowned gap.
    for offset in range(0x200, 0x300, 4):
        put32(text, offset, ADDIU_V0)

    return bytes(header) + text


def parse_ranges(text):
    """-> [(entry, [(lo, hi), ...])] in manifest order."""
    funcs, cur = [], None
    for line in text.splitlines():
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        if parts[0] == "F":
            cur = (int(parts[1], 16), [])
            funcs.append(cur)
        elif parts[0] == "R" and cur is not None:
            lo = int(parts[1], 16)
            cur[1].append((lo, lo + int(parts[2], 16)))
    return funcs


def overlapping_pairs(funcs, ignore_delay_slot_clone=False):
    """Real-entry pairs sharing emitted bytes -> {(a, b): shared_bytes}.

    Alias entries (lowest range starts below the entry) are excluded: they
    share one emitted body and duplicate nothing.

    On a real title every surviving pair overlaps by exactly one instruction:
    generate_ranges_manifest extends a range by 4 when translate_basic_block
    clones an out-of-block delay slot, and that word legitimately belongs to
    both blocks. Pass ignore_delay_slot_clone to drop those. The fixture below
    keeps the strict form -- its regression overlapped by 448 bytes.
    """
    items = sorted(
        (lo, hi, entry)
        for entry, ranges in funcs
        if not (ranges and min(lo for lo, _ in ranges) < entry)
        for lo, hi in ranges)

    pairs, active = {}, []
    for lo, hi, entry in items:
        active = [a for a in active if a[1] > lo]
        for alo, ahi, aentry in active:
            if aentry == entry:
                continue
            shared = min(ahi, hi) - max(alo, lo)
            if shared > 0:
                key = (min(aentry, entry), max(aentry, entry))
                pairs[key] = pairs.get(key, 0) + shared
        active.append((lo, hi, entry))
    if ignore_delay_slot_clone:
        pairs = {k: n for k, n in pairs.items() if n > 4}
    return pairs


def describe(pairs, limit=10):
    worst = sorted(pairs.items(), key=lambda kv: -kv[1])[:limit]
    return "; ".join(f"func_{a:08X}/func_{b:08X} share {n} bytes"
                     for (a, b), n in worst)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler",
                        default=os.environ.get("PSXRECOMP_GAME",
                                               "recompiler/build/psxrecomp-game"))
    parser.add_argument("--manifest",
                        help="audit an already-generated *_full.ranges instead "
                             "of running the fixture")
    args = parser.parse_args()

    if args.manifest:
        funcs = parse_ranges(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
        pairs = overlapping_pairs(funcs, ignore_delay_slot_clone=True)
        aliases = sum(1 for e, r in funcs
                      if r and min(lo for lo, _ in r) < e)
        print(f"{args.manifest}: {len(funcs)} entries "
              f"({aliases} aliases, shared body)")
        if pairs:
            print(f"FAIL: {len(pairs)} overlapping body pair(s): {describe(pairs)}")
            return 1
        print("PASS: no overlapping function bodies")
        return 0
    failures = []
    # The subprocess runs with cwd=ROOT for BIOS-profile resolution, but on
    # Windows a relative executable resolves against OUR cwd, not that one.
    recompiler = os.path.abspath(args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "gapsplit.psx")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as handle:
            handle.write(make_psxexe())

        # No --overlay: overlay mode disables split_mid_function_targets, which
        # is the pass under test.
        result = subprocess.run(
            [recompiler, psx, "--out-dir", out,
             "--project-root", str(ROOT)],
            capture_output=True, text=True, encoding="utf-8", cwd=str(ROOT))
        if result.returncode != 0:
            print("FAIL: recompiler exited", result.returncode)
            print(result.stderr or result.stdout)
            return 1

        manifests = list(pathlib.Path(out).glob("*_full.ranges"))
        if not manifests:
            print("FAIL: no .ranges manifest written to", out)
            return 1
        funcs = parse_ranges(manifests[0].read_text(encoding="utf-8"))

    entries = {entry for entry, _ in funcs}
    # Guard the fixture itself: if the pre-pass stopped minting these, the test
    # would pass vacuously and stop protecting anything.
    for name, addr in (("GAP_A", GAP_A), ("GAP_B", GAP_B)):
        if addr not in entries:
            failures.append(
                f"fixture no longer exercises the split pre-pass: "
                f"{name} 0x{addr:08X} is not an entry in the manifest")

    spans = {entry: (min(lo for lo, _ in r), max(hi for _, hi in r))
             for entry, r in funcs if r}
    if GAP_A in spans and GAP_B in spans and spans[GAP_A][1] > GAP_B:
        failures.append(
            f"gap targets did not partition their gap: func_{GAP_A:08X} ends at "
            f"0x{spans[GAP_A][1]:08X}, past the sibling target 0x{GAP_B:08X}")

    pairs = overlapping_pairs(funcs)
    if pairs:
        failures.append(
            f"{len(pairs)} overlapping body pair(s): {describe(pairs)}")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print(f"PASS: {len(funcs)} entries, no overlapping function bodies")
    return 0


if __name__ == "__main__":
    sys.exit(main())
