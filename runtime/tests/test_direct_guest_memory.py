"""Exercise native data accesses against production memory/observer code."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

import test_memory_debug_trace_guard as memory_fixture


def remove_function(source, name):
    match = re.search(r"\n(?:void|int) " + name + r"\([^;]*?\)\s*\{", source, re.S)
    assert match, name
    a = match.start()
    brace = source.index("{", a)
    depth = 1
    b = brace + 1
    while depth:
        depth += (source[b] == "{") - (source[b] == "}")
        b += 1
    return source[:a] + "\n" + source[b:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=memory_fixture.default_cc())
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    # Reuse the existing production-memory fixture. Link the real parity and
    # card modules rather than replacing their arming/provenance semantics.
    stubs = remove_function(memory_fixture.STUBS_C, "parity_trace_note_write")
    stubs = remove_function(stubs, "card_data_writes_check")
    stubs = stubs.replace("void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) {\n"
                          "    (void)biu; (void)addr; (void)value; abort();\n}",
                          "unsigned g_fixture_isc;\n"
                          "void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) {\n"
                          "    (void)biu; (void)addr; (void)value; ++g_fixture_isc;\n}")
    assert "unsigned g_fixture_isc" in stubs
    with tempfile.TemporaryDirectory(prefix="psx-direct-memory-") as td:
        work = Path(td)
        (work / "harness_common.h").write_text(memory_fixture.HARNESS_COMMON_H, encoding="utf-8")
        (work / "stubs.c").write_text(stubs, encoding="utf-8")
        (work / "trace.c").write_text(memory_fixture.TRACE_COUNTING_C, encoding="utf-8")
        for direct, production in ((0, True), (1, True), (1, False)):
            for opt in ("-O0", "-O2"):
                exe = work / ("fixture.exe" if os.name == "nt" else "fixture")
                argv = [args.cc, "-std=c11", opt, "-ffunction-sections", "-fdata-sections",
                        "-DPSX_NATIVE_CPU_SPANS=1", "-DPSX_DIRECT_GUEST_MEMORY=" + str(direct),
                        "-I", str(work), "-I", str(root / "runtime/include")]
                if production:
                    argv += ["-DPSX_NO_DEBUG_TOOLS=1"]
                argv += [str(root / "runtime/tests/test_direct_guest_memory.c")]
                argv += [str(root / "runtime/src" / name) for name in
                         ("memory.c", "psx_ram_geometry.c", "kernel_patch_ranges.c",
                          "parity_trace.c", "card_data_writes.c")]
                argv += [str(work / "stubs.c"), str(work / "trace.c"),
                         "-Wl,--gc-sections", "-o", str(exe)]
                subprocess.run(argv, check=True)
                subprocess.run([str(exe)], check=True)
    print("PASS: actual canonical/native data access and dynamic observers (O0/O2, production/debug)")


if __name__ == "__main__":
    main()
