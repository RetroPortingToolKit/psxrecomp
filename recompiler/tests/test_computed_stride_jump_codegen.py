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

A second function (see build_exe) covers the sibling shape from the same
decoder: an in-function pointer table indexed by a stored, unchecked byte
offset (`lui/ori base; addu; lw; jr`, no sltiu/beq guard, no sll), whose
extent resolve_self_limited_jump_table takes from the table's own layout.

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
    body += [0x00000000] * 4                                   # pad to 0x80010060

    # Second function @ 0x80010060: a self-limited pointer table indexed by a
    # stored, unchecked byte offset (no sltiu/beq guard, no sll):
    #   lui t5,0x8001 ; ori t5,t5,0x007C ; addu t5,a0,t5 ; lw t4,0(t5) ; nop ; jr t4 ; nop   (0x60..0x78)
    #   table @ 0x8001007C: 0x80010084, 0x80010090  (ends where its lowest target begins)
    #   0x80010084: addiu v0,zero,1 ; j exit ; nop
    #   0x80010090: addiu v0,zero,2 ; j exit ; nop
    #   0x8001009C exit: jr ra ; nop
    exit_pc = 0x8001009C
    j_exit = 0x08000000 | ((exit_pc >> 2) & 0x03FFFFFF)
    body += [0x3C0D8001, 0x35AD007C, 0x008D6821, 0x8DAC0000, 0x00000000, 0x01800008, 0x00000000]
    body += [0x80010084, 0x80010090]
    body += [0x24020001, j_exit, 0x00000000]
    body += [0x24020002, j_exit, 0x00000000]
    body += [0x03E00008, 0x00000000]
    return make_psxexe(LOAD, w(body))


def gen_c(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(build_exe())
    with open(seeds, "w") as f:
        f.write("0x80010000\n0x80010060\n")
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

    # Second function: the self-limited table.
    if not re.search(r"/\* self-limited jump table 0x8001007C \(rom 0x8001007C\), 2 entries, unchecked index \*/", c):
        fails.append("no self-limited table switch comment for table 0x8001007C / 2 entries")
    for t in (0x80010084, 0x80010090):
        if not re.search(r"case 0x%08Xu:\s*(?:\n.*?)*?goto block_%08X;" % (t, t), c):
            fails.append("missing self-limited case 0x%08X -> goto block_%08X" % (t, t))

    for f in fails:
        print("FAIL:", f)
    if fails:
        return 1
    print("PASS: computed-stride jump and self-limited table each emit a switch over every entry.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
