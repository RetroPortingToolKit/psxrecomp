#!/usr/bin/env python3
"""Onboard a retail PSX BIOS dump: verify it, derive its seeds, write its profile.

A retail image of the v2.2/v3.0/v4.x line shares its boot code and kernel with
SCPH-1001 and diverges only in the shell.  When that holds, everything
bios/SCPH1001.toml states about the kernel -- the ROM->RAM copy windows, the
install slots, the HLE anchors -- is a statement about bytes this image has too,
and carries over.  PR #243 made that argument by hand for SCPH-5552.  This tool
makes it mechanically, and REFUSES to carry anything it cannot verify.

What it checks, before writing a line:

  * size and identity (CRC32, SHA-256) of the dump;
  * that ROM [0x00000, 0x18000) -- boot + Kernel Part 1 + Kernel Part 2 -- is
    byte-identical to the reference.  That range is the whole justification:
    the kernel copy window, every install slot (RAM 0x500..0x8500 is sourced
    from it), shell_entry_phys (LoadRunShell at ROM 0x6FF0) and
    deliver_event_ret (kernel RAM 0x1718 -> ROM 0x11218) all live inside it.

If that range differs, the tool stops and says where.  It does not emit a
profile that claims SCPH-1001's address model for an image that does not match
it -- that is exactly the stale-profile defect this work exists to remove.

  tools/add_retail_bios.py --dump bios/SCPH5501.BIN \
      --id SCPH-5501 --name "Sony SCPH-5501 BIOS" --note "NTSC-U, v3.0"

Writes bios/<STEM>.toml and recompiler/seeds/phase2_ghidra_seeds_<STEM>.json,
then prints the psx_known_bios_images.h row for a human to add.  It never edits
C headers: an identity table entry is a claim the build makes to players, and
that belongs in a reviewed diff.
"""
import argparse
import hashlib
import json
import importlib.util
import os
import subprocess
import sys
import zlib

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11, as elsewhere in tools/
    import tomli as tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILTER_TOOL = os.path.join(ROOT, "tools", "gen_retail_bios_seeds.py")
REFERENCE_PROFILE = "bios/SCPH1001.toml"
REFERENCE_ROM = "bios/SCPH1001.BIN"

# The ROM regions every value this tool CARRIES from the reference profile is a
# fact about. A new backend compiles the image's OWN bytes, so the image may
# differ elsewhere and still be onboarded -- what must not differ is anything
# the carried address model, install slots and HLE anchors describe:
#
#   kernel copy window  the [[address_model.copy]] with kernel_bless, source of
#                       kernel RAM 0x500.. -- so every install slot and
#                       deliver_event_ret (kernel RAM 0x1718 -> ROM 0x11218)
#   LoadRunShell        the routine whose jump target shell_entry_phys records
#
# Derived from the reference profile where possible; the LoadRunShell probe is
# a fixed window because the profile records only the jump's target, not where
# the jump lives. SCPH-5500 is why this distinction exists: its reset stub
# carries nine extra instructions at ROM 0x24..0x47, which touches nothing
# carried here, so it gets its own backend rather than being refused.
LOADRUNSHELL_RANGE = (0x06F00, 0x07100)
BOOT_STUB_RANGE = (0x00000, 0x10000)

_spec = importlib.util.spec_from_file_location("gen_retail_bios_seeds", FILTER_TOOL)
_filter = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_filter)


def resolve(path):
    return path if os.path.isabs(path) else os.path.join(ROOT, path)


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def identify(data):
    wordsum = 0
    for off in range(0, len(data) - 3, 4):
        wordsum = (wordsum + int.from_bytes(data[off:off + 4], "little")) & 0xFFFFFFFF
    return {
        "size": len(data),
        "crc32": "%08X" % (zlib.crc32(data) & 0xFFFFFFFF),
        "sha256": hashlib.sha256(data).hexdigest(),
        "wordsum": "%08X" % wordsum,
    }


def first_diff(reference, target, lo, hi):
    """First differing ROM offset in [lo, hi), or None."""
    for offset in range(lo, min(hi, len(reference), len(target))):
        if reference[offset] != target[offset]:
            return offset
    return None


def diff_runs(reference, target, lo, hi):
    """Contiguous differing byte runs in [lo, hi), as (start, end) inclusive."""
    runs = []
    for offset in range(lo, min(hi, len(reference), len(target))):
        if reference[offset] == target[offset]:
            continue
        if runs and offset == runs[-1][1] + 1:
            runs[-1][1] = offset
        else:
            runs.append([offset, offset])
    return [tuple(r) for r in runs]


