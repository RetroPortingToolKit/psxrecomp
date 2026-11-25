"""Unaligned accesses at region ends against production memory.c (O0/O2).

Builds test_memory_unaligned_access.c with the shared memory.c fixture seams
(test_memory_debug_trace_guard.py) and the real render-pass store policy. The
build uses AddressSanitizer when the toolchain can link it, so a read or write
past a host array fails even where the stray bytes happen to match.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import test_memory_debug_trace_guard as memory_fixture
from test_direct_guest_memory import remove_function

MODULES = ("memory.c", "psx_ram_geometry.c", "kernel_patch_ranges.c",
           "text_source_guard.c", "crc32.c", "render_pass_plan.c")


def sanitizer_flags(cc, work):
    """-fsanitize=address when this toolchain compiles and links it."""
    probe = work / "asan_probe.c"
    probe.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    flags = ["-fsanitize=address", "-fno-omit-frame-pointer"]
    result = subprocess.run([cc, *flags, str(probe), "-o", str(work / "asan_probe")],
                            capture_output=True, text=True)
    return flags if result.returncode == 0 else []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=memory_fixture.default_cc())
    args = parser.parse_args()
    cc = memory_fixture.normalize_cc(args.cc)
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="psx-memory-unaligned-") as td:
        work = Path(td)
        memory_fixture.write_fixture(work, memory_fixture.MEMORY_C)
        # The real policy is linked; its fixture placeholder would abort. The
        # read watch is recorded by the fixture itself.
        stubs = (work / "stubs.c").read_text(encoding="utf-8")
        stubs = remove_function(stubs, "render_pass_store_to")
        stubs = remove_function(stubs, "debug_server_trace_ram_read_watch")
        (work / "stubs.c").write_text(stubs, encoding="utf-8")
        sanitize = sanitizer_flags(cc, work)
        print("AddressSanitizer:", "on" if sanitize else "not available", flush=True)
        for opt in ("-O0", "-O2"):
            exe = work / ("fixture" + opt + (".exe" if os.name == "nt" else ""))
            cmd = [cc, "-std=c11", opt, *sanitize, "-fno-lto",
                   "-ffunction-sections", "-fdata-sections",
                   "-I", str(work), "-I", str(root / "runtime/include"),
                   str(root / "runtime/tests/test_memory_unaligned_access.c")]
            cmd += [str(root / "runtime/src" / name) for name in MODULES]
            cmd += [str(work / "stubs.c"), str(work / "trace_empty.c"),
                    "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                    "-o", str(exe)]
            subprocess.run(cmd, check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
            subprocess.run([str(exe), str(work / "bios.bin")], check=True, env=env)
            print("PASS: unaligned region-end accesses", opt, flush=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        print(f"FAIL: command exited {exc.returncode}: {' '.join(map(str, exc.cmd))}")
        sys.exit(1)
