#!/usr/bin/env python3
"""Acceptance ledger for segment-aware code (docs/SEGMENT_AWARE_CODE.md).

A MIPS PC carries a segment: KUSEG 0x0xxxxxxx, KSEG0 0x8xxxxxxx or KSEG1
0xAxxxxxxx. Beetle (the oracle) keeps it in the link value a jal/jalr/bgezal
writes, in EPC, and in the I-cache tag. KSEG1 fetches are uncached and cost +4
each. Compiled code bakes a single segment into all of these.

This test synthesizes a KUSEG-linked PS-X EXE (tools/segment_testrom/
gen_segment_exe.py) and runs the real recompilers on it. It then checks what
they emit against a transcription of Beetle's fetch model. It uses:
  - the emitted PC constants;
  - the real emitted dispatch lookup, compiled and queried;
  - the runtime's own I-cache model (runtime/src/psx_icache.c), fed the
    emitted fetch sequence and compared with the per-instruction sequence
    Beetle would execute.

Each property the design must deliver is a check with a stable id. A failed
check is a gap. KNOWN_GAPS lists the gaps that master has today. The test
passes only when the set of observed gaps equals KNOWN_GAPS exactly:
  - a new gap is a regression;
  - a closed gap must be removed from the ledger in the same change.
The implementation PRs shrink KNOWN_GAPS to empty. The MODEL checks (Beetle
model == psx_icache.c, and the elision rule for cached code) must always pass.

Usage: python test_segment_aware_codegen.py --recompiler <psxrecomp-game>
           --bios-recompiler <psxrecomp-bios> [--compiler cc]
Exit 0 = the observed gaps equal the ledger and every model check passes.
"""
import argparse
import importlib.util
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
SEG_MASK, PHYS_MASK = 0xE0000000, 0x1FFFFFFF

# id -> (design section, what is missing). Remove an entry in the change
# that closes it; the test fails until the ledger matches.
KNOWN_GAPS = {}

# Closed ids that stay as regression guards: each reports a new gap if the
# property breaks again. id -> design section.
REGRESSION_GUARDS = {
    "bios-kseg1-fetch-charge": "5.6",   # PR A (#429)
    "bios-runtime-pc": "5.2",           # PR B: BIOS PCs handed to the runtime
    "store-pc-keys-runtime": "9",       # PR B: memory.c keys re-keyed
    # PR C: a KUSEG-linked EXE compiles for its link segment, and dispatch
    # is exact.
    "link-segment": "5.3",
    "fetch-tag-segment": "5.3",
    "irq-resume-segment": "5.3",
    "resume-pc-segment": "5.3",
    "store-pc-segment": "5.3",
    "home-seed-accepted": "5.3",
    "alias-fetch-coherence": "5.3",
    "segment-miss": "5.5",
    # PR D: segment-qualified seeds compile per-segment variants, and KSEG1
    # variants charge a fetch per instruction.
    "segment-variants": "5.4",
    "kseg1-fetch-charge": "5.6",
    # PR D: the call contract compares return PCs in full, in
    # psx_call_contract and in the BIOS dispatch loop alike.
    "exact-return-contract": "5.5",
    # PR E: overlay code captured at KUSEG/KSEG0/KSEG1 compiles one shard per
    # segment, each baking its own segment and a KSEG1 one charging every
    # fetch as Beetle does.
    "overlay-segment-shards": "5.7",
}