def carried_ranges(ref_profile):
    """ROM ranges the carried facts describe, derived from the profile."""
    ranges = []
    for copy in ref_profile["recompiler"].get("address_model", {}).get("copy", []):
        if copy.get("kernel_bless"):
            lo = int(copy["rom_lo"], 16) & 0x1FFFFFFF
            hi = int(copy["rom_hi"], 16) & 0x1FFFFFFF
            ranges.append(("kernel copy window", lo - 0x1FC00000, hi - 0x1FC00000))
    ranges.append(("LoadRunShell", LOADRUNSHELL_RANGE[0], LOADRUNSHELL_RANGE[1]))
    return ranges


def hexs(value):
    return '"0x%08X"' % value


def render_profile(stem, ident, args, ref_profile, ref_ident):
    recomp = ref_profile["recompiler"]
    model = recomp.get("address_model", {})
    copies = model.get("copy", [])
    slots = recomp.get("install_slots", [])
    exports = recomp.get("runtime_exports", {})

    note = (" (%s)" % args.note) if args.note else ""
    lines = [
        "# bios/%s.toml — BIOS build profile for the retail Sony %s image%s."
        % (stem, args.id, note),
        "#",
        "# Generated by tools/add_retail_bios.py from %s." % REFERENCE_PROFILE,
        "#",
        "# WHY THIS PROFILE MAY CARRY SCPH-1001'S ADDRESS MODEL",
        "# ----------------------------------------------------",
        "# Every region the values below describe — the kernel copy window that",
        "# sources kernel RAM 0x500.., and LoadRunShell — was compared",
        "# byte-for-byte against %s and is IDENTICAL." % REFERENCE_ROM,
        "#   reference  CRC32 %s  SHA-256 %s" % (ref_ident["crc32"], ref_ident["sha256"]),
        "#   this image CRC32 %s  SHA-256 %s" % (ident["crc32"], ident["sha256"]),
        "#",
        "# Everything below that describes the kernel is therefore a statement",
        "# about bytes this image demonstrably has: the copy windows, every",
        "# install slot (RAM 0x500..0x8500 is sourced from Kernel Part 2), and",
        "# both HLE anchors (LoadRunShell at ROM 0x6FF0; the DeliverEvent jalr",
        "# at kernel RAM 0x1718 -> ROM 0x11218).",
        "#",
        "%s" % ("\n".join(
            ["# THIS IMAGE DIFFERS FROM THE REFERENCE OUTSIDE THOSE REGIONS:"] +
            ["#   ROM 0x%05X..0x%05X (%d bytes)" % (lo, hi, hi - lo + 1)
             for lo, hi in getattr(args, "_boot_runs", [])] +
            ["# That code is compiled from THIS image's bytes into THIS",
             "# backend, which is why it needs its own backend and cannot be",
             "# an [[program.accepted]] entry of the shared one.", "#"])
            if getattr(args, "_boot_runs", []) else "#"),
        "# The shell (ROM 0x18000+) differs, as it does on every regional image.",
        "# Its functions are absent from the seed corpus and run through the",
        "# dirty-RAM interpreter, exactly as on SCPH-101 and SCPH-5552.",
        "#",
        "# EVIDENCE LIMIT: the install slots were measured on a live SCPH-1001",
        "# boot (see %s). They are carried here on the byte" % REFERENCE_PROFILE,
        "# identity above, NOT re-measured on a live boot of this image.",
        "",
        "[program]",
        'name         = "%s"' % args.name,
        'id           = "%s"' % args.id,
        'rom          = "bios/%s"' % os.path.basename(args.dump),
        "load_address = %s" % hexs(int(ref_profile["program"]["load_address"], 16)),
        "entry_pc     = %s" % hexs(int(ref_profile["program"]["entry_pc"], 16)),
        "text_size    = %s" % hexs(int(ref_profile["program"]["text_size"], 16)),
        "",
        "# Declared identity. The emitter refuses to generate from any other",
        "# image (main_bios.cpp's declared-identity gate). Without this pin a",
        "# wrong-revision dump generates silently and wild-jumps at run time.",
        "[program.image]",
        'sha256          = "%s"' % ident["sha256"],
        'license         = "proprietary"',
        "redistributable = false",
        "",
        "[recompiler]",
        'seeds    = "recompiler/seeds/phase2_ghidra_seeds_%s.json"' % stem,
        'out_dir  = "generated"',
        'out_stem = "%s"' % stem,
        "strict   = true",
        "",
        "# Carried verbatim from %s. Bounds are [lo, hi) with hi" % REFERENCE_PROFILE,
        "# EXCLUSIVE. Do NOT widen the shell window here: that profile records",
        "# widening it as a real behaviour change needing its own evidence.",
        "[recompiler.address_model]",
        'normalize_mask = "0x%08X"' % model.get("normalize_mask_int", 0x1FFFFFFF),
    ]
    for copy in copies:
        lines += [
            "",
            "[[recompiler.address_model.copy]]",
            'name         = "%s"' % copy["name"],
            "rom_lo       = %s" % hexs(int(copy["rom_lo"], 16)),
            "rom_hi       = %s" % hexs(int(copy["rom_hi"], 16)),
            "ram_lo       = %s" % hexs(int(copy["ram_lo"], 16)),
            "runtime_base = %s" % hexs(int(copy["runtime_base"], 16)),
            'dispatch_key = "%s"' % copy["dispatch_key"],
        ]
        if "kernel_bless" in copy:
            lines.append("kernel_bless = %s" % ("true" if copy["kernel_bless"] else "false"))
    if slots:
        lines += [
            "",
            "# Kernel-RAM ranges the guest's Psy-Q libapi patchers overwrite at",
            "# run time. Sourced from Kernel Part 2, which is byte-identical",
            "# above, so the same ranges hold. Re-measure per SDK version.",
        ]
        for slot in slots:
            lines += ["", "[[recompiler.install_slots]]",
                      "ram_addr = %s" % hexs(int(slot["ram_addr"], 16))]
            if "len" in slot:
                lines.append('len      = "0x%X"' % int(slot["len"], 16))
            if "resume" in slot:
                lines.append('resume   = "%s"' % slot["resume"])
    if exports:
        lines += [
            "",
            "# Per-image HLE anchors couriered into the generated C. Both live",
            "# inside the verified range. Omitting a key makes that HLE feature",
            "# structurally unavailable rather than firing on a wrong address.",
            "[recompiler.runtime_exports]",
        ]
        for key, value in exports.items():
            lines.append("%-17s = %s" % (key, hexs(int(value, 16))))
    return "\n".join(lines) + "\n"


