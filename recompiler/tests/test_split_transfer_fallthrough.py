#!/usr/bin/env python3
"""Issue #812: split CPS exits must not acquire dead calls or false warnings."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        # Synthetic layout from the report: an explicitly split conditional
        # branch, then unreachable jump-shaped data after a BIOS thunk.
        words = [0] * 33
        pieces = {
            0x00: [0x0C004008, 0, 0x0C004018, 0, 0x03E00008, 0],
            0x20: [0x14800005, 0],
            0x28: [0x27BDFFF8, 0xAFBF0004, 0x03E00008, 0x27BD0008],
            0x38: [0x03E00008, 0],
            0x60: [0x240A00B0, 0x01400008, 0x24090038, 0, 0x0A017350, 0x1036D11A],
            0x78: [0x240A00B0, 0x01400008, 0x2409003F],
        }
        for offset, piece in pieces.items():
            words[offset // 4:offset // 4 + len(piece)] = piece
        header = bytearray(2048)
        header[:8] = b"PS-X EXE"
        for offset, value in ((0x10, 0x80010000), (0x18, 0x80010000),
                              (0x1C, len(words) * 4), (0x30, 0x801FFFF0)):
            struct.pack_into("<I", header, offset, value)
        exe = work / "split.exe"
        exe.write_bytes(header + struct.pack("<33I", *words))
        seeds = work / "seeds.txt"
        seeds.write_text("".join(f"0x{0x80010000 + offset:08X}\n"
                                 for offset in (0, 0x20, 0x28, 0x38, 0x60, 0x78)))
        for cps in ("1", "0"):
            output = work / ("out" + cps)
            result = subprocess.run([args.recompiler, str(exe), "--seeds", str(seeds),
                                     "--out-dir", str(output), "--strict",
                                     "--project-root", str(ROOT)], cwd=ROOT,
                                    env=dict(os.environ, PSX_CPS=cps), capture_output=True,
                                    text=True, encoding="utf-8", errors="replace")
            assert result.returncode == 0, result.stdout + result.stderr
            assert "emitting fallthrough" not in result.stdout + result.stderr
            code = "\n".join(p.read_text(encoding="utf-8")
                             for p in output.glob("split.exe_full*.c"))
            assert code, result.stdout
            for address in ("80010028", "80010078"):
                assert f"func_{address}(cpu);  /* fallthrough to next function */" not in code
            transfers = ("CPS taken: split", "CPS not taken: split", "CPS j: split") if cps == "1" else (
                "taken: split piece", "not taken: split piece", "j to split (mid-func)")
            for transfer in transfers:
                assert transfer in code, transfer
            print(f"PASS: CPS={cps}, explicit transfers retained, dead fallthroughs/warnings absent")


if __name__ == "__main__":
    main()
