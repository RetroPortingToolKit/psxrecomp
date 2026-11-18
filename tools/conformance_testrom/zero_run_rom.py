#!/usr/bin/env python3
"""Zero-word-run execution fixture.

Builds a tiny PS-X EXE, a cooked ISO9660 image that boots it, and a game
project directory whose psx-cosim target runs it. The program loops forever
through zero-word runs while VBlank IRQs are enabled, so compiled-vs-interp
cosim sees the compacted zero-run loops executed millions of times with IRQs
landing at many positions inside them:

  ZR_A    2000 zero words, then jr ra          entered at its first word
  ZR_MID  = ZR_A + 1000 words (JAL target)     entered mid-run (function split)
  ZR_B    1500 zero words, then a counted      re-entered through a backward
          backward branch to ZR_B + 700 words  branch target (mid-run label)
  every run falls off its end into real code.

A loop counter at 0x80020000 increments once per pass.

The system area (sectors 0-15, license text) is copied from a disc image you
own (--license-from, a raw 2352-byte MODE2 .bin); nothing retail is written
anywhere else, and nothing this script writes is meant to be committed.

Usage:
  python zero_run_rom.py --output <dir> --framework-root <psxrecomp>
                         --license-from <disc.bin>
Then:
  <recompiler>/psxrecomp-game --config game.toml     (cwd = <dir>)
  cmake -S <dir> -B <dir>/build-cosim -G Ninja -DCMAKE_BUILD_TYPE=Release
        -DPSX_RECOMP_UI=OFF -DPSX_REWIND=OFF -DPSX_NETPLAY=OFF
  cmake --build <dir>/build-cosim --target psx-cosim
  COSIM_EXE=<dir>/build-cosim/psx-cosim.exe COSIM_GAME=<dir>/game.toml
  python tools/cosim.py --a compiled --b interp --stride 65536 --max N
"""
import argparse, os, struct, sys
from pathlib import Path

LOAD = 0x80010000
ZERO, AT, V0, A0, T0, T1, T2, T3, S0, SP, RA = 0, 1, 2, 4, 8, 9, 10, 11, 16, 29, 31

ZR_A_INDEX = 64
ZR_A_WORDS = 2000
ZR_MID_INDEX = ZR_A_INDEX + 1000
ZR_B_INDEX = 2100
ZR_B_WORDS = 1500
ZR_B_TARGET_INDEX = ZR_B_INDEX + 700
LOOP_INDEX = 7
COUNTER = 0x80020000


def at(i):
    return LOAD + 4 * i


def addiu(rt, rs, imm): return 0x24000000 | rs << 21 | rt << 16 | (imm & 0xFFFF)
def ori(rt, rs, imm):   return 0x34000000 | rs << 21 | rt << 16 | (imm & 0xFFFF)
def lui(rt, imm):       return 0x3C000000 | rt << 16 | (imm & 0xFFFF)
def lw(rt, off, base):  return 0x8C000000 | base << 21 | rt << 16 | (off & 0xFFFF)
def sw(rt, off, base):  return 0xAC000000 | base << 21 | rt << 16 | (off & 0xFFFF)
def jal(target):        return 0x0C000000 | (target >> 2) & 0x03FFFFFF
def j(target):          return 0x08000000 | (target >> 2) & 0x03FFFFFF
def bgtz(rs, pc_index, target_index):
    return 0x1C000000 | rs << 21 | ((target_index - (pc_index + 1)) & 0xFFFF)
SYSCALL = 0x0000000C
JR_RA = 0x03E00008
NOP = 0


def program():
    words = [0] * 3605
    code = {
        0: addiu(A0, ZERO, 2), 1: SYSCALL,            # ExitCriticalSection
        2: lui(T0, 0x1F80), 3: lw(T1, 0x1074, T0),    # I_MASK |= VBLANK
        4: ori(T1, T1, 1), 5: sw(T1, 0x1074, T0),
        6: lui(S0, COUNTER >> 16),
        LOOP_INDEX: jal(at(ZR_A_INDEX)), 8: NOP,
        9: jal(at(ZR_MID_INDEX)), 10: NOP,
        11: addiu(T3, ZERO, 3),
        12: jal(at(ZR_B_INDEX)), 13: NOP,
        14: lw(T2, 0, S0), 15: addiu(T2, T2, 1),
        16: j(at(LOOP_INDEX)), 17: sw(T2, 0, S0),
        ZR_A_INDEX + ZR_A_WORDS: JR_RA, ZR_A_INDEX + ZR_A_WORDS + 1: NOP,
        ZR_B_INDEX + ZR_B_WORDS: addiu(T3, T3, -1),
        ZR_B_INDEX + ZR_B_WORDS + 1: bgtz(T3, ZR_B_INDEX + ZR_B_WORDS + 1, ZR_B_TARGET_INDEX),
        ZR_B_INDEX + ZR_B_WORDS + 2: NOP,
        ZR_B_INDEX + ZR_B_WORDS + 3: JR_RA, ZR_B_INDEX + ZR_B_WORDS + 4: NOP,
    }
    for i, w in code.items():
        words[i] = w
    data = b"".join(struct.pack("<I", w) for w in words)
    return data + b"\0" * (-len(data) % 2048)


def psx_exe(data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, LOAD)          # pc0
    struct.pack_into("<I", h, 0x18, LOAD)          # t_addr
    struct.pack_into("<I", h, 0x1C, len(data))     # t_size
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)    # s_addr
    marker = b"Sony Computer Entertainment Inc. for North America area"
    h[0x4C:0x4C + len(marker)] = marker
    return bytes(h) + data


