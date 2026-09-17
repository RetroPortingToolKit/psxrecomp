#!/usr/bin/env python3
"""Codegen test: a computed-stride jump into an unrolled run gets a switch.

Duff's-device copy loops enter their unrolled body in the middle through
`jr R` with `R = base + index * stride`. There is no table in memory, so the
bounded-jump-table resolver cannot see it and the emitter used to fall back
to a bare CPS tail-transfer; the trampoline then found no dispatch key at the
interior PC and the dirty-RAM interpreter ran the rest of the copy on every
call (Breath of Fire III's decompressor: the whole non-kernel residual of a
play session). resolve_computed_stride_jump recognises the shape and the
emitter registers the interior targets and switches on them.

Synthesized function (load 0x80010000):

    sll   t7,a0,3
    sll   t5,a0,2
    addu  t7,t7,t5          ; t7 = a0 * 12
    addu  t7,a1,t7          ; t7 = base + a0 * 12
    jr    t7
    nop
    lbu t5,0(a2); nop; sb t5,0(a3)      ; group 0 @ +0x18
    lbu t5,1(a2); nop; sb t5,1(a3)      ; group 1 @ +0x24
    lbu t5,2(a2); nop; sb t5,2(a3)      ; group 2 @ +0x30
    lbu t5,3(a2); nop; sb t5,3(a3)      ; group 3 @ +0x3C
    jr ra ; nop                         ; tail    @ +0x48

Expected: a `computed-stride jump` switch with cases for every group and the
tail, each a `goto block_...`, and the CPS tail-transfer kept as default.

Usage:  python test_computed_stride_jump_codegen.py [--recompiler <psxrecomp-game.exe>]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

LOAD = 0x80010000


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, LOAD)
    struct.pack_into("<I", h, 0x1C, len(data))
    return bytes(h) + data


def build_exe():
    body = [
        0x000478C0,   # sll  t7,a0,3
        0x00046880,   # sll  t5,a0,2
        0x01ED7821,   # addu t7,t7,t5
        0x00AF7821,   # addu t7,a1,t7
        0x01E00008,   # jr   t7
        0x00000000,   # nop
    ]
    for k in range(4):
        body += [0x90CD0000 | k, 0x00000000, 0xA0ED0000 | k]   # lbu t5,k(a2); nop; sb t5,k(a3)
    body += [0x03E00008, 0x00000000]                           # jr ra; nop
    return make_psxexe(LOAD, w(body))


def gen_c(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(build_exe())
    with open(seeds, "w") as f:
        f.write("0x80010000\n")
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"recompiler failed:\n{r.stderr or r.stdout}")
    chunks = []
    for name in sorted(os.listdir(out)):
        if "_full" in name and name.endswith(".c") and "_dispatch" not in name:
            with open(os.path.join(out, name), encoding="utf-8") as f:
                chunks.append(f.read())
    if not chunks:
        raise SystemExit(f"no _full*.c emitted in {out}")
    return "\n".join(chunks)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_recomp = os.path.normpath(os.path.join(here, "..", "build", "psxrecomp-game.exe"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", default=default_recomp)
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler} (build it first)")

    with tempfile.TemporaryDirectory() as tmp:
        c = gen_c(args.recompiler, tmp)

    fails = []
    m = re.search(r"/\* computed-stride jump into unrolled run 0x80010018 \(rom 0x80010018\), "
                  r"stride 12, 5 entries \*/", c)
    if not m:
        fails.append("no computed-stride switch comment for run 0x80010018 / stride 12 / 5 entries")
    for k in range(5):
        t = 0x80010018 + 12 * k
        if not re.search(r"case 0x%08Xu:\s*(?:\n.*?)*?goto block_%08X;" % (t, t), c):
            fails.append("missing case 0x%08X -> goto block_%08X" % (t, t))
    if not re.search(r"default:\s*\n(?:.*\n)*?\s*cpu->pc = .*; return;  /\* CPS: jr table miss", c):
        fails.append("the CPS tail-transfer default is missing from the switch")
    # Interior labels must exist mid-block for the gotos to land on.
    for k in range(1, 4):
        t = 0x80010018 + 12 * k
        if ("block_%08X:" % t) not in c:
            fails.append("no interior label block_%08X" % t)

    for f in fails:
        print("FAIL:", f)
    if fails:
        return 1
    print("PASS: computed-stride jump emits a switch over every unrolled-run entry.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
