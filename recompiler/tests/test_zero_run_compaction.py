#!/usr/bin/env python3
"""Recompiler codegen regression test: zero-word runs are compacted faithfully.

Discovery reaches zero-filled BSS / overlay load areas through JAL targets, and
every zero word (sll $0,$0,0) used to expand to about ten lines of C; one large
title had a single function of 110,381 zero words. The emitter now writes a
run leader through the normal path and its followers as one loop that makes
the same per-word calls (the I-cache fetch at a line start, psx_cyc_step,
cosim_instr).

The image holds, in one function:
  run A  1 leader + 64 followers   -> compacted (threshold is 64 followers)
  run B  1 leader + 63 followers   -> stays expanded
  run C  200 zero words split by a backward-branch target in the middle
         -> two blocks, each compacted; the label stays outside both loops

Asserts the loop bounds, that short runs stay expanded, and that every loop
re-expands to exactly the per-word call text the emitter writes for an
expanded zero word of the same address class (taken from run B), so the loop
cannot drift from the expanded form. The only line the loop drops per word is
the "/* nop */" translation comment.

Usage:  python test_zero_run_compaction.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, glob, os, re, struct, subprocess, tempfile

LOAD = 0x80010000
T0 = 8
ADDIU_T0_ZERO_1 = 0x24080001
ADDIU_T0_T0_1 = 0x25080001
JR_RA = 0x03E00008


def at(i):
    return LOAD + 4 * i


def bne_t0_zero(pc_index, target_index):
    off = (target_index - (pc_index + 1)) & 0xFFFF
    return (0x05 << 26) | (T0 << 21) | off


RUN_A = (1, 66)      # [first, end) word indices
RUN_B = (67, 131)
RUN_C = (132, 332)
C_TARGET = 232
BRANCH = 332


def build_words():
    words = [ADDIU_T0_ZERO_1]
    words += [0] * (RUN_A[1] - RUN_A[0])
    words += [ADDIU_T0_T0_1]
    words += [0] * (RUN_B[1] - RUN_B[0])
    words += [ADDIU_T0_T0_1]
    words += [0] * (RUN_C[1] - RUN_C[0])
    assert len(words) == BRANCH
    words += [bne_t0_zero(BRANCH, C_TARGET), 0, JR_RA, 0]
    return words


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)
    return bytes(h) + data


def canonical_source(src):
    """Select the normal OPS0/DEMAND0 alternatives, leaving timing directives.

    Raw shards also contain mutually exclusive copies of each block for the
    optional precision paths. Counting those together does not describe any
    compiled function. Only these two guards are selected here; the per-word
    cycle and COSIM text below remains subject to every existing assertion.
    """
    stack, kept = [], []
    for line in src.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b(.*)", line)
        if directive:
            kind, condition = directive.groups()
            if kind in ("if", "ifdef", "ifndef"):
                precision = kind == "if" and any(flag in condition for flag in (
                    "PSX_PGXP_OPERATIONS", "PSX_PGXP_DEMAND_REGIONS"))
                stack.append([precision, precision])  # owned guard, skipped arm
                if precision:
                    continue
            elif kind == "endif":
                owned, _ = stack.pop()
                if owned:
                    continue
            elif stack[-1][0]:
                assert kind == "else", "unexpected precision guard elif"
                stack[-1][1] = not stack[-1][1]
                continue
        if not any(skipped for _, skipped in stack):
            kept.append(line)
    assert not stack, "unclosed emitted preprocessor guard"
    return "".join(kept)


def generate(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(make_psxexe(LOAD, LOAD, b"".join(struct.pack("<I", x) for x in build_words())))
    with open(seeds, "w") as f:
        f.write("0x%08X\n" % LOAD)
    env = dict(os.environ)
    env.pop("PSX_CODEGEN_CYCLE_PER_INSN", None)   # default: per-instruction timing
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True, encoding="utf-8", errors="replace",
                       env=env)
    if r.returncode != 0:
        raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
    srcs = sorted(glob.glob(os.path.join(out, "*_full*.c")))
    if not srcs:
        raise SystemExit("no *_full*.c emitted in " + out)
    text = "".join(open(p, encoding="utf-8", newline="").read() for p in srcs)
    return canonical_source(text.replace("\r\n", "\n"))


LOOP = re.compile(
    r"^(?P<in>[ ]*)\{ /\* zero-word run 0x(?P<first>[0-9A-F]{8})\.\.0x(?P<last>[0-9A-F]{8}): "
    r"(?P<n>\d+) words, per-word calls as expanded \*/\n"
    r"(?:.*\n)*?(?P=in)\}\n", re.M)

COSIM_END = "cosim_instr(0x%08Xu);\n#endif\n"


def word_calls(src, addr):
    """Expanded call text of the zero word at addr: everything after the
    previous word's cosim block through this word's, minus the translation
    comment line. Both words must be emitted expanded."""
    prev = src.find(COSIM_END % (addr - 4))
    cur = src.find(COSIM_END % addr)
    if prev < 0 or cur < 0 or cur < prev:
        return None
    chunk = src[prev + len(COSIM_END % (addr - 4)):cur + len(COSIM_END % addr)]
    lines = chunk.splitlines(keepends=True)
    kept = [l for l in lines if "/* nop */" not in l]
    if len(lines) - len(kept) != 1:
        return None
    return "".join(kept)


def loop_rule(body, var):
    """Per-word call text the loop body makes, as a function of the address.
    Recognises only the exact forms the emitter writes; None otherwise."""
    m = re.search(r"\n(?P<in>[ ]*)for \(%s = 0x[0-9A-F]{8}u; %s != 0x[0-9A-F]{8}u; %s \+= 4u\) \{\n"
                  r"(?P<body>(?:.*\n)*?)(?P=in)\}\n" % (var, var, var), body)
    if not m:
        return None
    inner = m.group("body")
    cyc = re.fullmatch(
        r"#ifdef PSX_ENABLE_BLOCK_CYCLES\n"
        r"(?P<fi>[ ]*)(?P<fetch>if \(\(%s & 0xCu\) == 0u\) psx_icache_fetch\(cpu, %s\);|"
        r"psx_icache_fetch\(cpu, %s\);)\n"
        r"(?P<si>[ ]*)psx_cyc_step\(cpu, (?P<mask>0x[0-9A-F]+u)\);\n"
        r"#endif\n"
        r"#ifdef PSX_COSIM\n(?P<ci>[ ]*)cosim_instr\(%s\);\n#endif\n" % (var, var, var, var),
        inner)
    if not cyc:
        return None
    every = not cyc.group("fetch").startswith("if ")

    def expand(addr, indent):
        a = "0x%08Xu" % addr
        out = ""
        if every or (addr & 0xC) == 0:
            out += "#ifdef PSX_ENABLE_BLOCK_CYCLES\n%spsx_icache_fetch(cpu, %s);\n#endif\n" % (indent, a)
        out += "#ifdef PSX_ENABLE_BLOCK_CYCLES\n%spsx_cyc_step(cpu, %s);\n#endif\n" % (
            indent, cyc.group("mask"))
        out += "#ifdef PSX_COSIM\n%scosim_instr(%s);\n#endif\n" % (indent, a)
        return out
    return expand, every


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build", "psxrecomp-game")))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler) and not os.path.isfile(args.recompiler + ".exe"):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        src = generate(args.recompiler, tmp)

    failures = []
    loops = list(LOOP.finditer(src))
    got = [(int(m.group("first"), 16), int(m.group("last"), 16), int(m.group("n"))) for m in loops]
    want = [
        (at(RUN_A[0] + 1), at(RUN_A[1] - 1), RUN_A[1] - RUN_A[0] - 1),
        (at(RUN_C[0] + 1), at(C_TARGET - 1), C_TARGET - RUN_C[0] - 1),
        (at(C_TARGET + 1), at(RUN_C[1] - 1), RUN_C[1] - C_TARGET - 1),
    ]
    if got != want:
        failures.append("zero-run loops %s, expected %s" % (
            [("%08X" % f, "%08X" % l, n) for f, l, n in got],
            [("%08X" % f, "%08X" % l, n) for f, l, n in want]))

    # Run B (63 followers) stays expanded, word by word.
    for i in range(RUN_B[0], RUN_B[1]):
        if "/* nop */  /* 0x%08X: 0x00000000 */" % at(i) not in src:
            failures.append("run B word 0x%08X is not emitted expanded" % at(i))
            break

    # The branch target inside run C is a real label between the two loops.
    label = "block_%08X:" % at(C_TARGET)
    if label not in src:
        failures.append("missing label %s inside run C" % label)
    elif len(loops) == 3 and not (loops[1].end() <= src.index(label) < loops[2].start()):
        failures.append("label %s is not between the two run C loops" % label)

    # Expanded-form templates from run B followers, one per (addr & 0xC) class.
    templates = {}
    for i in range(RUN_B[0] + 1, RUN_B[1]):
        cls = at(i) & 0xC
        if cls not in templates:
            calls = word_calls(src, at(i))
            if calls is None:
                failures.append("cannot find expanded text for 0x%08X" % at(i))
                break
            templates[cls] = (at(i), calls)
    if len(templates) == 4:
        def template_for(addr):
            base, calls = templates[addr & 0xC]
            return calls.replace("0x%08X" % base, "0x%08X" % addr)
        # Cached code: the expanded form fetches at a line start only.
        for cls, (base, calls) in templates.items():
            fetch = "psx_icache_fetch(cpu, 0x%08Xu);" % base in calls
            if fetch != (cls == 0):
                failures.append("expanded emitter fetch rule disagrees at 0x%08X" % base)
        indent = re.search(r"^([ ]*)cosim_instr", templates[0][1], re.M).group(1)
        for m in loops:
            first = int(m.group("first"), 16)
            n = int(m.group("n"))
            rule = loop_rule(m.group(0), "_zr_%s" % m.group("first"))
            if rule is None:
                failures.append("loop at 0x%s has an unexpected body:\n%s"
                                % (m.group("first"), m.group(0)))
                continue
            expand, every = rule
            if every:
                failures.append("loop at 0x%s fetches every word in cached code" % m.group("first"))
            got_text = "".join(expand(first + 4 * k, indent) for k in range(n))
            want_text = "".join(template_for(first + 4 * k) for k in range(n))
            if got_text != want_text:
                failures.append("loop at 0x%s does not re-expand to the expanded form"
                                % m.group("first"))
    elif not failures:
        failures.append("run B did not provide all four address classes")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        raise SystemExit(1)
    print("zero-word run compaction test passed (%d loops, %d words compacted)"
          % (len(loops), sum(n for _, _, n in got)))


if __name__ == "__main__":
    main()
