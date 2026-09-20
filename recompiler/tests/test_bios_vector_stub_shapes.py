#!/usr/bin/env python3
"""The emitted A0/B0/C0 native call-stub guard must accept both stub shapes.

Retail kernels install `lui $t0,hi; addiu $t0,$t0,lo; jr $t0; nop` at the
call vectors; OpenBIOS installs `addiu $t0,$zero,lo; jr $t0; nop; nop`. A
guard that knows only the first shape fails closed on OpenBIOS and every
kernel call then pays a dirty-RAM interpreter entry (Breath of Fire III:
~230 per frame in the field, ~460 interpreted instructions per frame).
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    default = os.path.join(root, "recompiler", "build", "psxrecomp-bios.exe")

    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=default)
    args = parser.parse_args()

    if not os.path.isfile(args.recompiler):
        print(f"FAIL: recompiler not found: {args.recompiler}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as out_dir:
        result = subprocess.run(
            [args.recompiler, "--config",
             os.path.join(root, "bios", "OpenBIOS.toml"),
             "--out-dir", out_dir],
            cwd=root, capture_output=True, text=True,
        )
        if result.returncode:
            print(result.stderr or result.stdout, file=sys.stderr)
            return 1
        with open(os.path.join(out_dir, "OpenBIOS_dispatch.c"),
                  encoding="utf-8") as stream:
            generated = stream.read()

    m = re.search(r"static int psx_bios_try_native_call_stub\(.*?\n\}\n",
                  generated, re.S)
    if not m:
        print("FAIL: no psx_bios_try_native_call_stub in the dispatch",
              file=sys.stderr)
        return 1
    body = m.group(0)

    checks = {
        "shape A guard (lui $t0)":
            "(w0 & 0xFFFF0000u) == 0x3C080000u" in body,
        "shape A guard (addiu $t0,$t0)":
            "(w1 & 0xFFFF0000u) == 0x25080000u" in body,
        "shape B guard (addiu $t0,$zero)":
            "(w0 & 0xFFFF0000u) == 0x24080000u" in body,
        "shape B guard (jr $t0 in word 1)":
            "w1 == 0x01000008u && w2 == 0u && w3 == 0u" in body,
        "shape B target is sign-extended word-0 immediate":
            "target = (uint32_t)(int32_t)(int16_t)(w0 & 0xFFFFu);" in body,
        "unrecognised words fail closed":
            "return 0;" in body,
        "native tail transfer publishes $t0 and pc":
            "cpu->gpr[8] = target;" in body and "cpu->pc = target;" in body,
    }
    failed = [k for k, ok in checks.items() if not ok]
    for k in failed:
        print(f"FAIL: {k}", file=sys.stderr)
    if failed:
        return 1
    print("PASS: native call-stub guard accepts the retail and OpenBIOS shapes.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
