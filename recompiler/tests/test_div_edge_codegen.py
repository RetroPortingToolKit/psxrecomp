#!/usr/bin/env python3
"""Recompiler codegen test: emitted DIV gives the R3000A results for every
divisor, including the two edge cases that do not trap on hardware.

PSX-SPX ("CPU Specifications", Multiply/divide) lists the results:

    div   0..+7FFFFFFFh    0   ->  HI = Rs   LO = -1
    div   -80000000h..-1   0   ->  HI = Rs   LO = +1
    div   -80000000h      -1   ->  HI = 0    LO = -80000000h

`INT32_MIN / -1` is undefined in C and raises a divide error on x86 hosts,
so the emitted statement must not reach the C division for that pair.

Generates a one-function PS-X EXE (div a0, a1; jr ra; nop), takes the
emitted DIV statement, compiles it into a small harness at -O0 and -O2 and
checks HI/LO for a table of operands.

Usage:  python test_div_edge_codegen.py --recompiler <psxrecomp-game> [--compiler <cc>]
Exit 0 = PASS.
"""
import argparse, glob, os, re, shutil, struct, subprocess, sys, tempfile

LOAD = 0x80010000
DIV_A0_A1 = 0x0085001A   # div a0, a1  (rs = 4, rt = 5)
JR_RA = 0x03E00008
NOP = 0x00000000

# (rs, rt, expected LO, expected HI)
CASES = [
    (0x00000007, 0x00000002, 0x00000003, 0x00000001),
    (0x00000007, 0xFFFFFFFE, 0xFFFFFFFD, 0x00000001),   #  7 / -2
    (0xFFFFFFF9, 0x00000002, 0xFFFFFFFD, 0xFFFFFFFF),   # -7 /  2
    (0x80000000, 0x00000001, 0x80000000, 0x00000000),
    (0x80000000, 0xFFFFFFFF, 0x80000000, 0x00000000),   # overflow
    (0x7FFFFFFF, 0xFFFFFFFF, 0x80000001, 0x00000000),
    (0x00000005, 0x00000000, 0xFFFFFFFF, 0x00000005),   # x >= 0 by zero
    (0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000),
    (0xFFFFFFFB, 0x00000000, 0x00000001, 0xFFFFFFFB),   # x < 0 by zero
    (0x80000000, 0x00000000, 0x00000001, 0x80000000),
]

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>

typedef struct { uint32_t gpr[32]; uint32_t hi, lo; } CPUState;

static void emitted_div(CPUState* cpu)
{
%(stmt)s
}

static volatile uint32_t cases[][4] = {
%(cases)s
};

int main(void)
{
    int failures = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CPUState s = {{0}};
        s.gpr[4] = cases[i][0];
        s.gpr[5] = cases[i][1];
        emitted_div(&s);
        if (s.lo != cases[i][2] || s.hi != cases[i][3]) {
            printf("FAIL: div %%08X / %%08X gave LO=%%08X HI=%%08X, expected LO=%%08X HI=%%08X\n",
                   (unsigned)cases[i][0], (unsigned)cases[i][1], (unsigned)s.lo,
                   (unsigned)s.hi, (unsigned)cases[i][2], (unsigned)cases[i][3]);
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
"""


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


def generate(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(make_psxexe(LOAD, LOAD, w([DIV_A0_A1, JR_RA, NOP])))
    with open(seeds, "w") as f:
        f.write("0x%08X\n" % LOAD)
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
    srcs = glob.glob(os.path.join(out, "*_full*.c"))
    if not srcs:
        raise SystemExit("no *_full*.c emitted in " + out)
    return "".join(open(p, encoding="utf-8", errors="replace").read() for p in srcs)


def div_statement(src):
    # The DIV translation is the statement between the instruction's cycle
    # step and its own mult/div completion deadline.
    m = re.search(r"psx_cyc_step\(cpu, 0x[0-9A-Fa-f]+u\);\n#endif\n(.*?)\n"
                  r"#ifdef PSX_ENABLE_BLOCK_CYCLES\n\s*psx_muldiv_set\(cpu, 37u\);",
                  src, re.S)
    if not m:
        raise SystemExit("FAIL: no DIV statement found in the emitted function")
    return m.group(1)


def build_and_run(compiler, tmp, stmt, opt):
    source = os.path.join(tmp, "div_harness.c")
    exe = os.path.join(tmp, "div_harness" + opt.replace("-", "").replace("/", "") +
                       (".exe" if os.name == "nt" else ""))
    rows = ",\n".join("    {0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu}" % c for c in CASES)
    with open(source, "w") as f:
        f.write(HARNESS % {"stmt": stmt, "cases": rows})
    name = os.path.basename(compiler).lower()
    if name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        cmd = [compiler, "/nologo", "/O2" if opt == "-O2" else "/Od", source, "/Fe" + exe]
    else:
        cmd = [compiler, "-std=c11", opt, source, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=tmp)
    if r.returncode != 0:
        raise SystemExit("harness compile failed (%s):\n%s%s" % (opt, r.stdout, r.stderr))
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build",
                                                          "psxrecomp-game")))
    ap.add_argument("--compiler",
                    default=os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc"))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)
    if not args.compiler:
        raise SystemExit("no C compiler found; pass --compiler")

    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        stmt = div_statement(generate(args.recompiler, tmp))
        for opt in ("-O0", "-O2"):
            rc, out = build_and_run(args.compiler, tmp, stmt, opt)
            if rc != 0:
                failed = True
                print("FAIL at %s (exit %d):" % (opt, rc))
                print(out.rstrip() or "  (no output: the harness crashed)")
    if failed:
        print("emitted DIV statement:\n  " + stmt.strip())
        raise SystemExit(1)
    print("DIV edge-case codegen test passed (%d cases, -O0/-O2)" % len(CASES))


if __name__ == "__main__":
    main()
