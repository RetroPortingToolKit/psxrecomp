"""Static callback naming and chained address constants, without symbol seeds."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analyzer", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        image = bytearray(2048 + 0x1000)
        image[:8] = b"PS-X EXE"
        struct.pack_into("<IIII", image, 16, 0x80010000, 0, 0x80010000, 0x1000)
        words = [
            0x3C088001, 0x25080600, 0x8D020004,  # lui/addiu/lw through updated t0
            0x3C098001, 0x35240700, 0x8C830008,  # cross-register ori; load through a0
            0x35080010, 0x8D0A0000,              # ori through the full existing address
            0x00A64021, 0x8D090000,              # unknown add clobbers the constant
            0x03E00008, 0,
        ]
        for i, word in enumerate(words):
            struct.pack_into("<I", image, 2048 + i * 4, word)
        struct.pack_into("<II", image, 2048 + 0x700, 0x03E00008, 0)
        exe = root / "input.exe"
        exe.write_bytes(image)
        subprocess.run([args.analyzer, str(exe), "--out", str(root / "out"),
                        "--quiet"], check=True, capture_output=True)
        refs = json.loads((root / "out/refs.json").read_text())["refs"]
        loads = {r["pc"]: r["target"] for r in refs if r["width"] == 4}
        assert loads[0x80010008] == 0x80010604, loads
        assert loads[0x80010014] == 0x80010708, loads
        assert loads[0x8001001C] == 0x80010610, loads
        assert 0x80010024 not in loads, loads
        funcs = json.loads((root / "out/analysis.json").read_text())["functions"]
        callback = next(f for f in funcs if f["addr"] == 0x80010700)
        assert callback["address_taken"], callback
        assert callback["reachable"], callback
    print("PASS: full address propagation and ORI callback references")


if __name__ == "__main__":
    main()
