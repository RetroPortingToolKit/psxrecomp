#!/usr/bin/env python3
"""Compiled BIOS code charges instruction fetch exactly as the interpreter does.

Beetle's ReadInstruction, which runtime/src/psx_icache.c transcribes, never
fills an I-cache line for a fetch at 0xA0000000 or above. Every such fetch
costs +4 and clears the pending load give-back. The dirty-RAM interpreter
reaches that charge by calling psx_icache_fetch_interp at every PC. The BIOS
main ROM runs in place at KSEG1 (0xBFC0....), so compiled ROM code must charge
a fetch before EVERY instruction. The emitters used to charge only at block
leaders and 16-byte line starts, which is exact only for cached code. That
left 5,477 of OpenBIOS's 9,592 KSEG1 instruction sites 4 cycles short, each
also missing its give-back clear.

The test compiles runtime/src/psx_icache.c into a small harness and drives it
two ways. "Compiled" runs only the fetches the emitter wrote
(psx_icache_fetch). "Interpreter" runs a fetch at every PC
(psx_icache_fetch_interp, the dirty-RAM interpreter's own entry point). Each
instruction's (cycles, give-back cleared) pair must be the same both ways:

  model    psx_icache.c (both entry points) equals a transcription of Beetle
           ReadInstruction on synthetic sequences: uncached repeat, cached cold
           then warm, KSEG0 -> KUSEG alias refill, partial refill.
  bios     psxrecomp-bios on bios/OpenBIOS.toml. Every straight-line run from
           a label is replayed from a cold cache: KSEG1 runs (ROM in place)
           and cached runs (kernel at 0x500+, shell at 0x80030000+) alike.
           Every KSEG1 instruction must carry a fetch naming its own runtime
           PC. Cached runs pin the line-start test to the runtime PC:
           OpenBIOS copies its kernel from ROM 0x1FC1E4D4 to RAM 0x500, so a
           ROM-address test charged each kernel line crossing one
           instruction early (a hit) and missed the real one.
  stubs    The A0/B0/C0 call-vector native stubs charge one fetch per
           executed word, so a KSEG1 call pays like the interpreter.

The game/overlay emitter's half is uncached_fetch_codegen_test.cpp.

Usage: test_uncached_fetch_charge.py --bios-recompiler <psxrecomp-bios> [--compiler cc]
Exit 0 = every compared instruction matches.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RUNTIME = os.path.join(ROOT, "runtime")

KUSEG, KSEG0, KSEG1 = 0x00000000, 0x80000000, 0xA0000000


def uncached(pc):
    """psx_fetch_uncached (runtime/include/psx_instr_cost.h)."""
    return pc >= 0xA0000000


# ---------------------------------------------------------------------------
# Beetle fetch model, transcribed from libretro/beetle-psx-libretro
# mednafen/psx/cpu.c @ a7f0811: ReadInstruction (738-835) with the I-cache on
# (BIU bit 11 set, as the BIOS leaves it). The tag compares the full virtual
# address. A miss clears the load give-back. KSEG1 then costs +4 and fills
# nothing. A cached miss costs 3, plus 1 for each word refilled from the
# missed word to the end of the line. (Same transcription as
# test_segment_aware_codegen.py in PR #419.)
# ---------------------------------------------------------------------------
class BeetleICache:
    def __init__(self):
        self.tv = [0x2] * 1024

    def fetch(self, addr):
        """(cycles, give-back cleared) for one fetch."""
        if self.tv[(addr & 0xFFC) >> 2] == addr:
            return 0, 0
        if addr >= 0xA0000000:
            return 4, 1
        line, base = addr & 0xFFFFFFF0, (addr & 0xFF0) >> 2
        for i in range(4):
            self.tv[base + i] = line | (i << 2) | 0x2
        cost = 3
        for i in range((addr & 0xC) >> 2, 4):
            self.tv[base + i] &= ~0x2
            cost += 1
        return cost, 1


def beetle_per_insn(pcs):
    model = BeetleICache()
    return [model.fetch(pc) for pc in pcs]


# One line of input per instruction:
#   R            reset the cache (cold, as psx_icache_reset leaves it)
#   E <hex>      compiled instruction whose emitted fetch names <hex>
#   N            compiled instruction with no emitted fetch
#   I <hex>      interpreter instruction at <hex> (psx_icache_fetch_interp)
# Every E/N/I line prints "<cycles> <cleared>" for that instruction.
HARNESS = r"""
#include "cpu_state.h"
#include "psx_icache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int g_ls_replay_active = 0;
static unsigned long long cycles;
void psx_advance_cycles(uint32_t c) { cycles += c; }
void psx_cpu_charge(uint32_t c) { cycles += c; }
int main(void) {
    static CPUState cpu;
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        char op = line[0];
        if (op == 'R') { psx_icache_reset(); g_psx_icache_active = 1; continue; }
        if (op != 'E' && op != 'N' && op != 'I') continue;
        unsigned long long before = cycles;
        cpu.read_absorb_which = 1u;
        cpu.read_absorb[1] = 1u;
        if (op == 'E') psx_icache_fetch(&cpu, (uint32_t)strtoul(line + 2, 0, 16));
        if (op == 'I') psx_icache_fetch_interp(&cpu, (uint32_t)strtoul(line + 2, 0, 16));
        printf("%llu %d\n", cycles - before,
               (cpu.read_absorb_which == 0u && cpu.read_absorb[1] == 0u) ? 1 : 0);
    }
    return 0;
}
"""


def cc(compiler, sources, include, out, defines=()):
    name = os.path.basename(compiler).lower()
    if name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        cmd = [compiler, "/nologo", "/Od", "/I" + include] + ["/D" + d for d in defines]
        cmd += sources + ["/Fe:" + out]
    else:
        cmd = [compiler, "-std=c11", "-O2", "-I", include] + ["-D" + d for d in defines]
        cmd += sources + ["-o", out]
    subprocess.run(cmd, check=True, cwd=os.path.dirname(out))


class Harness:
    def __init__(self, compiler, tmp):
        src = os.path.join(tmp, "fetch_harness.c")
        with open(src, "w") as f:
            f.write(HARNESS)
        self.exe = os.path.join(tmp, "fetch_harness" + (".exe" if os.name == "nt" else ""))
        cc(compiler, [src, os.path.join(RUNTIME, "src", "psx_icache.c")],
           os.path.join(RUNTIME, "include"), self.exe,
           ("PSX_ENABLE_BLOCK_CYCLES=1", "PSX_OVERLAY_DLL_BUILD=1"))

    def run(self, programs):
        """programs: lists of ('E', tag) / ('N', None) / ('I', pc), each run
        from a cold cache. Returns one [(cycles, cleared)] list per program."""
        script = []
        for prog in programs:
            script.append("R\n")
            for op, pc in prog:
                script.append("N\n" if op == "N" else "%s %08X\n" % (op, pc))
        out = subprocess.run([self.exe], input="".join(script), capture_output=True,
                             text=True, check=True).stdout.split("\n")
        vals = [tuple(int(x) for x in ln.split()) for ln in out if ln.strip()]
        res, i = [], 0
        for prog in programs:
            res.append(vals[i:i + len(prog)])
            i += len(prog)
        if i != len(vals):
            raise SystemExit("harness output length mismatch")
        return res


def interp_program(pcs):
    return [("I", pc) for pc in pcs]


# ---------------------------------------------------------------------------
# BIOS emitter output parsing
# ---------------------------------------------------------------------------
FUNC_RE = re.compile(r"^(?:static )?void \w+\(CPUState")
LABEL_RE = re.compile(r"^label_([0-9A-F]{8}):")
FETCH_RE = re.compile(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)")
INSN_RE = re.compile(r"/\* (DELAY \(orphaned\) )?0x([0-9A-F]{8}): [0-9A-F]{8} ")


def bios_runs(text, problems):
    """Straight-line runs of emitted instructions, split at labels, function
    starts and address gaps. Each instruction is (runtime_pc, fetch tag or
    None). Runtime PC = ROM address + the function's relocation delta, taken
    from its fetches (relocate_ra moves a whole window, so one function has one
    delta).

    An in-function instruction's fetch precedes its comment. An orphaned delay
    slot (inlined after its jr/branch) prints its comment first and then its
    fetch, interlock and body, so a fetch that directly follows an orphan's
    comment (only preprocessor lines between) belongs to the orphan."""
    runs = []
    run = None
    delta = None
    pending = None
    last_rom = None
    orphan = None   # [rom, run insns index] of an orphan still awaiting its fetch

    def close():
        nonlocal run
        if run and run["insns"]:
            runs.append(run)
        run = None

    for line in text.splitlines():
        if FUNC_RE.match(line):
            close()
            delta = pending = last_rom = orphan = None
            continue
        m = LABEL_RE.match(line)
        if m:
            close()
            orphan = None
            run = {"from_label": True, "label": int(m.group(1), 16), "insns": []}
            continue
        m = FETCH_RE.search(line)
        if m and orphan is not None:
            rom, idx = orphan
            orphan = None
            tag = int(m.group(1), 16)
            d = (tag - rom) & 0xFFFFFFFF
            if delta is None or d != delta:
                problems.append("orphan fetch 0x%08X names another window than ROM 0x%08X"
                                % (tag, rom))
            elif idx is not None:
                run["insns"][idx] = (run["insns"][idx][0], tag)
            continue
        if orphan is not None and not line.lstrip().startswith("#"):
            orphan = None
        if m:
            if pending is not None:
                problems.append("two fetches with no instruction between them before 0x%08X" %
                                int(m.group(1), 16))
            pending = int(m.group(1), 16)
            continue
        m = INSN_RE.search(line)
        if not m:
            continue
        rom = int(m.group(2), 16)
        if run is None or (last_rom is not None and rom != last_rom + 4):
            close()
            run = {"from_label": False, "label": rom, "insns": []}
        last_rom = rom
        tag = pending
        pending = None
        if tag is not None:
            d = (tag - rom) & 0xFFFFFFFF
            if delta is None:
                delta = d
            elif d != delta:
                problems.append("fetch 0x%08X names another window than ROM 0x%08X" % (tag, rom))
        if delta is None:
            run.setdefault("unmapped", 0)
            run["unmapped"] = run["unmapped"] + 1
            orphan = [rom, None] if m.group(1) else None
            continue
        run["insns"].append(((rom + delta) & 0xFFFFFFFF, tag))
        orphan = [rom, len(run["insns"]) - 1] if m.group(1) and tag is None else None
    close()
    return runs


STUB_FETCH_RE = re.compile(r"psx_icache_fetch\(cpu, addr(?: \+ (\d+)u)?\);")


def stub_shapes(dispatch_c):
    """Fetch offsets and step count for each shape of the native call stub."""
    begin = dispatch_c.index("static int psx_bios_try_native_call_stub(")
    end = dispatch_c.index("\n}\n", begin)
    body = dispatch_c[begin:end]
    shapes = {}
    for name in ("shape A", "shape B"):
        s = body.index(name)
        e = body.find("} else", s)
        seg = body[s:e if e >= 0 else len(body)]
        events = re.findall(r"psx_icache_fetch\(cpu, addr(?: \+ \d+u)?\);|psx_cyc_step\(", seg)
        offs = [int(m.group(1) or 0) for m in STUB_FETCH_RE.finditer(seg)]
        shapes[name] = (offs, events)
    return shapes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios-recompiler",
                    default=os.path.join(ROOT, "recompiler", "build", "psxrecomp-bios"))
    ap.add_argument("--compiler",
                    default=os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc"))
    args = ap.parse_args()
    if not os.path.isfile(args.bios_recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.bios_recompiler)
    if not args.compiler:
        raise SystemExit("a C compiler is required")
    if os.environ.get("PSX_CODEGEN_CYCLE_PER_INSN", "1")[:1] == "0":
        print("SKIP: PSX_CODEGEN_CYCLE_PER_INSN=0 emits no fetch charges")
        return 0

    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        harness = Harness(args.compiler, tmp)

        # -- model: psx_icache.c (compiled and interpreter entry points) == Beetle
        straight = [0x00010100 + 4 * i for i in range(10)]
        synthetic = {
            "uncached repeat": [KSEG1 | a for a in straight] * 2,
            "cached cold then warm": [KSEG0 | a for a in straight] * 2,
            "KSEG0 then KUSEG alias": [KSEG0 | a for a in straight] + [KUSEG | a for a in straight],
            "partial refill": [KSEG0 | (straight[0] + 8), KSEG0 | straight[0]],
        }
        names = list(synthetic)
        got_i = harness.run([interp_program(synthetic[n]) for n in names])
        got_e = harness.run([[("E", pc) for pc in synthetic[n]] for n in names])
        for n, gi, ge in zip(names, got_i, got_e):
            want = beetle_per_insn(synthetic[n])
            if gi != want or ge != want:
                failures.append("model %s: psx_icache.c interp %s / compiled %s != Beetle %s"
                                % (n, gi, ge, want))
        # The old leader-only rule on an uncached run must be caught (the
        # comparison below is sensitive to exactly the bug this fixes).
        k1 = [KSEG1 | a for a in straight]
        leader_only = [("E", pc) if pc == k1[0] or (pc & 0xC) == 0 else ("N", None) for pc in k1]
        old, ref = harness.run([leader_only, interp_program(k1)])
        if old == ref:
            failures.append("model: leader-only KSEG1 emission was not distinguishable from the interpreter")

        # -- bios: every emitted run vs the interpreter at every PC ------------
        out = os.path.join(tmp, "bios")
        os.makedirs(out)
        r = subprocess.run([args.bios_recompiler, "--config",
                            os.path.join(ROOT, "bios", "OpenBIOS.toml"), "--out-dir", out],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("psxrecomp-bios failed:\n%s" % (r.stderr or r.stdout))
        with open(os.path.join(out, "OpenBIOS_full.c")) as f:
            full_c = f.read()
        with open(os.path.join(out, "OpenBIOS_dispatch.c")) as f:
            dispatch_c = f.read()

        problems = []
        runs = bios_runs(full_c, problems)
        failures += problems
        unmapped = sum(r.get("unmapped", 0) for r in runs)
        if unmapped:
            failures.append("bios: %d instructions precede any fetch in their function" % unmapped)
        # A run that does not start at a label is entered only by fall-through
        # from code outside it (an orphaned delay slot's neighbour). Cached
        # instructions there depend on earlier lines; compare them from their
        # first fetch. Uncached instructions never depend on history.
        compiled, interp, kinds, uncached_sites, uncached_missing = [], [], [], 0, 0
        for run in runs:
            insns = run["insns"]
            if not run["from_label"]:
                first = next((i for i, (_, t) in enumerate(insns) if t is not None), len(insns))
                insns = [x for i, x in enumerate(insns) if i >= first or uncached(x[0])]
            if not insns:
                continue
            for pc, tag in insns:
                if uncached(pc):
                    uncached_sites += 1
                    if tag != pc:
                        uncached_missing += 1
            compiled.append([("E", t) if t is not None else ("N", None) for _, t in insns])
            interp.append(interp_program([pc for pc, _ in insns]))
            kinds.append(insns)
        got_c = harness.run(compiled)
        got_i = harness.run(interp)
        mismatch = {"uncached": 0, "cached": 0}
        cycles_short = 0
        beetle_bad = 0
        first_bad = None
        total = 0
        for insns, gc, gi in zip(kinds, got_c, got_i):
            want = beetle_per_insn([pc for pc, _ in insns])
            if gi != want:
                beetle_bad += sum(1 for a, b in zip(gi, want) if a != b)
            for (pc, _), c, i in zip(insns, gc, gi):
                total += 1
                if c != i:
                    mismatch["uncached" if uncached(pc) else "cached"] += 1
                    cycles_short += i[0] - c[0]
                    if first_bad is None:
                        first_bad = (pc, c, i)
        print("bios: %d runs, %d instructions compared (%d at KSEG1)"
              % (len(kinds), total, uncached_sites))
        if uncached_sites == 0:
            failures.append("bios: no KSEG1 instructions found (parser drift?)")
        if uncached_missing:
            failures.append("bios: %d of %d KSEG1 instruction sites have no fetch naming their own PC"
                            % (uncached_missing, uncached_sites))
        if mismatch["uncached"] or mismatch["cached"]:
            pc, c, i = first_bad
            failures.append("bios: compiled != interpreter on %d KSEG1 and %d cached instructions "
                            "(%d cycles short over one pass), first at 0x%08X: compiled %s, "
                            "interpreter %s" % (mismatch["uncached"], mismatch["cached"],
                                                cycles_short, pc, c, i))
        if beetle_bad:
            failures.append("bios: psx_icache.c interpreter path != Beetle on %d instructions"
                            % beetle_bad)

        # -- stubs: one fetch per executed word, in fetch-then-step order -----
        want_offs = {"shape A": [0, 4, 8, 12], "shape B": [0, 4, 8]}
        shapes = stub_shapes(dispatch_c)
        progs, refs, labels = [], [], []
        for name, (offs, events) in shapes.items():
            if offs != want_offs[name]:
                failures.append("stub %s: fetch offsets %s, executed words %s"
                                % (name, offs, want_offs[name]))
            steps = [e for e in events if e.startswith("psx_cyc_step")]
            order_ok = len(events) == 2 * len(steps) and all(
                events[2 * k].startswith("psx_icache_fetch") and
                events[2 * k + 1].startswith("psx_cyc_step") for k in range(len(steps)))
            if not order_ok:
                failures.append("stub %s: fetch/step order %s" % (name, events))
            for seg in (KUSEG, KSEG0, KSEG1):
                base = seg | 0xA0
                progs.append([("E", base + o) if o in offs else ("N", None)
                              for o in want_offs[name]])
                refs.append(interp_program([base + o for o in want_offs[name]]))
                labels.append("%s at 0x%08X" % (name, base))
        for label, gc, gi in zip(labels, harness.run(progs), harness.run(refs)):
            if gc != gi:
                failures.append("stub %s: compiled %s != interpreter %s" % (label, gc, gi))

    for msg in failures:
        print("FAIL: " + msg)
    if failures:
        return 1
    print("PASS: compiled fetch charges == interpreter == Beetle (OpenBIOS, stubs, model)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