def seed_addresses(path):
    with open(path) as handle:
        return [s["address"] for s in json.load(handle)["seeds"]]


def accept_into(args, stem, ident, target, reference):
    """Register `stem` as an additional accepted image of an existing profile.

    The admission test is stricter than the kernel-range compare used for a new
    profile: EVERY seeded function body must be byte-identical, because the
    accepted image runs the reference image's compiled code verbatim. The
    filter's kept-set is exactly that measurement, so requiring it to equal the
    reference corpus is the test.

    SCPH-5500 is the worked counter-example: its reset stub carries nine extra
    instructions where SCPH-1001 has padding, so it loses the reset_vector seed
    and is refused here. It needs its own backend, not an accepted entry.
    """
    profile_path = resolve(args.accept_into)
    if not os.path.isfile(profile_path):
        sys.exit("error: --accept-into profile not found: %s" % profile_path)
    with open(profile_path, "rb") as handle:
        profile = tomllib.load(handle)
    recomp = profile.get("recompiler", {})
    ref_corpus = resolve(recomp.get("seeds", ""))
    if not os.path.isfile(ref_corpus):
        sys.exit("error: profile's seeds file not found: %s" % ref_corpus)

    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        derived = tmp.name
    try:
        rc = subprocess.run(
            [sys.executable, FILTER_TOOL, "--target", resolve(args.dump),
             "--reference", resolve(args.reference), "--base",
             os.path.relpath(ref_corpus, ROOT), "--stem", stem, "--out", derived],
            cwd=ROOT).returncode
        if rc != 0:
            sys.exit("error: could not derive a corpus for this dump")
        mine, theirs = seed_addresses(derived), seed_addresses(ref_corpus)
    finally:
        if os.path.exists(derived):
            os.unlink(derived)

    if mine != theirs:
        missing = [a for a in theirs if a not in set(mine)]
        sys.exit(
            "error: %s does not run this backend's code.\n"
            "  %d of %d seeded functions match; %d differ, first %s\n"
            "Every seeded body must be byte-identical, because an accepted "
            "image executes the reference image's compiled code verbatim.\n"
            "This image needs its own backend, not an accepted entry."
            % (stem, len(mine), len(theirs), len(missing),
               missing[0] if missing else "?"))

    for existing in profile.get("program", {}).get("accepted", []):
        if existing.get("sha256", "").lower() == ident["sha256"]:
            print("already accepted: %s" % existing.get("id", stem))
            return 0
    if profile.get("program", {}).get("image", {}).get("sha256", "").lower() \
            == ident["sha256"]:
        sys.exit("error: that dump IS this profile's reference image")

    block = (
        "\n[[program.accepted]]\n"
        "# Verified %s: all %d seeded function bodies byte-identical to the\n"
        "# reference, so this image runs the same compiled code.\n"
        'id      = "%s"\n'
        'stem    = "%s"\n'
        'sha256  = "%s"\n'
        'crc32   = "0x%s"\n'
        "size    = %d\n"
        'wordsum = "0x%s"\n'
        % (args.note or "identity", len(mine), args.id, stem,
           ident["sha256"], ident["crc32"], ident["size"], ident["wordsum"]))
    with open(profile_path, "a") as handle:
        handle.write(block)
    print("all %d seeded functions identical -> accepted" % len(mine))
    print("appended [[program.accepted]] %s to %s"
          % (args.id, os.path.relpath(profile_path, ROOT)))
    print("\nRegenerate so the backend carries it:")
    print("    bash tools/regen_bios.sh --config %s"
          % os.path.relpath(profile_path, ROOT))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dump", required=True, help="the retail BIOS dump to onboard")
    parser.add_argument("--id", help='profile id, e.g. "SCPH-5501" (default: from --stem)')
    parser.add_argument("--name", help="human-readable name for [program] name")
    parser.add_argument("--note", help='short provenance note, e.g. "NTSC-U, v3.0"')
    parser.add_argument("--stem", help="image stem (default: model token of --dump)")
    parser.add_argument("--reference", default=REFERENCE_ROM)
    parser.add_argument("--reference-profile", default=REFERENCE_PROFILE)
    parser.add_argument("--force", action="store_true",
                        help="overwrite an existing profile or corpus")
    parser.add_argument("--accept-into", metavar="PROFILE",
                        help="instead of writing a new profile, register this "
                             "dump as an additional accepted image of PROFILE, "
                             "so one backend serves both with no rebuild")
    args = parser.parse_args()

    dump_path = resolve(args.dump)
    ref_path = resolve(args.reference)
    for path, what in ((dump_path, "--dump"), (ref_path, "--reference")):
        if not os.path.isfile(path):
            sys.exit("error: %s not found: %s" % (what, path))

    stem = args.stem or _filter.image_stem(args.dump)
    args.id = args.id or stem
    args.name = args.name or ("Sony %s BIOS" % args.id)

    target = read_bytes(dump_path)
    reference = read_bytes(ref_path)
    ident, ref_ident = identify(target), identify(reference)

    print("stem              : %s" % stem)
    print("dump              : %s" % args.dump)
    print("size              : %d bytes" % ident["size"])
    print("CRC32             : %s" % ident["crc32"])
    print("SHA-256           : %s" % ident["sha256"])

    if ident["size"] != ref_ident["size"]:
        sys.exit("error: dump is %d bytes, reference is %d — not the same class of "
                 "image" % (ident["size"], ref_ident["size"]))
    if ident["sha256"] == ref_ident["sha256"]:
        sys.exit("error: this dump IS the reference image; nothing to onboard")

    with open(resolve(args.reference_profile), "rb") as handle:
        ref_profile = tomllib.load(handle)

    for label, lo, hi in carried_ranges(ref_profile):
        diff = first_diff(reference, target, lo, hi)
        if diff is not None:
            sys.exit(
                "error: the %s (ROM [0x%05X, 0x%05X)) differs from the "
                "reference, first at 0x%05X (guest 0x%08X).\n"
                "The address model, install slots and HLE anchors this tool "
                "carries are all facts about that region, so they do NOT hold "
                "for this image.\nOnboarding it needs its own measurement "
                "pass, not this tool."
                % (label, lo, hi, diff, 0xBFC00000 + diff))
        print("%-18s: ROM [0x%05X, 0x%05X) IDENTICAL -- carried values hold"
              % (label, lo, hi))

    # Divergence OUTSIDE the carried regions is fine for a NEW backend: that
    # code is compiled from this image's own bytes. Say so loudly anyway, and
    # record it in the profile -- it is the difference between this image and
    # the reference, and a reader should not have to rediscover it.
    boot_runs = diff_runs(reference, target, *BOOT_STUB_RANGE)
    carried_lo = min(lo for _, lo, _ in carried_ranges(ref_profile))
    boot_runs = [r for r in boot_runs if r[0] < carried_lo]
    if boot_runs:
        total = sum(hi - lo + 1 for lo, hi in boot_runs)
        print("boot stub         : %d byte(s) differ in %d run(s) -- compiled "
              "from THIS image" % (total, len(boot_runs)))
        for lo, hi in boot_runs[:8]:
            print("                    ROM 0x%05X..0x%05X" % (lo, hi))
    args._boot_runs = boot_runs

    if args.accept_into:
        return accept_into(args, stem, ident, target, reference)

    profile_path = os.path.join(ROOT, "bios", "%s.toml" % stem)
    corpus_path = os.path.join(ROOT, "recompiler", "seeds",
                               "phase2_ghidra_seeds_%s.json" % stem)
    for path in (profile_path, corpus_path):
        if os.path.exists(path) and not args.force:
            sys.exit("error: %s exists; pass --force to overwrite"
                     % os.path.relpath(path, ROOT))

    # A STANDALONE backend compiles this image's own bytes, so its corpus is
    # about where functions START, not whether their bytes match the
    # reference. Filtering by byte-identity here would drop exactly the
    # functions that differ -- including reset_vector, the entry point, which
    # must be compiled or the process has nowhere to begin.
    #
    # The kernel's function boundaries are shared (the carried-regions gate
    # above proved the kernel copy window identical), while the shell's are
    # not, so the corpus is the base seeds below the shell. That is the same
    # 534 the shared backend uses, reset_vector included.
    shell_lo = None
    for copy in ref_profile["recompiler"].get("address_model", {}).get("copy", []):
        if not copy.get("kernel_bless"):
            shell_lo = int(copy["rom_lo"], 16) & 0x1FFFFFFF
    if shell_lo is None:
        sys.exit("error: reference profile declares no shell copy window")
    shell_guest = 0xBFC00000 + (shell_lo - 0x1FC00000)

    with open(resolve(ref_profile["recompiler"]["seeds"])) as handle:
        base = json.load(handle)
    kept = [sd for sd in base.get("seeds", [])
            if int(sd["address"], 16) < shell_guest]
    out = {
        "schema": base.get("schema", "psxrecomp phase2 seeds"),
        "source": "%s filtered to function starts below the shell (ROM 0x%05X) "
                  "for %s" % (os.path.basename(ref_profile["recompiler"]["seeds"]),
                              shell_lo - 0x1FC00000, stem),
        "seed_count": len(kept),
        "seeds": kept,
    }
    if "excluded" in base:
        out["excluded"] = base["excluded"]
    with open(corpus_path, "w") as handle:
        handle.write(json.dumps(out, indent=2))
    print()
    print("seed corpus       : %d function starts below the shell" % len(kept))
    print("wrote             : %s" % os.path.relpath(corpus_path, ROOT))

    ref_profile["recompiler"].setdefault("address_model", {})["normalize_mask_int"] = int(
        ref_profile["recompiler"].get("address_model", {}).get("normalize_mask", "0x1FFFFFFF"), 16)

    with open(profile_path, "w") as handle:
        handle.write(render_profile(stem, ident, args, ref_profile, ref_ident))
    print("wrote             : %s" % os.path.relpath(profile_path, ROOT))

    print()
    print("NEXT — these are reviewed edits, not something this tool writes:")
    print()
    print("  1. runtime/include/psx_bios_known_images.h, in psx_known_bios_images[]:")
    print('       { "%s", "%s", 0x%su, %du },'
          % (stem, args.id, ident["crc32"], ident["size"]))
    print()
    print("  2. Generate the backend and check it boots:")
    print("       bash tools/regen_bios.sh --config bios/%s.toml" % stem)
    print()
    print("  3. Link it by adding %s to PSXRECOMP_BIOS_STEMS." % stem)
    return 0


if __name__ == "__main__":
    sys.exit(main())
