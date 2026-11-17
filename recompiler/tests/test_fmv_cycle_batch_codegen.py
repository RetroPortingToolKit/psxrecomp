#!/usr/bin/env python3
"""Verify generated FMV cycle batching has portable scope and IRQ flushes."""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


LOAD = 0x80010000


def generate_source(recompiler):
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD)
    struct.pack_into("<I", header, 0x18, LOAD)
    words = (0x24020001, 0x1440FFFE, 0x00000000, 0x03E00008, 0x00000000)
    body = b"".join(struct.pack("<I", word) for word in words)
    struct.pack_into("<I", header, 0x1C, len(body))

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "cycle.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(exe, "wb") as stream:
            stream.write(header + body)
        with open(seeds, "w", encoding="utf-8") as stream:
            stream.write(f"0x{LOAD:08X}\n")
        result = subprocess.run(
            [recompiler, exe, "--seeds", seeds, "--out-dir", out],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        full = sorted(
            name
            for name in os.listdir(out)
            if "_full" in name and name.endswith(".c") and "_dispatch" not in name
        )
        if not full:
            raise RuntimeError("no generated _full*.c source found")
        sources = []
        for name in full:
            with open(os.path.join(out, name), encoding="utf-8") as stream:
                sources.append(stream.read())
        return "\n".join(sources)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True)
    args = parser.parse_args()
    try:
        source = generate_source(args.recompiler)
    except (OSError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    guard = (
        "#if defined(PSX_ENABLE_BLOCK_CYCLES) && "
        "(defined(__GNUC__) || defined(__clang__))\n"
    )
    guarded_begin = (
        guard
        + "    __attribute__((cleanup(psx_cyc_bb_defer_cleanup))) "
        "int _psx_cyc_bb_guard = 1;\n"
        "    psx_cyc_bb_defer_begin();\n"
        "#endif\n"
    )
    failures = []
    if guarded_begin not in source:
        failures.append("basic-block defer is not wholly GCC/Clang guarded")
    if "PSX_BLOCK_EDGE(cpu," not in source:
        failures.append("generated interrupt edges do not use the publishing boundary")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: generated FMV cycle batching is scoped and flushed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