def both16(v): return struct.pack("<H", v) + struct.pack(">H", v)
def both32(v): return struct.pack("<I", v) + struct.pack(">I", v)


def dir_record(name, lba, size, is_dir):
    date = bytes([126, 9, 17, 0, 0, 0, 0])
    body = (both32(lba) + both32(size) + date + bytes([2 if is_dir else 0, 0, 0])
            + both16(1) + bytes([len(name)]) + name)
    rec = bytes([0, 0]) + body
    rec = bytes([len(rec) + (len(rec) % 2)]) + rec[1:]
    return rec + b"\0" * (len(rec) % 2)


def iso(license_sectors, files):
    """files: [(name, bytes)] -> cooked 2048-byte ISO9660 image."""
    sectors = {}
    lba = 21
    placed = []
    for name, blob in files:
        n = max(1, -(-len(blob) // 2048))
        placed.append((name, lba, blob))
        for k in range(n):
            sectors[lba + k] = blob[k * 2048:(k + 1) * 2048].ljust(2048, b"\0")
        lba += n
    total = lba
    root = dir_record(b"\0", 20, 2048, True) + dir_record(b"\1", 20, 2048, True)
    for name, flba, blob in placed:
        root += dir_record(name + b";1", flba, len(blob), False)
    sectors[20] = root.ljust(2048, b"\0")
    pt_le = bytes([1, 0]) + struct.pack("<I", 20) + struct.pack("<H", 1) + b"\0\0"
    pt_be = bytes([1, 0]) + struct.pack(">I", 20) + struct.pack(">H", 1) + b"\0\0"
    sectors[18] = pt_le.ljust(2048, b"\0")
    sectors[19] = pt_be.ljust(2048, b"\0")
    pvd = bytearray(2048)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[40:72] = b"ZERO_RUN".ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(2048)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack("<I", 18)
    pvd[148:152] = struct.pack(">I", 19)
    pvd[156:190] = dir_record(b"\0", 20, 2048, True)
    pvd[881] = 1
    sectors[16] = bytes(pvd)
    sectors[17] = b"\xFFCD001\x01".ljust(2048, b"\0")
    for i in range(16):
        sectors[i] = license_sectors[i]
    return b"".join(sectors.get(i, b"\0" * 2048) for i in range(total))


def license_area(bin_path):
    raw = open(bin_path, "rb").read(16 * 2352)
    if len(raw) != 16 * 2352 or raw[0:12] != b"\x00" + b"\xFF" * 10 + b"\x00":
        raise SystemExit("--license-from must be a raw 2352-byte data track image")
    return [raw[i * 2352 + 24:i * 2352 + 24 + 2048] for i in range(16)]


GAME_TOML = """[game]
name = "Zero-run fixture"
id = "ZERORUN-00000"
players = 1
exe = "disc/TEST.EXE"
discs = ["disc/ZERORUN.iso"]
load_address = "0x{load:08X}"
entry_pc = "0x{load:08X}"
text_size = "0x{size:08X}"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
bios_config = "{root}/bios/SCPH1001.toml"
strict = true

[runtime]
window_title = "Zero-run fixture"
memcard_dir = "saves"
"""

CMAKE = """cmake_minimum_required(VERSION 3.20)
set(PSXRECOMP_ROOT "{root}" CACHE PATH "Path to psxrecomp")
project(ZeroRunFixture LANGUAGES C CXX)
include("${{PSXRECOMP_ROOT}}/runtime/runtime.cmake")
psxrecomp_add_game_runtime(psx-cosim COSIM
    PRELOADED_MODS_DIR NONE
    WINDOW_TITLE "Zero-run cosim"
    EXE_NAME "psx-cosim"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 1
    GEN_MARKER "generated/TEST.EXE_dispatch.c"
    GEN_FULL_GLOB "generated/TEST.EXE_full_*.c"
)
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--framework-root", type=Path, required=True)
    ap.add_argument("--license-from", type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"refusing occupied output directory: {out}")
    (out / "disc").mkdir(parents=True, exist_ok=True)
    data = program()
    exe = psx_exe(data)
    (out / "disc" / "TEST.EXE").write_bytes(exe)
    cnf = b"BOOT = cdrom:\\TEST.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n"
    (out / "disc" / "ZERORUN.iso").write_bytes(
        iso(license_area(args.license_from), [(b"SYSTEM.CNF", cnf), (b"TEST.EXE", exe)]))
    seeds = [at(0), at(ZR_A_INDEX), at(ZR_MID_INDEX), at(ZR_B_INDEX)]
    (out / "seeds.txt").write_text("".join("0x%08X\n" % s for s in seeds))
    (out / "game.toml").write_text(GAME_TOML.format(load=LOAD, size=len(data),
                         root=args.framework_root.resolve().as_posix()))
    (out / "CMakeLists.txt").write_text(
        CMAKE.format(root=args.framework_root.resolve().as_posix()))
    print(f"fixture written to {out}")
    print(f"  loop counter 0x{COUNTER:08X}; runs A 0x{at(ZR_A_INDEX):08X} "
          f"(mid 0x{at(ZR_MID_INDEX):08X}), B 0x{at(ZR_B_INDEX):08X} "
          f"(label 0x{at(ZR_B_TARGET_INDEX):08X})")


if __name__ == "__main__":
    main()
