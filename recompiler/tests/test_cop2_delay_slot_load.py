#!/usr/bin/env python3
"""Recompiler codegen regression test: MFC2 load delay, in a branch delay slot.

On the R3000A, MFC2/CFC2 write their GPR one instruction late, like LW. Tekken
2's vertex loop (USA Rev 1, 0x80072890) is software-pipelined around that:

    L:  mtc2  t0, VXY0          # reads the OLD t0: the vertex word from lw
        ...
        bgtz  t9, L
        mfc2  t0, SZ1           # delay slot; t0 lands after L's first insn

Writing t0 at the mfc2 hands VXY0 the depth SZ1 (Y = 0), so every fighter
vertex batch projects one long spike. This test builds that shape plus an
in-block `mfc2; addu` pair and checks the emitted C:

- the delay-slot MFC2 writes the psx_dsl_<slot> temp, not the GPR;
- the taken arm publishes the pending window before its interrupt edge, and
  the target exposes the old operand only during its first instruction;
- the fall-through arm (does not read t0 first) writes t0 back at once;
- the in-block `mfc2 t1; addu t2,t1,t1` pair defers t1 past the addu.

The emitted function also executes with the actual runtime IRQ, TCB save,
ChangeThread and fresh native-function re-entry, plus an ordinary no-IRQ run.

Usage:  python test_cop2_delay_slot_load.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, glob, os, re, struct, subprocess, sys, tempfile
from contextlib import nullcontext
from pathlib import Path

LOAD = 0x80010000
LOOP = LOAD + 0x08          # mtc2 t0, $0
SLOT = LOAD + 0x14          # mfc2 t0, $17 in bgtz's delay slot

WORDS = [
    0x24190002,  # 0x00 addiu t9, zero, 2
    0x8C880000,  # 0x04 lw    t0, 0(a0)
    0x48880000,  # 0x08 L: mtc2 t0, $0      (reads t0 first)
    0x2739FFFF,  # 0x0C addiu t9, t9, -1
    0x1F20FFFD,  # 0x10 bgtz  t9, L
    0x48088800,  # 0x14 mfc2  t0, $17       (delay slot)
    0x48099000,  # 0x18 mfc2  t1, $18
    0x01295021,  # 0x1C addu  t2, t1, t1    (in-block dependent successor)
    0x03E00008,  # 0x20 jr    ra
    0x00000000,  # 0x24 nop
]


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
        f.write(make_psxexe(LOAD, LOAD, b"".join(struct.pack("<I", w) for w in WORDS)))
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


def block(src, addr):
    """Text of block_<addr> up to the next block label."""
    m = re.search(r"\nblock_%08X:\n(.*?)(?=\nblock_[0-9A-F]{8}:\n|\Z)" % addr, src, re.S)
    return m.group(1) if m else ""


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build",
                                                          "psxrecomp-game")))
    ap.add_argument('--compiler', default='gcc')
    ap.add_argument('--guest', type=Path,
                    default=Path(here).parents[1] / 'runtime/tests/test_load_delay_generated.c')
    ap.add_argument('--control', action='store_true',
                    help='execute the old emitter for a behavior-based IRQ/TCB negative')
    ap.add_argument('--work-dir', type=Path,
                    help='retain one local failing fixture for native debugging')
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    with (nullcontext(str(args.work_dir)) if args.work_dir else tempfile.TemporaryDirectory()) as tmp:
        src = generate(args.recompiler, tmp)
        if not args.control:
            check_structure(src)
        execute(args, Path(tmp))


def check_structure(src):

    temp = "psx_dsl_%08X" % SLOT
    failures = []

    if not re.search(r"%s = gte_read_data\(cpu, 17\);" % temp, src):
        failures.append("delay-slot mfc2 does not write %s (t0 written early)" % temp)
    if re.search(r"cpu->gpr\[8\] = gte_read_data\(cpu, 17\);", src):
        failures.append("delay-slot mfc2 still writes cpu->gpr[8] directly")

    branch = block(src, LOAD + 0x08)
    taken, _, not_taken = branch.partition("} else {")
    stage = "dirty_ram_ld_delay_stage(cpu, 8u, %s, 0x%08Xu);" % (temp, LOOP)
    if stage not in taken or taken.find(stage) > taken.find('PSX_BLOCK_EDGE'):
        failures.append("taken arm must publish its load before the actual interrupt edge")
    precision = taken.find('PGXP_COP2(%s_meta_instr' % temp)
    if not 0 <= taken.find(stage) < precision < taken.find('PSX_BLOCK_EDGE'):
        failures.append('the old precision receipt must be captured before publishing the loaded shadow')
    if "cpu->gpr[8] = %s;" % temp not in not_taken:
        failures.append("fall-through arm does not write t0 back from %s" % temp)

    begin = branch.find('dirty_ram_ld_delay_begin(')
    op = branch.find('/* mtc2 */')
    end = branch.find('dirty_ram_ld_delay_end(')
    if not 0 <= begin < op < end:
        failures.append('the actual successor must execute inside the shared old-operand window')
    if 'psx_dslp_' in src:
        failures.append('guest pending writes must not be owned by a C-frame flag')

    pair = "psx_ldd_%08X" % (LOAD + 0x18)
    if not re.search(r"%s = gte_read_data\(cpu, 18\);" % pair, src):
        failures.append("in-block mfc2 t1; addu t2,t1,t1 is not a load-delay pair")
    elif not re.search(r"cpu->gpr\[10\] = cpu->gpr\[9\] \+ cpu->gpr\[9\];.*?"
                       r"cpu->gpr\[9\] = %s;" % pair, src, re.S):
        failures.append("t1 is written back before the addu reads it")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        raise SystemExit(1)
    print("COP2 load-delay test passed (slot %s, pair %s)" % (temp, pair))


def execute(args, work):
    runtime = args.guest.resolve().parents[1]
    sys.path.insert(0, str(runtime / 'tests'))
    from fixture_link import build_and_run
    from test_load_delay_irq import provider_source
    provider = work / 'production_load_window.c'
    provider.write_text(provider_source(runtime), encoding='utf-8')
    sources = list((work / 'out').glob('*_full*.c'))
    assert len(sources) == 1
    if not args.control:
        # The canonical precision helper is a separate C function and needs
        # its own instruction-local cookie declarations too.
        (work / 'psx_observation_config.h').write_text(
            '#pragma once\n#define PSX_OBSERVATION_WAIT_SITE(pc) 0\n', encoding='utf-8')
        subprocess.run([args.compiler, '-std=c11', '-Werror=implicit-function-declaration',
                        '-DPSX_NO_DEBUG_TOOLS=1', '-DPSX_PGXP=1',
                        '-DPSX_ENABLE_BLOCK_CYCLES=1', '-DPSX_PGXP_DEMAND_REGIONS=1',
                        '-DPSX_NATIVE_CPU_SPANS=1', '-DPSX_OBSERVATION_TIME=1',
                        '-I' + str(runtime / 'include'), '-I' + str(work), '-c', str(sources[0]),
                        '-o', str(work / 'precision-native.o')], check=True)
    modules = ['interrupts.c', 'traps.c', 'psx_bios_backend.c', str(provider)]
    for opt in ('-O0', '-O2'):
        for irq in ([True] if args.control else [False, True]):
            case = work / (opt[1:] + ('-irq' if irq else '-plain'))
            case.mkdir()
            definitions = ['PSX_NO_DEBUG_TOOLS=1',
                           'PSX_LOAD_DELAY_GENERATED="' + sources[0].as_posix() + '"']
            if not irq: definitions += ['FX_NO_IRQ=1']
            print(build_and_run(args.compiler, args.guest.parent, runtime, opt,
                                case, modules, args.guest.name,
                                defines=['PSX_NO_DEBUG_TOOLS=1'],
                                fixture_defines=definitions).rstrip(), flush=True)


if __name__ == "__main__":
    main()
