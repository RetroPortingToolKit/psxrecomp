#!/usr/bin/env python3
"""Derive a retail BIOS seed corpus by filtering SCPH-1001's against another dump.

Every retail PSX BIOS of the v2.2/v3.0/v4.x line shares its boot code and kernel
with SCPH-1001 and diverges in the shell.  A new image therefore does not need a
Ghidra run: take SCPH-1001's seed corpus, keep every seed whose surrounding bytes
are identical in the target ROM, and drop the rest.  That is exactly how
phase2_ghidra_seeds_SCPH101.json and _SCPH5552.json were produced -- their
`source` field records it -- but the tool that produced them was removed in
f55c731c, whose "no tracked file references" sweep was correct and still missed
this: the only reference was a provenance string inside generated data.

This is that tool, rebuilt (CLAUDE.md rule 15: a known-broken tool is the task).

It doubles as the kernel-identity check.  A seed survives only if `window` bytes
at its address match, so the kept/dropped split IS the measurement of where the
target diverges from the reference -- printed on every run, and the evidence a
new bios/<STEM>.toml needs before it carries SCPH-1001's address model.

  tools/gen_retail_bios_seeds.py --target bios/SCPH5501.BIN

  tools/gen_retail_bios_seeds.py --target bios/SCPH5552.BIN \
      --check recompiler/seeds/phase2_ghidra_seeds_SCPH5552.json

The base corpus is never written.  It feeds SCPH-1001's emitter fingerprint, so
editing it would force every retail BIOS in the fleet to regenerate.
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_BASE = "recompiler/seeds/phase2_ghidra_seeds.json"
DEFAULT_REFERENCE = "bios/SCPH1001.BIN"
ROM_BASE_PHYS = 0x1FC00000
KSEG_MASK = 0x1FFFFFFF


def model_token(path):
    """Last '-'-separated piece of the filename stem, uppercased.

    Mirrors PSXRecompV4::bios_model_token (recompiler/include/bios_rom_alias.h)
    so a region-qualified dump ("EUR-PSX-SCPH5502.bin") yields the same stem the
    C++ side would resolve.
    """
    stem = os.path.splitext(os.path.basename(path))[0]
    return stem.rsplit("-", 1)[-1].upper()


def image_stem(path):
    """A usable backend stem for a dump filename.

    model_token() mirrors the C++ rule exactly, which is right for MATCHING a
    wanted model but wrong for DERIVING an identifier: on the dashed spelling
    "SCPH-5501.BIN" — a name psx_known_bios_filenames() itself probes for — the
    last '-'-separated piece is "5501". That is not a legal backend stem
    (runtime.cmake requires ^[A-Za-z_][A-Za-z0-9_]*$, so a leading digit is a
    hard configure error) and it would name the emitted symbol prefix.

    So: take the model token when it is already a legal stem, else fold the
    dashes out of the whole filename stem. "SCPH-5501" -> "SCPH5501".
    """
    token = model_token(path)
    if token and (token[0].isalpha() or token[0] == "_") and token.isalnum():
        return token
    folded = os.path.splitext(os.path.basename(path))[0].replace("-", "").upper()
    if not folded or not (folded[0].isalpha() or folded[0] == "_"):
        sys.exit("error: cannot derive a backend stem from %r; pass --stem" % path)
    return folded


def rom_offset(address):
    """Guest address -> ROM file offset, tolerating KUSEG/KSEG0/KSEG1 spellings."""
    return (int(address, 16) & KSEG_MASK) - ROM_BASE_PHYS


def load_rom(path, label):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data:
        sys.exit("error: %s ROM is empty: %s" % (label, path))
    return data


def describe_divergence(reference, target):
    """First differing byte offset, or None when the images are identical."""
    limit = min(len(reference), len(target))
    for offset in range(limit):
        if reference[offset] != target[offset]:
            return offset
    if len(reference) != len(target):
        return limit
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", required=True,
                        help="the BIOS dump to derive a corpus for")
    parser.add_argument("--reference", default=DEFAULT_REFERENCE,
                        help="the dump the base corpus was built from (default: %(default)s)")
    parser.add_argument("--base", default=DEFAULT_BASE,
                        help="seed corpus to filter (default: %(default)s); never written")
    parser.add_argument("--out", help="output corpus (default: derived from --stem)")
    parser.add_argument("--stem", help="image stem for the output name and provenance "
                                       "(default: model token of --target)")
    parser.add_argument("--window", type=int, default=64,
                        help="bytes compared at each seed address (default: %(default)s)")
    parser.add_argument("--check", metavar="CORPUS",
                        help="compare against an existing corpus and exit; writes nothing")
    args = parser.parse_args()

    def resolve(path):
        return path if os.path.isabs(path) else os.path.join(ROOT, path)

    if args.window <= 0:
        sys.exit("error: --window must be positive")

    stem = args.stem or image_stem(args.target)
    base_path = resolve(args.base)
    with open(base_path) as handle:
        base = json.load(handle)

    reference = load_rom(resolve(args.reference), "reference")
    target = load_rom(resolve(args.target), "target")
    if len(reference) != len(target):
        sys.exit("error: ROM sizes differ (reference %d, target %d); these are not "
                 "the same class of image" % (len(reference), len(target)))

    kept, dropped = [], []
    for seed in base.get("seeds", []):
        offset = rom_offset(seed["address"])
        if offset < 0 or offset >= len(target):
            dropped.append((seed, "outside ROM"))
            continue
        end = min(offset + args.window, len(target))
        if reference[offset:end] == target[offset:end]:
            kept.append(seed)
        else:
            dropped.append((seed, "window differs"))

    out = {
        "schema": base.get("schema", "psxrecomp phase2 seeds"),
        "source": "%s filtered vs %s ROM (%d-byte window)"
                  % (os.path.basename(base_path), stem, args.window),
        "seed_count": len(kept),
        "seeds": kept,
    }
    if "excluded" in base:
        out["excluded"] = base["excluded"]
    text = json.dumps(out, indent=2)

    first_diff = describe_divergence(reference, target)
    print("stem              : %s" % stem)
    print("reference         : %s" % args.reference)
    print("target            : %s" % args.target)
    print("ROM bytes         : %d" % len(target))
    if first_diff is None:
        print("first difference  : none -- the two dumps are identical")
    else:
        print("first difference  : ROM 0x%05X (guest 0x%08X)"
              % (first_diff, 0xBFC00000 + first_diff))
    print("seeds kept        : %d of %d" % (len(kept), len(base.get("seeds", []))))
    print("seeds dropped     : %d" % len(dropped))
    if kept:
        print("highest kept      : %s" % kept[-1]["address"])
    if dropped:
        print("lowest dropped    : %s" % dropped[0][0]["address"])

    if not kept:
        sys.exit("error: no seed survived the filter -- this dump shares no code with "
                 "the reference, so the base corpus does not apply to it")

    if args.check:
        expected_path = resolve(args.check)
        with open(expected_path) as handle:
            expected = handle.read()
        if text == expected:
            print("check             : MATCH (byte-identical to %s)" % args.check)
            return 0
        print("check             : MISMATCH against %s" % args.check)
        try:
            other = json.loads(expected)
        except ValueError:
            return 1
        mine = [s["address"] for s in kept]
        theirs = [s["address"] for s in other.get("seeds", [])]
        only_mine = [a for a in mine if a not in set(theirs)]
        only_theirs = [a for a in theirs if a not in set(mine)]
        print("  only here       : %d %s" % (len(only_mine), only_mine[:8]))
        print("  only there      : %d %s" % (len(only_theirs), only_theirs[:8]))
        if not only_mine and not only_theirs:
            print("  same seeds; formatting or metadata differs")
        return 1

    out_path = resolve(args.out) if args.out else os.path.join(
        ROOT, "recompiler", "seeds", "phase2_ghidra_seeds_%s.json" % stem)
    with open(out_path, "w") as handle:
        handle.write(text)
    print("wrote             : %s" % os.path.relpath(out_path, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
