#!/usr/bin/env python3
"""Compile the overlay callback shim against the public runtime headers."""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import ctypes


ROOT = pathlib.Path(__file__).resolve().parents[2]
INCLUDE = ROOT / "runtime" / "include"


def find_gcc() -> str:
    candidates = [
        os.environ.get("CC"),
        shutil.which("gcc"),
        shutil.which("cc"),
        r"C:\msys64\mingw64\bin\gcc.exe" if os.name == "nt" else None,
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return candidate
    raise SystemExit("gcc/cc is required for the overlay shim compile test")


def main() -> int:
    source = r'''#include "cpu_state.h"
#include "overlay_dispatch_preamble.c.inc"

/* As CodeGenerator::emit_runtime_externs declares them in every shard. */
extern int  psx_ws_cull_bgez(uint32_t v);
extern uint32_t psx_ws_clip_edge_x(uint32_t v, uint32_t w);
extern uint32_t psx_ws_cull_lui_hi(uint32_t imm);
extern int  psx_ws_cull_bltz_hi(uint32_t v);
extern int  psx_ws_cull_bgez_hi(uint32_t v);

void func_80010000(CPUState *cpu) {
    psx_cyc_bb_defer_begin();
    psx_cyc_charge(1u);
    psx_advance_cycles(1u);
    cpu->gpr[2] = psx_cyc_load_word(cpu, cpu->gpr[4], 2u, 0u);
    cpu->gpr[3] = psx_cyc_load_half(cpu, cpu->gpr[5], 3u, 0u);
    cpu->gpr[6] = psx_ws_cull_slti_lower(cpu->gpr[7], 0xff00u);
    /* [widescreen.cull] bgez_sites / clip_edge_x_load_sites: the emitter
     * writes both in overlay code, so the shim must define them. */
    cpu->gpr[8] = (uint32_t)psx_ws_cull_bgez(cpu->gpr[9]);
    cpu->gpr[10] = psx_ws_clip_edge_x(cpu->gpr[11], 0x140u);
    /* [widescreen.cull] screen_x_sites on lui / bltz / bgez (a screen X in the
     * high half of a register): emitted in overlay code too. */
    cpu->gpr[22] = psx_ws_cull_lui_hi(0x0200u);
    if (psx_ws_cull_bltz_hi(cpu->gpr[8])) cpu->gpr[12] = 1u;
    if (psx_ws_cull_bgez_hi(cpu->gpr[8])) cpu->gpr[13] = 1u;
    psx_cyc_bb_defer_flush();
    psx_cyc_bb_defer_end();
    if (psx_slice_block(cpu, 0x80010000u, 1u, 0)) return;
    debug_server_log_call_entry(0x80010000u);
}

static unsigned load_calls, begin_calls, end_calls, charged;
static CPUState *load_cpu;
static void fixture_charge(uint32_t cycles) { charged += cycles; }
static void fixture_stage(CPUState *cpu, uint32_t rt, uint32_t value, uint32_t pc) {
    if (cpu == load_cpu && rt == 8u && value == 0x2222u && pc == 0x80010008u)
        ++load_calls;
}
static uint64_t fixture_begin(CPUState *cpu, uint32_t pc, uint32_t instruction) {
    if (cpu == load_cpu && pc == 0x80010008u && instruction == 0x48880000u)
        ++begin_calls;
    return UINT64_C(0x1234567801);
}
static void fixture_end(CPUState *cpu, uint64_t cookie, uint32_t instruction) {
    if (cpu == load_cpu && cookie == UINT64_C(0x1234567801) && instruction == 0x48880000u)
        ++end_calls;
}
PSX_OVERLAY_EXPORT int fixture_load_window(void) {
    CPUState cpu = {0};
    OverlayCallbacks callbacks = {0};
    callbacks.cpu_charge = fixture_charge;
    callbacks.ld_delay_stage = fixture_stage;
    callbacks.ld_delay_begin = fixture_begin;
    callbacks.ld_delay_end = fixture_end;
    load_cpu = &cpu;
    overlay_init(&callbacks);
    psx_cpu_charge(17u);
    dirty_ram_ld_delay_stage(&cpu, 8u, 0x2222u, 0x80010008u);
    uint64_t cookie = dirty_ram_ld_delay_begin(&cpu, 0x80010008u, 0x48880000u);
    dirty_ram_ld_delay_end(&cpu, cookie, 0x48880000u);
    if (charged || load_calls != 1u || begin_calls != 1u || end_calls != 1u) return 1;
    overlay_flush_cycles();
    return charged == 17u ? 0 : 2;
}
'''
    gcc = find_gcc()
    with tempfile.TemporaryDirectory() as temp_dir:
        temp = pathlib.Path(temp_dir)
        c_path = temp / "overlay_shim_contract.c"
        out_path = temp / ("overlay_shim_contract.dll" if os.name == "nt"
                           else "overlay_shim_contract.so")
        c_path.write_text(source, encoding="ascii")
        command = [
            gcc, "-shared", "-O2",
            "-DPSX_OVERLAY_DLL_BUILD",
            "-DPSX_NO_DEBUG_TOOLS",
            "-DPSX_ENABLE_BLOCK_CYCLES=1",
            "-DPSX_OVERLAY_FLAVOR=0",
            "-Werror=implicit-function-declaration",
        ]
        if os.name != "nt":
            command.append("-fPIC")
        command.extend([
            str(c_path), "-o", str(out_path), f"-I{INCLUDE}", "-lm",
        ])
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            return result.returncode
        if not out_path.is_file():
            raise AssertionError("overlay compiler reported success without output")
        library = ctypes.CDLL(str(out_path))
        library.fixture_load_window.restype = ctypes.c_int
        if library.fixture_load_window() != 0:
            raise AssertionError("load window forwarding lost its host owner or flushed clocks early")
        # Release the Windows module before TemporaryDirectory cleanup.
        if os.name == "nt":
            import _ctypes
            _ctypes.FreeLibrary(library._handle)
        del library
    print("PASS: overlay shim compiles and forwards the load window before clock observation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
