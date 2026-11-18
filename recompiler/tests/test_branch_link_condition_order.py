#!/usr/bin/env python3
"""Recompiler codegen regression test: BLTZAL/BGEZAL on $ra test the old $ra.

BLTZAL and BGEZAL compare rs with zero and write the return address to $ra.
The comparison uses rs as it was before the link write, so

    bltzal $ra, target

branches on the caller's $ra, not on the new link value (a KSEG0 address,
which is always negative as a signed word).

The CFG emitter wrote cpu->gpr[31] first and only then latched the branch
condition before the delay slot, so a branch on $ra always saw the link value:
BLTZAL $ra was always taken and BGEZAL $ra never was.

Asserts that, for both instructions, the condition latch is emitted before the
link write in the same block.

Usage:  python test_branch_link_condition_order.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, glob, os, re, struct, subprocess, sys, tempfile

LOAD = 0x80010000
BLTZAL_AT = LOAD          # bltzal $ra, +2
BGEZAL_AT = LOAD + 0x20   # bgezal $ra, +2

BLTZAL_RA_2 = 0x07F00002  # regimm rs=31 rt=0x10 imm=2
BGEZAL_RA_2 = 0x07F10002  # regimm rs=31 rt=0x11 imm=2
JR_RA = 0x03E00008
NOP = 0x00000000


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)
    return bytes(h) + data


def build_exe():
    # Each function: the link branch, its slot, the fall-through return and
    # the taken-path return. Padded so the second function starts at +0x20.
    one = [None, NOP, JR_RA, NOP, JR_RA, NOP, NOP, NOP]
    a = list(one); a[0] = BLTZAL_RA_2
    b = list(one); b[0] = BGEZAL_RA_2
    return make_psxexe(LOAD, LOAD, w(a + b))


def generate(recompiler, tmp, root):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(build_exe())
    with open(seeds, "w") as f:
        f.write("0x%08X\n0x%08X\n" % (BLTZAL_AT, BGEZAL_AT))
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=root)
    if r.returncode != 0:
        raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
    srcs = glob.glob(os.path.join(out, "*_full*.c"))
    if not srcs:
        raise SystemExit("no *_full*.c emitted in " + out)
    return "".join(open(p, encoding="utf-8", errors="replace").read() for p in srcs)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build",
                                                          "psxrecomp-game")))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        src = generate(args.recompiler, tmp, root)

    failures = []
    for name, at in (("bltzal", BLTZAL_AT), ("bgezal", BGEZAL_AT)):
        latch = re.search(r"int\s+_bc_%08X\s*=\s*\(([^;]*)\);" % at, src)
        link = re.search(r"cpu->gpr\[31\]\s*=\s*0x%08Xu;" % (at + 8), src)
        if not latch:
            failures.append("%s: no _bc_%08X condition latch emitted" % (name, at))
            continue
        if not link:
            failures.append("%s: no $ra link write of 0x%08X emitted" % (name, at + 8))
            continue
        if "gpr[31]" not in latch.group(1):
            failures.append("%s: latch does not test $ra: %r" % (name, latch.group(1)))
        if latch.start() > link.start():
            failures.append("%s: condition latched after the $ra link write, so "
                            "it tests the link value, not the old $ra" % name)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        raise SystemExit(1)

    print("branch-and-link condition order test passed (bltzal, bgezal)")


if __name__ == "__main__":
    main()
