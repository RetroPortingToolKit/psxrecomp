#!/usr/bin/env python3
"""Recompiler codegen regression test: a KUSEG-linked EXE compiles for KUSEG,
segment-qualified seeds compile segment variants, and dispatch is exact
(docs/SEGMENT_AWARE_CODE.md §5.3, §5.4, §5.5).

PS1 segments alias the same physical RAM, but a PC carries its segment into
every link value, EPC and I-cache tag. Most PS-X EXE headers carry KSEG0
addresses (0x8001xxxx); some carry KUSEG ones (0x0001xxxx: Alien Resurrection
SLUS-00633, Kula World SCES-01000), and the program then runs at KUSEG PCs.

The recompiler used to fold such a header to KSEG0 and look PCs up by physical
address, so every KUSEG PC ran a body that baked KSEG0 links and tags, and a
KSEG0 or KSEG1 alias ran it too. Now:
  - the EXE parser keeps the header's segment (the link segment) and the
    image compiles there: rows, names and the PCs a body bakes are KUSEG;
  - the emitted lookup indexes the physical word and then requires the exact
    PC, so an alias PC with no body of its own misses; psx_game_address_in_text
    stays physical, which sends that segment miss to the interpreter;
  - a seed in the link segment is an entry; a seed for the same bytes in
    another segment is a variant request (§5.4), never folded into the home
    body: the home functions it reaches through direct edges are compiled
    again in its segment, with their own names, rows and baked PCs (a cached
    variant is the home C with the segment swapped; a KSEG1 one is charged a
    fetch per instruction), and several rows then share a physical word in
    both lookup forms. A jump-table case whose value is in another segment
    than its body tail-transfers instead of continuing locally, in a variant
    and in a home compile alike. A variant closure that reaches a data stub
    compiles it with no dispatch row, and one that reaches an alias entry
    compiles its whole alias group, whose shared body every shard can call
    through the declarations header. A variant seed that names no home
    dispatch entry, or that lies in a segment that does not map physical
    memory (0x20000000-0x7FFFFFFF, KSEG2), stops the build;
  - a header whose entry and load address are in different segments is
    rejected, a KSEG1-linked image compiles as uncached (a fetch charge per
    instruction), and a config code site in a foreign segment is refused.

The RAM-mirror contract is pinned against the runtime's real geometry
(runtime/src/psx_ram_geometry.c, linked in): a PC in the 2nd-4th RAM mirror is
a different code address, in retail 2 MiB and 8 MiB geometry alike, so it never
resolves to a compiled body and never counts as EXE text.

Usage:  python test_kuseg_dispatch_lookup.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile

LOAD = 0x00010000          # KUSEG: no KSEG bit, the case under test
PHYS_MASK = 0x1FFFFFFF
SEG_MASK = 0xE0000000
KUSEG, KSEG0, KSEG1 = 0x00000000, 0x80000000, 0xA0000000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)   # stack_base, as real headers carry
    return bytes(h) + data


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def build_body(load):
    # func A @ load: addiu sp,-8 ; sw ra,4(sp) ; jal B ; nop ; lw ra,4(sp) ;
    #                addiu sp,8 ; jr ra ; nop
    # func B @ load+0x20: jr ra ; nop
    a = [0x27BDFFF8, 0xAFBF0004, jal(load + 0x20), 0x00000000, 0x8FBF0004,
         0x27BD0008, 0x03E00008, 0x00000000]
    body = bytearray(w(a))
    body += b"\x00" * (0x20 - len(body))
    body += w([0x03E00008, 0x00000000])
    return bytes(body)


def build_jal_body():
    """Functions only a JAL scan finds, in a KUSEG image (§5.3).

    +00 main: jal B, then jal save with $a0 = $sp; +18 is the return from
        save, a SaveState-style continuation (save stores $ra through $a0).
    +28 a `j .` spin, so nothing before B ends in `jr $ra`.
    +30 B: reached only by main's jal.
    +3C save: sw $ra, 0($a0).
    """
    return w([0x27BDFFF8, 0xAFBF0004, jal(LOAD + 0x30), 0x00000000,
              jal(LOAD + 0x3C), 0x03A02025, 0x8FBF0004, 0x27BD0008,
              0x03E00008, 0x00000000,
              0x0800400A, 0x00000000,
              0x24020001, 0x03E00008, 0x00000000,
              0xAC9F0000, 0x03E00008, 0x00000000])


WIDE_SPAN = 0x200000       # the emitter indexes spans below this; wider ones bsearch


def build_wide_body():
    """Two leaf functions WIDE_SPAN apart: the emitted lookup must take its
    binary-search form (tables that span 2 MiB or more, e.g. 8 MiB titles)."""
    body = bytearray(WIDE_SPAN + 0x800)
    body[0:8] = w([0x03E00008, 0x00000000])
    body[WIDE_SPAN:WIDE_SPAN + 8] = w([0x03E00008, 0x00000000])
    return bytes(body)


def lookup_fragment(src):
    """psx_game_address_in_text .. psx_game_is_function_entry, and the func_
    names it references (bound to a fixture double by the harness)."""
    begin = src.index("int psx_game_address_in_text(uint32_t addr) {")
    entry_probe = src.index("int psx_game_is_function_entry(uint32_t addr) {", begin)
    end = src.index("}\n", entry_probe) + 2
    fragment = src[begin:end]
    return fragment, sorted(set(re.findall(r"func_[0-9A-Fa-f]{8}", fragment)))


def compile_and_run(compiler, harness, sources, include):
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "lookup.c")
        binary = os.path.join(tmp, "lookup.exe" if os.name == "nt" else "lookup")
        with open(source, "w", encoding="utf-8") as f:
            f.write(harness)
        compiler_name = os.path.basename(compiler).lower()
        if compiler_name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [compiler, "/nologo", "/Od", "/I" + include, source] + sources + \
                      ["/Fe:" + binary]
        else:
            command = [compiler, "-std=c11", "-O2", "-I", include, source] + sources + \
                      ["-o", binary]
        subprocess.run(command, check=True, cwd=tmp)
        subprocess.run([binary], check=True, cwd=tmp)


def compile_only(compiler, sources, includes, native_tail=False, opt='-O0'):
    """Compile `sources` without linking (syntax and declarations only)."""
    if os.path.basename(compiler).lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        command = [compiler, "/nologo", "/Zs"] + ["/I" + i for i in includes] + sources
    else:
        command = [compiler, "-std=c11", "-fsyntax-only",
                   "-Werror=implicit-function-declaration"] + \
                  [a for i in includes for a in ("-I", i)] + sources
    if native_tail:
        # Compile real generated products through musttail lowering, not only
        # front-end syntax. Objects remain in the existing temporary product.
        command.remove('-fsyntax-only')
        command[1:1] = ['-c', opt, '-DPSX_NO_DEBUG_TOOLS',
                        '-DPSX_NATIVE_CPU_SPANS=1', '-DPSX_NATIVE_STATIC_CALLS=1',
                        '-DPSX_NATIVE_TAIL_CHAINS=1']
    r = subprocess.run(command, capture_output=True, text=True,
                       cwd=os.path.dirname(sources[0]) if native_tail else None)
    return r.returncode == 0, r.stdout + r.stderr


def run(recompiler, args, cwd=ROOT):
    return subprocess.run([recompiler] + args, capture_output=True, text=True, cwd=cwd)


def generate(recompiler, tmp, name, entry, load, seeds, body=None):
    psx = os.path.join(tmp, name + ".psx")
    seed_path = os.path.join(tmp, name + ".seeds.txt")
    out = os.path.join(tmp, name)
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(make_psxexe(entry, load, body if body is not None else build_body(load)))
    with open(seed_path, "w") as f:
        f.write("".join("0x%08X\n" % s for s in seeds))
    r = run(recompiler, [psx, "--seeds", seed_path, "--out-dir", out])
    if r.returncode != 0:
        raise SystemExit("recompiler failed on %s:\n%s" % (name, r.stderr or r.stdout))
    disp = [f for f in os.listdir(out) if f.endswith("_dispatch.c")]
    if not disp:
        raise SystemExit("no _dispatch.c emitted in " + out)
    with open(os.path.join(out, disp[0])) as f:
        dsrc = f.read()
    full = ""
    for n in sorted(os.listdir(out)):
        if re.match(r".*_full(_\d+)?\.c$", n):
            with open(os.path.join(out, n)) as f:
                full += f.read()
    return dsrc, full, r.stdout


def bodies_of(full):
    """{func_ name: body text} of the emitted C."""
    return {m.group(1): m.group(2) for m in re.finditer(
        r"^void (func_[0-9A-F]{8})\(CPUState\* cpu\)\s*\{(.*?)^\}", full, re.M | re.S)}


def to_segment(text, seg):
    """Every 8-digit hex token of the image moved into `seg` (constants,
    labels and names alike)."""
    return re.sub(r"(?<![0-9A-Fa-f])([0-9A-F])([0-9A-F]{7})(?![0-9A-Fa-f])",
                  lambda m: "%08X" % (seg | (int(m.group(0), 16) & PHYS_MASK))
                  if (int(m.group(0), 16) & PHYS_MASK) >> 16 == (LOAD >> 16) else m.group(0),
                  text)


def build_switch_body(load=LOAD, case_seg=None):
    """A bounded jump table in an image at `load`: sltiu/beq guard, lw of a
    table of case PCs in `case_seg` (default: the image's own segment), jr.
    Each case branches to one shared jr $ra."""
    if case_seg is None:
        case_seg = load & SEG_MASK
    body = w([0x3C080000 | (load >> 16), 0x25100100, 0, 0x2C620003, 0x10400010, 0,
              0x00031080, 0x00501021, 0x8C420000, 0, 0x00400008, 0,
              0x24020001, 0x10000007, 0, 0x24020002, 0x10000004, 0,
              0x24020003, 0x10000001, 0, 0x03E00008, 0])
    body += b"\x00" * (0x100 - len(body))
    phys = load & PHYS_MASK
    return body + w([case_seg | (phys + 0x30), case_seg | (phys + 0x3C), case_seg | (phys + 0x48)])


def build_stub_body():
    """A function whose jal target decodes as data: the target is a data stub
    (psx_unknown_dispatch, no dispatch row), in the home compile and in a
    variant that reaches it."""
    return w([0x27BDFFF8, 0xAFBF0004, jal(LOAD + 0x20), 0, 0x8FBF0004, 0x27BD0008,
              0x03E00008, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF])


def build_alias_body(n=40):
    """One straight-line host function with interior seeds at +0x10 and +0x20:
    an alias group (one shared psx_alias_body_ body, a wrapper per entry)."""
    return w([0x24420001] * n + [0x03E00008, 0])


def rows_of(src):
    tbl = re.search(r"k_psx_game_dispatch\[\] = \{(.*?)\n\};", src, re.DOTALL)
    if not tbl:
        raise SystemExit("dispatch table not found in emitted dispatch")
    return [(int(a, 16), int(r, 16), fn) for a, r, fn in re.findall(
        r"\{0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, \d+u, \d+u, (func_[0-9A-F]{8})\}", tbl.group(1))]


def check(cond, msg):
    if not cond:
        raise SystemExit("FAIL: " + msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(HERE, "..", "build",
                                                          "psxrecomp-game")))
    ap.add_argument("--compiler", default=os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc"))
    ap.add_argument('--native-tail', action='store_true', help='compile eligible JAL and alias fallback products with supported musttail ABI')
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        # -- the KUSEG image: seeds in the link segment, plus one KSEG0 seed ---
        src, full, log = generate(args.recompiler, tmp, "kuseg", LOAD, LOAD,
                                  [LOAD, LOAD + 0x20, KSEG0 | (LOAD + 0x20)])
        check("Loaded 2 extra function addresses" in log,
              "KUSEG seeds in the link segment must be loaded (log: %r)"
              % re.findall(r"Loaded .*", log))
        check("variant seed 0x80010020 (entry, KSEG0): home PC would be 0x00010020" in log
              and "Segment variant 0x80000000" in log,
              "a KSEG0 seed for a KUSEG image must be compiled as a variant request")
        rows = rows_of(src)
        check(len(rows) >= 3, "expected at least three dispatch entries, got %d" % len(rows))
        check(all((a & SEG_MASK) == (int(fn[5:], 16) & SEG_MASK) and
                  (r == 0 or (r & SEG_MASK) == (a & SEG_MASK)) for a, r, fn in rows),
              "every row key and resume PC is in its body's segment: %s" % rows)
        check(all(r != 0 or fn == "func_%08X" % a for a, r, fn in rows),
              "entry rows name their own body: %s" % rows)
        check([(a, fn) for a, _, fn in rows if (a & SEG_MASK) != KUSEG] ==
              [(KSEG0 | (LOAD + 0x20), "func_80010020")],
              "the only row outside KUSEG is the requested KSEG0 variant: %s" % rows)
        check({a for a, r, _ in rows} >= {LOAD, LOAD + 0x20}, "both functions have rows")
        bodies = bodies_of(full)
        check({"func_00010000", "func_00010020", "func_80010020"} <= set(bodies),
              "bodies are named by their PC, the variant's included (§5.1): %s" % sorted(bodies))
        home = bodies["func_00010000"] + bodies["func_00010020"]
        check("cpu->gpr[31] = 0x00010010u;" in home, "the jal links a KUSEG PC")
        check("psx_icache_fetch(cpu, 0x00010000u)" in home, "fetch tags are KUSEG")
        check(not re.search(r"0x8001[0-9A-F]{4}u", home),
              "no KSEG0 PC of the image appears in the KUSEG bodies")
        if args.native_tail:
            check('PSX_TRY_NATIVE_TAIL(' in home, 'ordinary known JAL emits a guaranteed-tail candidate')
            ordinary_dir = os.path.join(tmp, 'kuseg')
            for opt in ('-O0', '-O2'):
                ok, output = compile_only(args.compiler,
                    sorted(os.path.join(ordinary_dir, n) for n in os.listdir(ordinary_dir) if n.endswith('.c')),
                    [ordinary_dir, os.path.join(ROOT, 'runtime', 'include')], native_tail=True, opt=opt)
                check(ok, 'actual generated ordinary musttail products compile '+opt+':\n'+output[-3000:])
        keys = [a & PHYS_MASK for a, _, _ in rows]
        check(keys == sorted(keys), "dispatch table is sorted by physical address")

        m = re.search(r"static const PsxGameDispatchEntry\* psx_game_find_entry"
                      r"\(uint32_t addr\) \{(.*?)\n\}", src, re.DOTALL)
        check(m, "psx_game_find_entry not found in emitted dispatch")
        body = m.group(1)
        check("0x1FFFFFFFu" in body and "k_psx_game_dispatch_index" in body,
              "small resident tables index the physical word")
        check("k_psx_game_dispatch[row].addr == addr" in body,
              "a word with rows in two segments is searched for the exact PC")

        # -- segment variants: the direct-edge closure, per segment (§5.4) ----
        vsrc, vfull, vlog = generate(
            args.recompiler, tmp, "variants", LOAD, LOAD,
            [LOAD, LOAD + 0x20, KSEG0 | LOAD, KSEG1 | LOAD, KSEG0 | (LOAD + 0x10)])
        vrows = {a: (r, fn) for a, r, fn in rows_of(vsrc)}
        for seg in (KSEG0, KSEG1):
            name = "func_%08X" % (seg | LOAD)
            want = {seg | LOAD: (0, name), seg | (LOAD + 0x10): (seg | (LOAD + 0x10), name),
                    seg | (LOAD + 0x20): (0, "func_%08X" % (seg | (LOAD + 0x20)))}
            got = {a: v for a, v in vrows.items() if (a & SEG_MASK) == seg}
            check(got == want, "the 0x%08X variant is A, its call return and its jal callee B "
                  "(the closure), with rows in that segment: %s" % (seg, got))
        vbodies = bodies_of(vfull)
        for fn in ("func_00010000", "func_00010020"):
            v0 = "func_%08X" % (KSEG0 | int(fn[5:], 16))
            check(to_segment(vbodies[fn], KSEG0) == vbodies[v0],
                  "a cached variant is the home C with the segment swapped (%s)" % v0)
            v1 = vbodies["func_%08X" % (KSEG1 | int(fn[5:], 16))]
            insns = re.findall(r"/\* 0x([0-9A-F]{8}): ", v1)
            fetched = set(re.findall(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)", v1))
            check(insns and all(i in fetched and i.startswith("A") for i in insns),
                  "every instruction of a KSEG1 variant is charged its own fetch (§5.6)")
            check(not re.search(r"0x[08]001[0-9A-F]{4}u", v1),
                  "a KSEG1 variant bakes no KUSEG or KSEG0 PC of the image")

        # A variant seed must name a home dispatch entry.
        psx = os.path.join(tmp, "variants.psx")
        bad_seeds = os.path.join(tmp, "unplaced.seeds.txt")
        with open(bad_seeds, "w") as f:
            f.write("0x%08X\n0x%08X\n" % (LOAD, KSEG0 | (LOAD + 4)))
        r = run(args.recompiler, [psx, "--seeds", bad_seeds, "--out-dir",
                                  os.path.join(tmp, "unplaced")])
        check(r.returncode != 0 and "variant seed 0x80010004 cannot be compiled" in r.stderr,
              "a variant seed with no home dispatch entry stops the build:\n%s"
              % (r.stderr or r.stdout)[-2000:])

        # A jump-table case is local only in its own segment: the KSEG0
        # variant of a KUSEG switch tail-transfers every KUSEG case value.
        _, sfull, _ = generate(args.recompiler, tmp, "switch", LOAD, LOAD,
                               [LOAD, KSEG0 | LOAD], body=build_switch_body())
        sbodies = bodies_of(sfull)
        check("/* jump table 0x00010100" in sbodies["func_00010000"] and
              re.search(r"case 0x00010030u:\s*\n(.*\n)*?\s*goto block_00010030;",
                        sbodies["func_00010000"]),
              "the home body switches locally on its own segment's case PCs")
        v = sbodies["func_80010000"]
        check("jump table" not in v and not re.search(r"case 0x0001[0-9A-F]{4}u", v) and
              "cpu->pc = _jt_80010028; return;" in v,
              "the KSEG0 variant tail-transfers a KUSEG case value to dispatch")

        # A home compile follows the same rule. Its tables normally hold its
        # own segment's PCs and stay local switches; a table whose values are
        # another segment's PCs of the image's bytes (a KSEG0 image whose table
        # holds KUSEG PCs) dispatches those cases with their full PC, where
        # they are segment misses, instead of continuing in the KSEG0 body.
        for case_seg, local in ((KSEG0, True), (KUSEG, False)):
            name = "switch_k0_" + ("own" if local else "kuseg")
            _, hfull, _ = generate(args.recompiler, tmp, name, KSEG0 | LOAD, KSEG0 | LOAD,
                                   [KSEG0 | LOAD], body=build_switch_body(KSEG0 | LOAD, case_seg))
            h = bodies_of(hfull)["func_80010000"]
            if local:
                check("/* jump table 0x80010100" in h and
                      re.search(r"case 0x80010030u:\s*\n(.*\n)*?\s*goto block_80010030;", h),
                      "a KSEG0 home body switches locally on its own segment's case PCs")
            else:
                check("jump table" not in h and not re.search(r"case 0x0001[0-9A-F]{4}u", h) and
                      "cpu->pc = _jt_80010028; return;" in h,
                      "a KSEG0 home body tail-transfers a KUSEG case value to dispatch")

        # Only KUSEG below 0x20000000, KSEG0 and KSEG1 map the image's bytes. A
        # seed in 0x20000000-0x7FFFFFFF or in KSEG2 names no code of the image:
        # it stops the build with the link-segment spelling of its bytes.
        for bad in (0x20000000 | LOAD, 0xC0000000 | LOAD):
            bad_seeds = os.path.join(tmp, "badseg.seeds.txt")
            with open(bad_seeds, "w") as f:
                f.write("0x%08X\n0x%08X\n" % (LOAD, bad))
            r = run(args.recompiler, [psx, "--seeds", bad_seeds, "--out-dir",
                                      os.path.join(tmp, "badseg")])
            check(r.returncode != 0 and
                  ("seed 0x%08X is in segment 0x%08X, which does not map physical memory"
                   % (bad, bad & SEG_MASK)) in r.stderr and
                  ("are 0x%08X in its link segment" % LOAD) in r.stderr and
                  "Segment variant" not in r.stdout,
                  "a seed in a segment that does not map physical memory stops the build:\n%s"
                  % (r.stderr or r.stdout)[-2000:])

        # A variant closure that reaches a data stub compiles the stub in its
        # segment, and the stub gets no dispatch row, as at home.
        dsrc, dfull, _ = generate(args.recompiler, tmp, "stub", LOAD, LOAD, [LOAD, KSEG0 | LOAD],
                                  body=build_stub_body())
        drows = {a for a, _, _ in rows_of(dsrc)}
        check("psx_unknown_dispatch(cpu, 0x80010020u, 0x00010020u);" in
              bodies_of(dfull).get("func_80010020", ""),
              "the variant's jal target is compiled as its data stub")
        check((KSEG0 | LOAD) in drows and not drows & {LOAD + 0x20, KSEG0 | (LOAD + 0x20)},
              "a data stub has no dispatch row, at home or in a variant: %s"
              % sorted("%08X" % a for a in drows))

        # A variant seed at an alias entry compiles its alias group: one shared
        # psx_alias_body_ in the variant's segment and a wrapper per entry.
        # The shards share one declarations header; every alias body a shard
        # defines is declared there, so a wrapper in another shard can call it
        # (compiled below against the header alone), and every emitted file
        # compiles.
        _, afull, _ = generate(args.recompiler, tmp, "alias", LOAD, LOAD,
                               [LOAD, LOAD + 0x10, LOAD + 0x20, KSEG0 | (LOAD + 0x10)],
                               body=build_alias_body())
        abodies = bodies_of(afull)
        check("psx_alias_body_80010000(cpu, 0x80010010u);" in abodies.get("func_80010010", "") and
              "psx_alias_body_80010000(cpu, 0x80010020u);" in abodies.get("func_80010020", ""),
              "an alias entry's variant brings its whole alias group: %s" % sorted(abodies))
        adir = os.path.join(tmp, "alias")
        hosts = sorted(set(re.findall(r"^void (psx_alias_body_[0-9A-F]{8})\(", afull, re.M)))
        check(hosts == ["psx_alias_body_00010000", "psx_alias_body_80010000"],
              "the home and the variant alias bodies are emitted: %s" % hosts)
        if args.compiler:
            probe = os.path.join(tmp, "alias_decls_probe.c")
            with open(probe, "w") as f:
                f.write('#include "alias.psx_decls.h"\n'
                        "void (*const alias_bodies_probe[])(CPUState*, uint32_t) = {%s};\n"
                        % ", ".join(hosts))
            sources = [probe] + sorted(os.path.join(adir, n) for n in os.listdir(adir)
                                       if n.endswith(".c"))
            ok, out = compile_only(args.compiler, sources, [adir, os.path.join(ROOT, "runtime", "include")])
            check(ok, "every alias body is declared in the shared header and the variant "
                  "output compiles:\n" + out[-3000:])
            if args.native_tail:
                for host in hosts:
                    alias_body = re.search(r'^void '+re.escape(host)+r'\(CPUState\* cpu, uint32_t entry\)\s*\{(.*?)^\}', afull, re.M | re.S)
                    check(alias_body and 'PSX_TRY_NATIVE_TAIL(' not in alias_body.group(1),
                          'two-argument alias body keeps its portable CPS transfer')
                for opt in ('-O0', '-O2'):
                    ok, output = compile_only(args.compiler, sources,
                        [adir, os.path.join(ROOT, 'runtime', 'include')], native_tail=True, opt=opt)
                    check(ok, 'actual generated alias fallback products compile '+opt+':\n'+output[-3000:])


        # -- a table spanning 2 MiB or more takes the binary-search form ------
        wide_src, _, _ = generate(args.recompiler, tmp, "kuseg_wide", LOAD, LOAD,
                                  [LOAD, LOAD + WIDE_SPAN, KSEG1 | (LOAD + WIDE_SPAN)],
                                  body=build_wide_body())
        wide_rows = rows_of(wide_src)
        check({LOAD, LOAD + WIDE_SPAN, KSEG1 | (LOAD + WIDE_SPAN)} <=
              {a for a, _, _ in wide_rows} and
              all((a & SEG_MASK) == KUSEG for a, _, _ in wide_rows
                  if a != KSEG1 | (LOAD + WIDE_SPAN)),
              "the wide KUSEG image has KUSEG rows at both ends and one KSEG1 variant: %s"
              % wide_rows)
        wm = re.search(r"static const PsxGameDispatchEntry\* psx_game_find_entry"
                       r"\(uint32_t addr\) \{(.*?)\n\}", wide_src, re.DOTALL)
        check(wm and "k_psx_game_dispatch_index" not in wm.group(1) and
              "while (lo < hi)" in wm.group(1) and
              "k_psx_game_dispatch[row].addr == addr" in wm.group(1),
              "a table spanning 2 MiB or more binary-searches the physical word, "
              "then the exact PC among the word's rows")

        # -- JAL-driven discovery takes the call's segment, not KSEG0 --------
        jsrc, _, jlog = generate(args.recompiler, tmp, "kuseg_jal", LOAD, LOAD, [],
                                 body=build_jal_body())
        jrows = {a: (r, fn) for a, r, fn in rows_of(jsrc)}
        check(jrows.get(LOAD + 0x30) == (0, "func_00010030"),
              "a function reached only by jal is discovered in a KUSEG image: %s" % jrows)
        check(jrows.get(LOAD + 0x18) == (0, "func_00010018"),
              "the SaveState-style continuation after jal save is an entry: %s" % jrows)
        check("Identified 1 SaveState continuation entries" in jlog,
              "the SaveState scan follows the KUSEG jal")

        # -- a header whose entry is in another segment than its load address
        psx = os.path.join(tmp, "mixed.psx")
        with open(psx, "wb") as f:
            f.write(make_psxexe(KSEG0 | LOAD, LOAD, build_body(LOAD)))
        r = run(args.recompiler, [psx, "--out-dir", os.path.join(tmp, "mixed")])
        check(r.returncode != 0 and "different segments" in (r.stdout + r.stderr),
              "an entry and load address in different segments must be rejected")

        # -- a KSEG1-linked image: uncached home segment -----------------------
        _, k1_full, _ = generate(args.recompiler, tmp, "kseg1", KSEG1 | LOAD, KSEG1 | LOAD,
                                 [KSEG1 | LOAD, KSEG1 | (LOAD + 0x20)])
        body_a = re.search(r"void func_A0010000\(CPUState\* cpu\)\s*\{(.*?)^\}", k1_full,
                           re.M | re.S)
        check(body_a, "the KSEG1 image's entry is named func_A0010000")
        insns = re.findall(r"/\* 0x([0-9A-F]{8}): ", body_a.group(1))
        fetched = set(re.findall(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)", body_a.group(1)))
        check(insns and all(i in fetched for i in insns),
              "every KSEG1 instruction is charged its own fetch (§5.6)")

        # -- config code sites are code identities in the link segment ---------
        exe = os.path.join(tmp, "kuseg.psx")
        for site, ok in ((KSEG0 | (LOAD + 0x20), False), (LOAD + 0x20, True)):
            cfg = os.path.join(tmp, "game_%08X.toml" % site)
            # TOML literal strings: a Windows path's backslashes are not escapes.
            with open(cfg, "w") as f:
                f.write("[game]\nname = 't'\nexe = '%s'\ntext_size = '0x%X'\n"
                        "[recompiler]\nseeds = '%s'\nout_dir = '%s'\n"
                        "hot_funcs = ['0x%08X']\n"
                        % (exe, len(build_body(LOAD)), os.path.join(tmp, "kuseg.seeds.txt"),
                           os.path.join(tmp, "cfg_%08X" % site), site))
            r = run(args.recompiler, ["--config", cfg, "--project-root", ROOT])
            if ok:
                check(r.returncode == 0, "a link-segment config site is accepted:\n%s"
                      % (r.stderr or r.stdout)[-2000:])
                # The seeds ask for a KSEG0 variant of that function: a config
                # code site applies to every segment compiled for its bytes.
                hot = ""
                for n in os.listdir(os.path.join(tmp, "cfg_%08X" % site)):
                    if n.endswith(".c"):
                        with open(os.path.join(tmp, "cfg_%08X" % site, n)) as f:
                            hot += f.read()
                for fn in ("func_00010020", "func_80010020"):
                    check("__attribute__((hot))\n#endif\nvoid %s(CPUState* cpu)" % fn in hot,
                          "the link-segment hot_funcs site reaches %s" % fn)
            else:
                check(r.returncode != 0 and
                      "recompiler.hot_funcs: 0x80010020 -> 0x00010020" in r.stderr,
                      "a KSEG0 config site for a KUSEG image must be refused:\n%s"
                      % (r.stderr or r.stdout)[-2000:])

        # load_accel return PCs are compared exactly with $ra by the runtime.
        cfg = os.path.join(tmp, "game_horizon.toml")
        with open(cfg, "w") as f:
            f.write("[game]\nname = 't'\nexe = '%s'\ntext_size = '0x%X'\n"
                    "[recompiler]\nseeds = '%s'\nout_dir = '%s'\n"
                    "[load_accel.vsync_query]\nfunc = '0x%08X'\n"
                    "counter_addr = '0x80090000'\ngpustat_ptr_addr = '0x80090004'\n"
                    "timer1_ptr_addr = '0x80090008'\ntimer1_cache_addr = '0x8009000C'\n"
                    "event_horizon_extra_sites = ['0x%08X']\n"
                    % (exe, len(build_body(LOAD)), os.path.join(tmp, "kuseg.seeds.txt"),
                       os.path.join(tmp, "cfg_horizon"), LOAD, KSEG0 | (LOAD + 0x10)))
        r = run(args.recompiler, ["--config", cfg, "--project-root", ROOT])
        check(r.returncode != 0 and
              "load_accel.vsync_query.event_horizon_extra_sites: 0x80010010 -> 0x00010010"
              in r.stderr,
              "a KSEG0 event-horizon return PC for a KUSEG image must be refused:\n%s"
              % (r.stderr or r.stdout)[-2000:])

        # A kind the emitter matches by physical address (a full-word-guarded
        # substitution, like a byte patch) reaches the same bytes in any
        # spelling, so it is not refused, and it still applies.
        lui_exe = os.path.join(tmp, "kuseg_lui.psx")
        with open(lui_exe, "wb") as f:
            f.write(make_psxexe(LOAD, LOAD, w([0x3C020140, 0x03E00008, 0x00000000])))
        lui_seeds = os.path.join(tmp, "kuseg_lui.seeds.txt")
        with open(lui_seeds, "w") as f:
            f.write("0x%08X\n" % LOAD)
        cfg = os.path.join(tmp, "game_bound.toml")
        with open(cfg, "w") as f:
            f.write("[game]\nname = 't'\nexe = '%s'\ntext_size = '0xC'\n"
                    "[recompiler]\nseeds = '%s'\nout_dir = '%s'\n"
                    "[[widescreen.signed_x_bound]]\naddress = '0x%08X'\n"
                    "expected = '0x3C020140'\n"
                    % (lui_exe, lui_seeds, os.path.join(tmp, "cfg_bound"), KSEG0 | LOAD))
        r = run(args.recompiler, ["--config", cfg, "--project-root", ROOT])
        check(r.returncode == 0, "a physically matched config site is accepted in any "
              "segment:\n%s" % (r.stderr or r.stdout)[-2000:])
        bound_c = ""
        for n in os.listdir(os.path.join(tmp, "cfg_bound")):
            if n.endswith(".c"):
                with open(os.path.join(tmp, "cfg_bound", n)) as f:
                    bound_c += f.read()
        check("psx_ws_player_x_bound((int32_t)0x01400000)" in bound_c,
              "the physically matched signed_x_bound site still applies")

        # Seeds-file directives name code by PC too: a foreign spelling is
        # refused with the link-segment one, not a bare "invalid" message.
        for directive, want in (
                ("producer_range 0x80010000 0x80010028",
                 "write its addresses as: 0x00010000 0x00010028"),
                ("cross_call_allow 0x80010020", "write its addresses as: 0x00010020"),
                ("hosted_interior 0x80010010 0x00010000",
                 "write its addresses as: 0x00010010 0x00010000")):
            seeds = os.path.join(tmp, "directive.seeds.txt")
            with open(seeds, "w") as f:
                f.write("0x%08X\n%s\n" % (LOAD, directive))
            r = run(args.recompiler, [exe, "--seeds", seeds, "--out-dir",
                                      os.path.join(tmp, "directive")])
            check(r.returncode != 0 and want in r.stderr,
                  "a KSEG0 %r for a KUSEG image must be refused with its KUSEG "
                  "spelling:\n%s" % (directive, (r.stderr or r.stdout)[-2000:]))

    # Compile the REAL emitted lookup, validity gates and dispatch function,
    # linked against the runtime's REAL live-geometry state machine
    # (runtime/src/psx_ram_geometry.c + psx_memory.h), so the contracts below
    # are checked against exactly what the runtime links -- no copies. Only
    # peripheral callbacks and generated MIPS bodies are fixture doubles.
    if not args.compiler:
        raise SystemExit("a C compiler is required for the dispatch regression")
    runtime = os.path.join(ROOT, "runtime")
    include = os.path.join(runtime, "include")
    fragment, funcs = lookup_fragment(src)
    harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "psx_memory.h"
int psx_mod_set_main_ram_8mb(int enabled);   /* runtime psx_ram_geometry.c */
typedef struct { uint32_t pc; } CPUState;
static int allowed = 1, calls = 0, irqs = 0;
static uint32_t resumed;
static void dummy(CPUState* cpu) { calls++; resumed = cpu->pc; }
static int dirty_ram_text_native_ok_ranges_from(const uint32_t* r, uint32_t n, uint32_t a) {
    (void)r; (void)n; (void)a; return allowed;
}
static int dirty_ram_text_native_ok_ranges(const uint32_t* r, uint32_t n) {
    return dirty_ram_text_native_ok_ranges_from(r, n, 0);
}
static void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t a) {
    (void)cpu; (void)a; irqs++;
}
int psx_vsync_query_hle_try(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
"""
    harness += "\n".join(f"#define {fn} dummy" for fn in funcs) + "\n" + fragment
    harness += r"""
static void set_geometry(int expanded) {
    psx_ram_reset_size_request();
    if (expanded) psx_mod_set_main_ram_8mb(1);
    psx_ram_apply_size_request();
    assert(memory_get_ram_bytes() == (expanded ? 0x00800000u : 0x00200000u));
}

int main(void) {
    const uint32_t segments[] = {0x00000000u, 0x80000000u, 0xA0000000u};
    CPUState cpu = {0};
    unsigned home_hits = 0;
    /* Retail (8 MB mod off) first, then the expanded map: code identity is the
     * full PC in BOTH, so geometry never changes which compiled body a PC may
     * run. */
    for (int expanded = 0; expanded < 2; ++expanded) {
        set_geometry(expanded);
        for (unsigned s = 0; s < 3; ++s) {
            for (uint32_t phys = 0xFFF0u; phys < 0x10080u; ++phys) {
                uint32_t addr = segments[s] | phys;
                /* Exact: only a row compiled for this very PC answers. The
                 * image is KUSEG-linked, so its KSEG0 and KSEG1 aliases have
                 * no body and miss (a segment miss, interpreted by the
                 * runtime), while text membership stays physical. */
                const PsxGameDispatchEntry* expected = 0;
                for (unsigned i = 0; i < PSX_GAME_DISPATCH_COUNT; ++i)
                    if (k_psx_game_dispatch[i].addr == addr)
                        expected = &k_psx_game_dispatch[i];
                assert(psx_game_find_entry(addr) == expected);
                assert(psx_game_is_function_entry(addr) == (expected != 0));
                /* Outside KUSEG only the requested variant has a body. */
                if (segments[s] != 0x00000000u) assert(!expected || addr == 0x80010020u);
                if (addr == 0x80010020u) assert(expected && expected->addr == addr);
                int has_home = 0;
                for (unsigned i = 0; i < PSX_GAME_DISPATCH_COUNT; ++i)
                    if ((k_psx_game_dispatch[i].addr & 0x1FFFFFFFu) == phys) has_home = 1;
                if (has_home) assert(psx_game_address_in_text(addr));
                if (has_home && !expected) {
                    int old_calls = calls, old_irqs = irqs;
                    cpu.pc = 0xDEADBEEFu;
                    assert(!psx_game_text_native_ok(addr));
                    assert(!psx_game_text_native_ok_full(addr));
                    assert(!psx_dispatch_game_compiled(&cpu, addr));
                    assert(calls == old_calls && irqs == old_irqs && cpu.pc == 0xDEADBEEFu);
                }
                /* A PC in a 2nd-4th RAM mirror keeps its own address: the
                 * compiled body bakes the PCs it was compiled for into
                 * $ra/EPC, so it must never run for a mirror PC, and the
                 * mirror is not the EXE text. (Retail executes the folded
                 * bytes through the interpreter, with the mirror PC.) */
                for (uint32_t m = 1; m < 4; ++m) {
                    const uint32_t mirror = addr + m * 0x00200000u;
                    int old_calls = calls, old_irqs = irqs;
                    cpu.pc = 0xDEADBEEFu;
                    assert(psx_game_find_entry(mirror) == 0);
                    assert(!psx_game_address_in_text(mirror));
                    assert(!psx_game_text_native_ok(mirror));
                    assert(!psx_dispatch_game_compiled(&cpu, mirror));
                    assert(calls == old_calls && irqs == old_irqs &&
                           cpu.pc == 0xDEADBEEFu);
                }
                if (!expected) continue;
                home_hits++;
                // A previously resolved PC must NOT cache its live-byte verdict.
                allowed = 0; cpu.pc = 0xDEADBEEFu;
                int old_calls = calls, old_irqs = irqs;
                assert(!psx_game_text_native_ok(addr));
                assert(!psx_dispatch_game_compiled(&cpu, addr));
                assert(calls == old_calls && irqs == old_irqs && cpu.pc == 0xDEADBEEFu);
                allowed = 1;
                assert(psx_dispatch_game_compiled(&cpu, addr));
                assert(calls == old_calls + 1 && irqs == old_irqs + 1);
                assert(resumed == expected->resume_pc);
            }
        }
        assert(!psx_game_find_entry(0xFFFFFFFFu));
        assert(!psx_game_find_entry(0));
    }
    assert(home_hits >= 6);   /* both functions and the variant, both geometries */
    return 0;
}
"""
    compile_and_run(args.compiler, harness,
                    [os.path.join(runtime, "src", "psx_ram_geometry.c")], include)

    # The binary-search form of the same lookup (a table spanning 2 MiB or
    # more): every row answers only for its exact PC, in every segment; the
    # other segments' aliases and the words next to a row miss and never run.
    wide_fragment, wide_funcs = lookup_fragment(wide_src)
    wide = r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef struct { uint32_t pc; } CPUState;
static int calls = 0;
static void dummy(CPUState* cpu) { (void)cpu; calls++; }
static int dirty_ram_text_native_ok_ranges_from(const uint32_t* r, uint32_t n, uint32_t a) {
    (void)r; (void)n; (void)a; return 1;
}
static int dirty_ram_text_native_ok_ranges(const uint32_t* r, uint32_t n) {
    return dirty_ram_text_native_ok_ranges_from(r, n, 0);
}
static void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; }
int psx_vsync_query_hle_try(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
"""
    wide += "\n".join(f"#define {fn} dummy" for fn in wide_funcs) + "\n" + wide_fragment
    wide += r"""
int main(void) {
    const uint32_t segments[] = {0x00000000u, 0x80000000u, 0xA0000000u};
    CPUState cpu = {0};
    unsigned hits = 0;
    assert(PSX_GAME_DISPATCH_COUNT >= 2);
    for (unsigned i = 0; i < PSX_GAME_DISPATCH_COUNT; ++i) {
        const uint32_t phys = k_psx_game_dispatch[i].addr & 0x1FFFFFFFu;
        /* Each word once: a variant's row follows its home row's word. */
        if (i && (k_psx_game_dispatch[i - 1].addr & 0x1FFFFFFFu) == phys) continue;
        for (unsigned s = 0; s < 3; ++s) {
            for (int d = -4; d <= 4; d += 4) {
                const uint32_t addr = segments[s] | (phys + (uint32_t)d);
                const PsxGameDispatchEntry* expected = 0;
                for (unsigned j = 0; j < PSX_GAME_DISPATCH_COUNT; ++j)
                    if (k_psx_game_dispatch[j].addr == addr) expected = &k_psx_game_dispatch[j];
                assert(psx_game_find_entry(addr) == expected);
                assert(psx_game_is_function_entry(addr) == (expected != 0));
                /* Outside KUSEG only the requested KSEG1 variant has a body. */
                if (segments[s] != 0x00000000u)
                    assert(!expected || addr == (0xA0000000u | (0x00010000u + 0x200000u)));
                const int before = calls;
                assert(psx_dispatch_game_compiled(&cpu, addr) == (expected != 0));
                assert(calls == before + (expected != 0));
                hits += expected != 0;
            }
        }
    }
    assert(hits == PSX_GAME_DISPATCH_COUNT);
    return 0;
}
"""
    compile_and_run(args.compiler, wide, [], include)

    print("KUSEG-linked EXE test passed (%d rows, exact lookup in the indexed and "
          "binary-search forms with variant rows, unrequested aliases miss, variants "
          "compiled as their direct-edge closure, cached variants equal the home C, KSEG1 "
          "variants and images charged per instruction, foreign jump-table cases "
          "dispatched at home and in variants, data stubs rowless, variant alias groups "
          "declared for every shard, seeds outside the physical segments refused, foreign "
          "config sites and directives refused, physically matched "
          "sites accepted, mirror PCs never run compiled code in 2 or 8 MiB geometry)"
          % len(rows))


if __name__ == "__main__":
    main()