def load_generator():
    path = os.path.join(ROOT, "tools", "segment_testrom", "gen_segment_exe.py")
    spec = importlib.util.spec_from_file_location("gen_segment_exe", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# Beetle fetch model, transcribed from libretro/beetle-psx-libretro
# mednafen/psx/cpu.c @ a7f0811 (2026-09-27): ReadInstruction (lines 738-835)
# and CPU_SetBIU (484-505). The tag compares the FULL virtual address (Beetle's
# own comment at 719-730 notes that hardware strips bit 31 first). KSEG1, or a
# cache disabled in BIU, costs +4 per fetch and fills nothing. A cached miss
# costs 3, plus 1 for each word refilled from the missed word to the end of
# the line. The BIOS enables the I-cache during boot (BIU bit 11), so that is
# the state modeled here.
# ---------------------------------------------------------------------------
class BeetleICache:
    def __init__(self):
        self.tv = [0x2] * 1024   # CPU_Power with BIU enabled: TV = 0x2

    def fetch(self, addr):
        if self.tv[(addr & 0xFFC) >> 2] == addr:
            return 0
        if addr >= 0xA0000000:
            return 4
        line, base = addr & 0xFFFFFFF0, (addr & 0xFF0) >> 2
        for i in range(4):
            self.tv[base + i] = line | (i << 2) | 0x2
        cost = 3
        for i in range((addr & 0xC) >> 2, 4):
            self.tv[base + i] &= ~0x2
            cost += 1
        return cost


def beetle_cycles(seq):
    model = BeetleICache()
    return sum(model.fetch(a) for a in seq)


ICACHE_HARNESS = r"""
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
    CPUState cpu; char line[64];
    memset(&cpu, 0, sizeof cpu);
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'R') { psx_icache_reset(); g_psx_icache_active = 1; cycles = 0; }
        else if (line[0] == 'Q') { printf("%llu\n", cycles); }
        else psx_icache_fetch(&cpu, (uint32_t)strtoul(line, 0, 16));
    }
    return 0;
}
"""

# psx_call_contract (runtime/include/cpu_state.h), compiled as the game C and
# the interpreter use it. A return to another segment's alias of the call site
# is not a return to this site: it starts a bail, and a bail resolves only at
# the exact PC.
CONTRACT_HARNESS = r"""
#include "cpu_state.h"
#include <stdio.h>
#include <string.h>
int g_psx_call_bail;
uint64_t g_psx_bail_first, g_psx_bail_resolved, g_psx_bail_flattened, g_psx_bail_anomaly;
void psx_bail_record(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    (void)a; (void)b; (void)c; (void)d;
}
int main(void) {
    const uint32_t ra = 0x00010018u, sp = 0x801FFF00u;
    const uint32_t alias[] = {0x80010018u, 0xA0010018u};
    int bad = 0;
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.gpr[29] = sp; cpu.gpr[31] = ra;
    bad |= psx_call_contract(&cpu, ra, sp) != 0 || g_psx_call_bail;
    for (int i = 0; i < 2; ++i) {
        g_psx_call_bail = 0; cpu.pc = 0; cpu.gpr[31] = alias[i];
        bad |= (psx_call_contract(&cpu, ra, sp) != 1 || !g_psx_call_bail) << 1;
        cpu.pc = alias[i];
        bad |= (psx_call_contract(&cpu, ra, sp) != 1 || !g_psx_call_bail) << 2;
        cpu.pc = ra;
        bad |= (psx_call_contract(&cpu, ra, sp) != 0 || g_psx_call_bail) << 3;
    }
    printf("%d\n", bad);
    return 0;
}
"""

# The BIOS dispatch loop's return checks (return boundary, bail resolve, the
# wild-return test and the exception-stack straddle), as emitted.
BIOS_RETURN_CHECKS = (
    "if (cpu->pc == stop_addr) cpu->pc = 0;",
    "cpu->pc == stop_addr &&",
    "cpu->gpr[31] != stop_addr)) {",
    "(cpu->gpr[31] == stop_addr) &&",
)

DISPATCH_HARNESS_HEAD = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "psx_memory.h"
typedef struct { uint32_t pc; } CPUState;
static void dummy(CPUState* cpu) { (void)cpu; }
static int dirty_ram_text_native_ok_ranges_from(const uint32_t* r, uint32_t n, uint32_t a) {
    (void)r; (void)n; (void)a; return 1;
}
static int dirty_ram_text_native_ok_ranges(const uint32_t* r, uint32_t n) {
    return dirty_ram_text_native_ok_ranges_from(r, n, 0);
}
static void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; }
int psx_vsync_query_hle_try(CPUState* cpu, uint32_t a) { (void)cpu; (void)a; return 0; }
"""

DISPATCH_HARNESS_MAIN = r"""
int main(int argc, char** argv) {
    psx_ram_reset_size_request();
    psx_ram_apply_size_request();
    for (int i = 1; i < argc; ++i) {
        const PsxGameDispatchEntry* e = psx_game_find_entry((uint32_t)strtoul(argv[i], 0, 16));
        printf("%ld\n", e ? (long)(e - k_psx_game_dispatch) : -1L);
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


class ICacheModel:
    """runtime/src/psx_icache.c, compiled once and fed batches of sequences."""

    def __init__(self, compiler, tmp):
        src = os.path.join(tmp, "icache_harness.c")
        with open(src, "w") as f:
            f.write(ICACHE_HARNESS)
        self.exe = os.path.join(tmp, "icache_harness" + (".exe" if os.name == "nt" else ""))
        cc(compiler, [src, os.path.join(RUNTIME, "src", "psx_icache.c")],
           os.path.join(RUNTIME, "include"), self.exe,
           ("PSX_ENABLE_BLOCK_CYCLES=1", "PSX_OVERLAY_DLL_BUILD=1"))

    def cycles(self, seqs):
        script = "".join("R\n" + "".join("%08X\n" % a for a in s) + "Q\n" for s in seqs)
        out = subprocess.run([self.exe], input=script, capture_output=True, text=True,
                             check=True).stdout.split()
        return [int(x) for x in out]


def run_recompiler(binary, args):
    r = subprocess.run([binary] + args, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("%s failed:\n%s" % (os.path.basename(binary), r.stderr or r.stdout))
    return r.stdout + r.stderr


def parse_functions(out_dir, pattern):
    bodies = {}
    for name in sorted(os.listdir(out_dir)):
        if not re.match(pattern, name):
            continue
        with open(os.path.join(out_dir, name)) as f:
            text = f.read()
        for m in re.finditer(r"^(?:static )?void (func_[0-9A-F]{8})\(CPUState\* cpu\)\s*\{(.*?)^\}",
                             text, re.M | re.S):
            bodies[m.group(1)] = m.group(2)
    return bodies


PC_SITES = {
    "link-segment": r"cpu->gpr\[31\] = 0x([0-9A-F]{8})u;\s*/\* (?:jal|jalr|branch-and-link) ",
    "fetch-tag-segment": r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)",
    "irq-resume-segment": r"psx_check_interrupts_at\(cpu, 0x([0-9A-F]{8})u\)",
    "resume-pc-segment": r"cpu->pc = 0x([0-9A-F]{8})u;|case 0x([0-9A-F]{8})u: goto",
    # Not a debug-only breadcrumb: memory.c's RAM 0x0-0xF store filters compare
    # it with exact PCs in every build, and the interpreter stamps the full PC.
    "store-pc-segment": r"g_debug_last_store_pc = 0x([0-9A-F]{8})u;",
}


def pc_constants(body, pattern):
    for m in re.finditer(pattern, body):
        yield int(next(g for g in m.groups() if g), 16)


def block_fetches(body, block_va):
    """Emitted fetch tags from block_<va> up to the next block label."""
    m = re.search(r"^block_%08X:(.*?)(?=^block_[0-9A-F]{8}:|\Z)" % block_va, body, re.M | re.S)
    if not m:
        return None
    return [int(x, 16) for x in re.findall(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)", m.group(1))]


def bios_uncharged_kseg1(bios_c):
    """Emitted instructions that run at a KSEG1 PC without a fetch of their own.

    A fetch call names the runtime PC of the next emitted instruction. Its
    followers run at that PC plus their compile-address distance.
    """
    fetch = re.compile(r"psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\)")
    insn = re.compile(r"/\* 0x([0-9A-F]{8}): [0-9A-F]{8} ")
    func = re.compile(r"^(?:static )?void \w+\(CPUState")
    total = uncharged = 0
    pending = base = None
    for line in bios_c.splitlines():
        if func.match(line):
            pending = base = None
            continue
        m = fetch.search(line)
        if m:
            pending = int(m.group(1), 16)
            continue
        m = insn.search(line)
        if not m:
            continue
        rom = int(m.group(1), 16)
        if pending is not None:
            base, runtime, charged, pending = (rom, pending), pending, True, None
        elif base:
            runtime, charged = base[1] + (rom - base[0]), False
        else:
            continue
        if runtime >= KSEG1:
            total += 1
            uncharged += not charged
    return total, uncharged


def copy_windows(profile):
    """(rom_lo, rom_hi, runtime_base, name) of a BIOS profile's relocated copy
    windows; ROM ranges are [lo, hi).

    A regex, not tomllib, so the test runs on any Python 3 the build has.
    """
    with open(profile) as f:
        text = f.read()
    out = []
    for block in text.split("[[recompiler.address_model.copy]]")[1:]:
        lo = re.search(r'^rom_lo\s*=\s*"(0x[0-9A-Fa-f]+)"', block, re.M)
        hi = re.search(r'^rom_hi\s*=\s*"(0x[0-9A-Fa-f]+)"', block, re.M)
        rt = re.search(r'^runtime_base\s*=\s*"(0x[0-9A-Fa-f]+)"', block, re.M)
        name = re.search(r'^name\s*=\s*"([^"]*)"', block, re.M)
        if lo and hi and rt:
            out.append((int(lo.group(1), 16), int(hi.group(1), 16),
                        int(rt.group(1), 16), name.group(1) if name else "?"))
    return out


def in_rom_window(pc, windows):
    phys = pc & PHYS_MASK
    return any(lo <= phys < hi for lo, hi, _, _ in windows)


def in_runtime_window(pc, windows):
    return any(rt <= pc < rt + (hi - lo) for lo, hi, rt, _ in windows)


def window_runtime_pc(rom, windows):
    """BiosAddressModel::runtime_pc: a ROM PC inside a copy window runs at
    runtime_base + its offset; None outside every window."""
    phys = rom & PHYS_MASK
    for lo, hi, rt, _ in windows:
        if lo <= phys < hi:
            return rt + (phys - lo)
    return None


# PCs the BIOS emitter hands the runtime (§5.2). A relocated window runs at its
# RAM address, so none of these may name a ROM address inside a copy window.
BIOS_PC_SITES = {
    "store-pc": r"g_debug_last_store_pc = 0x([0-9A-F]{8})u;",
    "syscall-epc": r"cpu->pc = 0x([0-9A-F]{8})u; (?:if \()?psx_syscall",
    "break-pc": r"psx_break\(cpu, 0x[0-9A-F]+u, 0x([0-9A-F]{8})u\)",
    "unaligned-pc": r"psx_unaligned_access\(cpu, psx_addr, 0x([0-9A-F]{8})u\)",
    "fallthrough-pc": r"cpu->pc = 0x([0-9A-F]{8})u; return;  /\* fallthrough \*/",
}


def bios_rom_pcs(bios_c, windows):
    """{site: (sites seen, ROM-window PCs)} over the emitted BIOS."""
    out = {}
    for site, pattern in BIOS_PC_SITES.items():
        vals = [int(v, 16) for v in re.findall(pattern, bios_c)]
        out[site] = (len(vals), sorted({v for v in vals if in_rom_window(v, windows)}))
    return out


def memory_c_store_pc_keys():
    """runtime/src/memory.c's store-PC keys: (raw keys, gated pairs).

    Raw keys are compared with g_debug_last_store_pc directly. Gated pairs are
    scph1001_relocated_store(pc, rom) calls: keys for relocated SCPH-1001 code,
    which match only while that ROM instruction is what sits at pc.
    """
    with open(os.path.join(RUNTIME, "src", "memory.c")) as f:
        text = f.read()
    keys = [int(v, 16) for v in re.findall(r"g_debug_last_store_pc == 0x([0-9A-F]{8})u", text)]
    for body in re.findall(r"switch \(g_debug_last_store_pc\) \{(.*?)\n\s*\}", text, re.S):
        keys += [int(v, 16) for v in re.findall(r"case 0x([0-9A-F]{8})u:", body)]
    gated = [(int(pc, 16), int(rom, 16)) for pc, rom in re.findall(
        r"scph1001_relocated_store\(0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u\)", text)]
    return keys, gated


def overlay_shard_gaps(recompiler, gen, info, icache, tmp):
    """§5.7: the probe's overlay phase reads its own EXE file back into RAM at
    OVL_BUF and calls the copy of `ov_run` through KUSEG, KSEG0 and KSEG1.
    Build the capture record overlay_capture.c writes for that (schema v3),
    split it with compile_overlays.py's capture_segment_views(), and compile
    each view the way its fragment pass does (the entry has no callable
    boundary: a dispatch_root) with the real recompiler. Each shard must bake
    only its segment's PCs and name its entries by their VA, its manifest must
    carry S, and ov_run's straight run must cost what Beetle charges: cached
    at KUSEG/KSEG0 (line leaders), +4 per fetch at KSEG1. Returns a note, or
    None when every property holds."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import compile_overlays as co
    import base64
    import binascii
    L, ov = info["labels"], info["overlay"]
    data = gen.build()                   # the file: 2 KiB header, then the image
    buf, entry = ov["buffer"], ov["ov_run"]
    copy = lambda label: buf + 0x800 + L[label] - info["load"]
    executed = list(range(copy("ov_run"), copy("ov_getpc") + 8, 4))
    cap = {
        "schema": "psxrecomp overlay capture v3",
        "load_addr": "0x%08X" % (KSEG0 | buf), "size": len(data), "guard_bytes": 0,
        "bytes_b64": base64.b64encode(data).decode(),
        "executed_pcs": ["0x%08X" % (KSEG0 | pc) for pc in executed],
        "dispatch_entry_pcs": ["0x%08X" % (KSEG0 | entry)],
        "dispatch_entry_segments": {n: ["0x%08X" % (s | entry)] for n, s in
                                    (("kuseg", KUSEG), ("kseg0", KSEG0), ("kseg1", KSEG1))},
        "function_entry_pcs": [], "seeds": ["0x%08X" % (KSEG0 | entry)],
    }
    crc = binascii.crc32(data) & 0xFFFFFFFF
    straight = list(range(copy("ov_run_straight"), copy("ov_run_end"), 4))
    problems = []
    views = co.capture_segment_views(cap)
    if [int(v["load_addr"], 16) & SEG_MASK for v in views] != [KSEG0, KUSEG, KSEG1]:
        return "capture views: %s" % [v["load_addr"] for v in views]
    for view in views:
        load = int(view["load_addr"], 16)
        seg = load & SEG_MASK
        with co.image_segment(seg):
            _seeds, audit = co.classify_overlay_seeds(view, data, load, len(data), crc, {})
        # No callable boundary: the entry is an isolated-fragment demand, in
        # the view's segment.
        if audit["dispatch_fragment_demands"] != {seg | entry}:
            problems.append("%08X: fragment demands %s" % (
                seg, sorted("%08X" % a for a in audit["dispatch_fragment_demands"])))
        out = os.path.join(tmp, "ovl_%08X" % seg)
        os.makedirs(out)
        exe = os.path.join(out, "ovl.psx")
        with open(exe, "wb") as f:
            f.write(co.make_psxexe(load, seg | entry, data, guard_bytes=0))
        seeds_path = os.path.join(out, "seeds.txt")
        with open(seeds_path, "w") as f:
            f.write("dispatch_root 0x%08X\n" % (seg | entry))
        run_recompiler(recompiler, [exe, "--seeds", seeds_path, "--out-dir", out, "--overlay"])
        bodies = parse_functions(out, r".*_full\.c$")
        ranges = next(os.path.join(out, n) for n in os.listdir(out) if n.endswith("_full.ranges"))
        with co.image_segment(seg):
            func_ids = co.parse_overlay_func_ids(ranges, data, load, len(data))
        manifest = co.overlay_ranges_text(func_ids)
        want_names = {"func_%08X" % (seg | entry), "func_%08X" % (seg | copy("ov_getpc"))}
        if set(bodies) != want_names:
            problems.append("%08X: functions %s" % (seg, sorted(bodies)))
        foreign = sorted({pc for body in bodies.values() for pattern in PC_SITES.values()
                          for pc in pc_constants(body, pattern) if pc & SEG_MASK != seg})
        if foreign:
            problems.append("%08X: %d PCs baked in another segment, e.g. 0x%08X"
                            % (seg, len(foreign), foreign[0]))
        if (("S %08X\n" % seg in manifest) != (seg != KSEG0) or
                "F %08X " % (seg | entry) not in manifest):
            problems.append("%08X: manifest S/F records" % seg)
        body = bodies.get("func_%08X" % (seg | entry))
        tags = block_fetches(body, seg | copy("ov_run_straight")) if body else None
        want = beetle_cycles([seg | pc for pc in straight])
        got = icache.cycles([tags])[0] if tags else None
        if got != want:
            problems.append("%08X: straight run %s cycles, Beetle %d" % (seg, got, want))
    return "; ".join(problems) or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.join(ROOT, "recompiler", "build", "psxrecomp-game"))
    ap.add_argument("--bios-recompiler",
                    default=os.path.join(ROOT, "recompiler", "build", "psxrecomp-bios"))
    ap.add_argument("--compiler",
                    default=os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc"))
    args = ap.parse_args()
    for b in (args.recompiler, args.bios_recompiler):
        if not os.path.isfile(b):
            raise SystemExit("recompiler not found: %s (build it first)" % b)
    if not args.compiler:
        raise SystemExit("a C compiler is required")

    gen = load_generator()
    info = gen.probes()
    L = info["labels"]
    link = info["link_segment"]
    gaps, notes, model_fail = set(), {}, []

    def at(seg, label):
        return seg | (L[label] & PHYS_MASK)

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "segment_testrom.exe")
        with open(exe, "wb") as f:
            f.write(gen.build())
        seeds_home = os.path.join(tmp, "seeds_home.txt")
        seeds_all = os.path.join(tmp, "seeds_all.txt")
        with open(seeds_home, "w") as f:
            f.write("".join("0x%08X\n" % a for a in info["seeds"]["home"]))
        with open(seeds_all, "w") as f:
            f.write("".join("0x%08X\n" % a
                            for a in info["seeds"]["home"] + info["seeds"]["variants"]))

        # -- 3.2: a seed in the link segment is an entry of this EXE ----------
        log = run_recompiler(args.recompiler,
                             [exe, "--seeds", seeds_home, "--out-dir", os.path.join(tmp, "home")])
        m = re.search(r"Loaded (\d+) extra function addresses", log)
        loaded = int(m.group(1)) if m else -1
        if loaded != len(info["seeds"]["home"]):
            gaps.add("home-seed-accepted")
            notes["home-seed-accepted"] = "loaded %d of %d" % (loaded, len(info["seeds"]["home"]))

        out = os.path.join(tmp, "all")
        run_recompiler(args.recompiler, [exe, "--seeds", seeds_all, "--out-dir", out])
        bodies = parse_functions(out, r".*_full(_\d+)?\.c$")
        disp_name = next(n for n in os.listdir(out) if n.endswith("_dispatch.c"))
        with open(os.path.join(out, disp_name)) as f:
            dsrc = f.read()

        # -- the real emitted lookup, compiled and queried --------------------
        rows = [(int(a, 16), int(r, 16), fn) for a, r, fn in re.findall(
            r"\{0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, \d+u, \d+u, (func_[0-9A-F]{8})\}", dsrc)]
        begin = dsrc.index("int psx_game_address_in_text(uint32_t addr) {")
        end = dsrc.index("/* 1 iff addr is a re-enterable", begin)
        fragment = dsrc[begin:end]
        harness = DISPATCH_HARNESS_HEAD
        harness += "".join("#define %s dummy\n" % fn
                           for fn in sorted(set(re.findall(r"func_[0-9A-F]{8}", fragment))))
        harness += fragment + DISPATCH_HARNESS_MAIN
        hsrc = os.path.join(tmp, "dispatch_harness.c")
        with open(hsrc, "w") as f:
            f.write(harness)
        hexe = os.path.join(tmp, "dispatch_harness" + (".exe" if os.name == "nt" else ""))
        cc(args.compiler, [hsrc, os.path.join(RUNTIME, "src", "psx_ram_geometry.c")],
           os.path.join(RUNTIME, "include"), hexe)

        home_q = [at(link, n) for n in ("main", "leaf", "seeded", "probe_run", "getpc")]
        variant_q = [at(s, n) for s in (KSEG0, KSEG1) for n in ("probe_run", "getpc")]
        miss_q = [at(s, n) for s in (KSEG0, KSEG1) for n in ("leaf", "main")
                  if at(s, n) not in variant_q]
        queries = home_q + variant_q + miss_q
        res = subprocess.run([hexe] + ["%08X" % q for q in queries], capture_output=True,
                             text=True, check=True).stdout.split()
        found = {q: (rows[int(i)] if int(i) >= 0 else None) for q, i in zip(queries, res)}

        # -- 3.3: every compiled (segment, entry) has its own body, others miss
        bad = [q for q in variant_q if not found[q] or found[q][0] != q]
        if bad:
            gaps.add("segment-variants")
            notes["segment-variants"] = ", ".join(
                "0x%08X->%s" % (q, "0x%08X" % found[q][0] if found[q] else "miss") for q in bad)
        else:
            # A variant body is its segment's code identity: every PC it bakes
            # (links, fetch tags, IRQ resume PCs, exits and continuation keys,
            # store PCs) is in that segment, and so is its name.
            foreign = sorted({pc for q in variant_q for gid, pattern in PC_SITES.items()
                              for pc in pc_constants(bodies[found[q][2]], pattern)
                              if (pc & SEG_MASK) != (q & SEG_MASK)} |
                             {int(found[q][2][5:], 16) for q in variant_q
                              if (int(found[q][2][5:], 16) & SEG_MASK) != (q & SEG_MASK)})
            if foreign:
                gaps.add("segment-variants")
                notes["segment-variants"] = "%d PCs baked in another segment, e.g. 0x%08X" % (
                    len(foreign), foreign[0])
        wrong_home = [q for q in home_q if not found[q] or found[q][0] != q]
        stray = [q for q in miss_q if found[q]]
        if stray or wrong_home:
            gaps.add("segment-miss")
            notes["segment-miss"] = ", ".join(
                "0x%08X->0x%08X" % (q, found[q][0]) for q in stray + wrong_home if found[q])

        # -- 3.1: every PC a home body bakes is in the link segment ----------
        home_bodies = sorted({found[q][2] for q in home_q if found[q]}) or sorted(bodies)
        for gid, pattern in PC_SITES.items():
            off = sorted({pc for fn in home_bodies for pc in pc_constants(bodies[fn], pattern)
                          if (pc & SEG_MASK) != link})
            if gid == "resume-pc-segment":
                # The dispatch rows' keys and resume PCs are PCs too (§5.2):
                # dispatch sets cpu->pc = resume_pc before entering a body, so
                # each row's key and resume PC are in the segment of the body
                # it enters (the home body's, or a variant's, §5.4).
                off = sorted(set(off) | {pc for a, r, fn in rows for pc in (a, r)
                                         if pc and (pc & SEG_MASK) != (int(fn[5:], 16) & SEG_MASK)})
            if off:
                gaps.add(gid)
                notes[gid] = "%d constants, e.g. 0x%08X" % (len(off), off[0])

        icache = ICacheModel(args.compiler, tmp)
        straight = [pc & PHYS_MASK for pc in info["straight"]]
        leaf = [L["leaf"] & PHYS_MASK, (L["leaf"] & PHYS_MASK) + 4]

        # -- MODEL: psx_icache.c agrees with the Beetle transcription --------
        model_seqs = [
            [link | a for a in straight] * 2,                      # cold then warm, cached
            [KSEG0 | a for a in straight] + [KUSEG | a for a in straight],  # alias refill
            [KSEG1 | a for a in straight] * 2,                     # uncached both times
            [KSEG0 | (straight[0] + 8), KSEG0 | straight[0]],      # partial refill
        ]
        for seq, got in zip(model_seqs, icache.cycles(model_seqs)):
            if got != beetle_cycles(seq):
                model_fail.append("psx_icache.c %d != Beetle %d on %s" % (
                    got, beetle_cycles(seq), " ".join("%08X" % a for a in seq[:4])))

        # -- MODEL: leader-only fetch emission is exact for cached code ------
        home_run = found[at(link, "probe_run")]
        if home_run:
            tags = block_fetches(bodies[home_run[2]], home_run[0] - L["probe_run"] + L["probe_run_straight"])
            want = beetle_cycles([link | a for a in straight])
            got = icache.cycles([tags])[0] if tags else -1
            if got != want:
                model_fail.append("cached leader elision: emitted %d != Beetle %d" % (got, want))

        # -- 5: KSEG1 code charges +4 for every fetch -------------------------
        k1 = found[at(KSEG1, "probe_run")]
        tags = (block_fetches(bodies[k1[2]], at(KSEG1, "probe_run_straight"))
                if k1 and k1[0] == at(KSEG1, "probe_run") else None)
        want = beetle_cycles([KSEG1 | a for a in straight])
        got = icache.cycles([tags])[0] if tags else None
        if got != want:
            gaps.add("kseg1-fetch-charge")
            notes["kseg1-fetch-charge"] = ("no KSEG1 body" if got is None else
                                           "emitted %d, Beetle %d" % (got, want))

        # -- 3.1: interpreter and compiled body share I-cache lines ----------
        # The PR #417 shape: the interpreter (or other code) ran `leaf` at its
        # real PC, then the compiled body runs it. Beetle hits the second time.
        lf = found[at(link, "leaf")]
        tags = block_fetches(bodies[lf[2]], lf[0]) if lf else None
        interp = [link | a for a in leaf]
        want = beetle_cycles(interp + interp)
        got = icache.cycles([interp + tags])[0] if tags else None
        if got != want:
            gaps.add("alias-fetch-coherence")
            notes["alias-fetch-coherence"] = "emitted %s, Beetle %d" % (got, want)

        # -- 5.7: overlay code gets a shard per segment it was captured in ----
        note = overlay_shard_gaps(args.recompiler, gen, info, icache, tmp)
        if note:
            gaps.add("overlay-segment-shards")
            notes["overlay-segment-shards"] = note

    # -- 5: the BIOS emitter charges every KSEG1 (ROM) fetch ------------------
    openbios_toml = os.path.join(ROOT, "bios", "OpenBIOS.toml")
    with tempfile.TemporaryDirectory() as tmp:
        run_recompiler(args.bios_recompiler, ["--config", openbios_toml, "--out-dir", tmp])
        with open(os.path.join(tmp, "OpenBIOS_full.c")) as f:
            bios_c = f.read()
        with open(os.path.join(tmp, "OpenBIOS_dispatch.c")) as f:
            bios_dispatch = f.read()

        # -- 5.5: return PCs are compared in full (PR D) ----------------------
        # Closed by PR D, kept as a regression guard: psx_call_contract and
        # the emitted BIOS dispatch loop require the exact return PC.
        src = os.path.join(tmp, "contract.c")
        with open(src, "w") as f:
            f.write(CONTRACT_HARNESS)
        exe = os.path.join(tmp, "contract" + (".exe" if os.name == "nt" else ""))
        cc(args.compiler, [src], os.path.join(RUNTIME, "include"), exe)
        contract = int(subprocess.run([exe], capture_output=True, text=True,
                                      check=True).stdout.strip() or -1)
        masked = re.findall(r"\((?:cpu->pc|cpu->gpr\[31\]) \^ stop_addr\) & 0x1FFFFFFFu",
                            bios_dispatch)
        missing = [c for c in BIOS_RETURN_CHECKS if c not in bios_dispatch]
        if contract != 0 or masked or missing:
            gaps.add("exact-return-contract")
            parts = []
            if contract != 0:
                parts.append("psx_call_contract accepts another segment's alias "
                             "(check mask 0x%X)" % contract)
            if masked or missing:
                parts.append("%d masked, %d missing exact BIOS dispatch return checks"
                             % (len(masked), len(missing)))
            notes["exact-return-contract"] = "; ".join(parts)
    total, uncharged = bios_uncharged_kseg1(bios_c)
    if total == 0:
        model_fail.append("OpenBIOS: no KSEG1 instructions found (parser drift?)")
    elif uncharged:
        gaps.add("bios-kseg1-fetch-charge")
        notes["bios-kseg1-fetch-charge"] = "%d of %d KSEG1 instructions uncharged" % (uncharged, total)

    # -- 5.2: the BIOS emitter hands the runtime runtime PCs, not ROM ones ---
    # Closed by PR B, kept as a regression guard: a store-PC stamp, syscall
    # EPC, break/unaligned PC or fallthrough PC naming a ROM address inside a
    # relocated window reports the id again.
    windows = copy_windows(openbios_toml)
    if not windows:
        model_fail.append("OpenBIOS.toml: no copy windows parsed")
    rom_pcs = bios_rom_pcs(bios_c, windows)
    if not rom_pcs["store-pc"][0] or not rom_pcs["syscall-epc"][0]:
        model_fail.append("OpenBIOS: no store-PC or syscall sites found (parser drift?)")
    leaked = {k: v[1] for k, v in rom_pcs.items() if v[1]}
    # The output only shows the translations OpenBIOS exercises: most of the
    # orphaned-delay-slot paths never inline a PC-bearing instruction there.
    # So every StrictTranslator::translate() call in the full-function
    # emitter must also pass the runtime PC (relocate_ra).
    with open(os.path.join(ROOT, "recompiler", "src", "full_function_emitter.cpp")) as f:
        calls = re.findall(r"StrictTranslator::translate\(([^;]*?)\);", f.read())
    if not calls:
        model_fail.append("full_function_emitter.cpp: no StrictTranslator::translate calls found")
    unrouted = [c for c in calls if "relocate_ra(" not in c]
    if leaked or unrouted:
        gaps.add("bios-runtime-pc")
        parts = ["%d %s, e.g. 0x%08X" % (len(v), k, v[0]) for k, v in sorted(leaked.items())]
        if unrouted:
            parts.append("%d emitter translate() calls without a runtime PC, e.g. (%s)"
                         % (len(unrouted), unrouted[0]))
        notes["bios-runtime-pc"] = "; ".join(parts)

    # -- 9: memory.c store-PC keys are runtime PCs ---------------------------
    # The keys name SCPH-1001 stores. A key inside one of its relocated ROM
    # windows can only match a ROM-address stamp, which no backend makes. A
    # key for relocated code must be the runtime PC of its ROM store
    # (BiosAddressModel::runtime_pc) and go through the gated helper: a raw
    # RAM key would also match other BIOSes and game code at that address.
    scph_windows = copy_windows(os.path.join(ROOT, "bios", "SCPH1001.toml"))
    keys, gated = memory_c_store_pc_keys()
    if not keys or not gated or not scph_windows:
        model_fail.append("memory.c store-PC keys or SCPH1001.toml windows not parsed")
    bad = sorted({k for k in keys if in_rom_window(k, scph_windows)})
    bad += sorted({pc for pc, _ in gated if in_rom_window(pc, scph_windows)})
    raw_ram = sorted({k for k in keys if in_runtime_window(k, scph_windows)})
    mismatched = sorted((pc, rom) for pc, rom in gated
                        if window_runtime_pc(rom, scph_windows) != pc)
    if bad or raw_ram or mismatched:
        gaps.add("store-pc-keys-runtime")
        parts = []
        if bad:
            parts.append("%d ROM-address keys, e.g. 0x%08X" % (len(bad), bad[0]))
        if raw_ram:
            parts.append("%d ungated relocated-code keys, e.g. 0x%08X" % (len(raw_ram), raw_ram[0]))
        if mismatched:
            parts.append("%d keys that are not their ROM store's runtime PC, e.g. "
                         "0x%08X for 0x%08X" % (len(mismatched), mismatched[0][0], mismatched[0][1]))
        notes["store-pc-keys-runtime"] = "; ".join(parts)

    known = set(KNOWN_GAPS)
    print("segment-aware codegen ledger (docs/SEGMENT_AWARE_CODE.md):")
    for gid in sorted(known | gaps | set(REGRESSION_GUARDS)):
        state = ("gap" if gid in gaps else "closed")
        sec = KNOWN_GAPS.get(gid, (REGRESSION_GUARDS.get(gid, "?"), ""))[0]
        print("  %-24s %-6s section %-4s %s" % (gid, state, sec, notes.get(gid, "")))
    for msg in model_fail:
        print("  MODEL FAIL: " + msg)

    new, closed = gaps - known, known - gaps
    if new:
        print("FAIL: new gap(s), a regression: " + ", ".join(sorted(new)))
    if closed:
        print("FAIL: gap(s) closed; remove them from KNOWN_GAPS: " + ", ".join(sorted(closed)))
    if new or closed or model_fail:
        return 1
    print("PASS: %d known gap(s), model checks exact" % len(gaps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
