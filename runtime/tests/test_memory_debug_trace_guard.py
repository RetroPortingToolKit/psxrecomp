#!/usr/bin/env python3
"""Compile actual memory.c to validate production debug-trace guards."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
MEMORY_C = ROOT / "runtime" / "src" / "memory.c"
TRACE_CALL = "debug_server_trace_write_check"
DEFAULT_GCC = pathlib.Path(r"C:\msys64\mingw64\bin\gcc.exe")

HARNESS_COMMON_H = r"""
#ifndef PSX_MEMORY_TRACE_HARNESS_COMMON_H
#define PSX_MEMORY_TRACE_HARNESS_COMMON_H
#include <stdint.h>
#include <stddef.h>
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "psx_bios_image.h"

extern int g_ls_mode;
extern int g_ls_suppress_record;
extern volatile int g_ds_recording;
extern int g_dma_exec_depth;
extern int g_dma_cur_ch;
extern uint32_t g_dma_cur_madr;
extern uint32_t g_dma_cur_bcr;
extern uint32_t g_dma_initiator_pc;
extern uint32_t g_debug_last_store_pc;
extern uint32_t g_debug_current_func_addr;
extern uint64_t s_frame_count;
extern uint32_t g_overlay_region_floor;
extern uint32_t g_text_image_lo;
extern int g_ram_read_watch_active;
extern uint64_t psx_cycle_count;
extern uint64_t g_psx_cycle_fast_limit;
extern int g_event_step_conservative;
extern int g_ls_replay_active;
extern CPUState *debug_cpu_ptr;
extern void (*g_overlay_flush_pending_cycles)(void);
extern uint32_t g_dirty_ram_exec_page_bitmap[16];
extern uint32_t g_dirty_ram_exec_pc_bitmap[16384];
extern uint32_t g_dirty_ram_dispatch_pc_bitmap[16384];

extern unsigned g_trace_write_calls;
extern unsigned g_parity_write_calls;
extern unsigned g_card_write_calls;
extern uint32_t g_trace_old_accum;
extern uint32_t g_trace_new_accum;

int fntrace_is_game_started(void);
int psx_get_in_exception(void);
uint32_t ls_read_hook(uint32_t addr, uint8_t width, uint32_t value);
void ls_write_hook(uint32_t addr, uint8_t width, uint32_t value);
void ds_note_read(uint32_t addr, uint8_t width);
void ds_note_write(uint32_t addr, uint8_t width);
void ds_note_dma_write(void);
void psx_devices_mmio_sync(void);
void psx_fatal_halt(const char *reason);
void psx_irq_refresh_cause_ip2(void);
int sio_card_should_hold_imask_bit7(void);
void sio_card_handoff_on_imask(uint32_t old_mask, uint32_t new_mask);
uint32_t sio_read(uint32_t addr);
void sio_write(uint32_t addr, uint32_t val);
void sio_tick(uint32_t cycles);
uint32_t dma_read(uint32_t addr);
void dma_write(uint32_t addr, uint32_t val);
void dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask);
uint32_t timers_read(uint32_t addr);
void timers_write(uint32_t addr, uint32_t val);
uint32_t cdrom_read(uint32_t addr);
void cdrom_write(uint32_t addr, uint32_t val);
uint32_t gpu_read_gpuread(void);
uint32_t gpu_read_gpustat(void);
void gpu_set_gp0_source(uint32_t source);
void gpu_write_gp0(uint32_t val);
void gpu_write_gp1(uint32_t val);
uint32_t mdec_read(uint32_t addr);
void mdec_write(uint32_t addr, uint32_t val);
uint16_t spu_read(uint32_t addr);
void spu_write(uint32_t addr, uint16_t val);
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width);
void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width);
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val);
void debug_server_trace_entryint_write(uint32_t phys, uint32_t old_val,
                                       uint32_t new_val, uint8_t width);
void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc);
int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);
void overlay_loader_note_code_write(void);
void overlay_loader_active_write_check(uint32_t phys, uint32_t size);
void overlay_loader_resync_validation_after_restore(void);

#endif
"""

STUBS_C = r"""
#include "harness_common.h"
#include <stdio.h>
#include <stdlib.h>

static const PsxKernelBody s_psx_bios_kernel_bodies[1] = {{0, 0, 0}};
const PsxKernelBody *psx_bios_kernel_bodies = s_psx_bios_kernel_bodies;
uint32_t psx_bios_kernel_body_count = 0;
PsxBiosImageInfo psx_bios_image = {0};

int g_ls_mode = 0;
int g_ls_suppress_record = 0;
volatile int g_ds_recording = 0;
int g_dma_exec_depth = 0;
int g_dma_cur_ch = -1;
uint32_t g_dma_cur_madr = 0;
uint32_t g_dma_cur_bcr = 0;
uint32_t g_dma_initiator_pc = 0;
uint32_t g_debug_last_store_pc = 0;
uint32_t g_debug_current_func_addr = 0;
uint64_t s_frame_count = 0;
uint32_t g_overlay_region_floor = 0x200000u;
uint32_t g_text_image_lo = 0x10000u;
int g_ram_read_watch_active = 0;
uint64_t psx_cycle_count = 0;
uint64_t g_psx_cycle_fast_limit = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
CPUState *debug_cpu_ptr = 0;
void (*g_overlay_flush_pending_cycles)(void) = 0;
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS] = {0};
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS] = {0};
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS] = {0};

unsigned g_trace_write_calls = 0;
unsigned g_parity_write_calls = 0;
unsigned g_card_write_calls = 0;
uint32_t g_trace_old_accum = 0;
uint32_t g_trace_new_accum = 0;

int fntrace_is_game_started(void) { return 0; }
int psx_get_in_exception(void) { return 0; }
uint32_t ls_read_hook(uint32_t addr, uint8_t width, uint32_t value) {
    (void)addr; (void)width; return value;
}
void ls_write_hook(uint32_t addr, uint8_t width, uint32_t value) {
    (void)addr; (void)width; (void)value;
}
void ds_note_read(uint32_t addr, uint8_t width) { (void)addr; (void)width; }
void ds_note_write(uint32_t addr, uint8_t width) { (void)addr; (void)width; }
void ds_note_dma_write(void) {}
void psx_devices_mmio_sync(void) {}
void psx_fatal_halt(const char *reason) { fprintf(stderr, "%s\n", reason); abort(); }
void psx_irq_refresh_cause_ip2(void) {}
int sio_card_should_hold_imask_bit7(void) { return 0; }
void sio_card_handoff_on_imask(uint32_t old_mask, uint32_t new_mask) {
    (void)old_mask; (void)new_mask;
}
uint32_t sio_read(uint32_t addr) { (void)addr; return 0; }
void sio_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; }
void sio_tick(uint32_t cycles) { (void)cycles; }
uint32_t dma_read(uint32_t addr) { (void)addr; return 0; }
void dma_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; }
void dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask) {
    (void)addr; (void)val; (void)mask;
}
uint32_t timers_read(uint32_t addr) { (void)addr; return 0; }
void timers_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; }
uint32_t cdrom_read(uint32_t addr) { (void)addr; return 0; }
void cdrom_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; }
uint32_t gpu_read_gpuread(void) { return 0; }
uint32_t gpu_read_gpustat(void) { return 0; }
void gpu_set_gp0_source(uint32_t source) { (void)source; }
void gpu_write_gp0(uint32_t val) { (void)val; }
void gpu_write_gp1(uint32_t val) { (void)val; }
uint32_t mdec_read(uint32_t addr) { (void)addr; return 0; }
void mdec_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; }
uint16_t spu_read(uint32_t addr) { (void)addr; return 0; }
void spu_write(uint32_t addr, uint16_t val) { (void)addr; (void)val; }
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr; (void)val; (void)width;
}
void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width) {
    (void)addr; (void)val; (void)width;
}
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t val) {
    (void)phys; (void)val;
}
void debug_server_trace_entryint_write(uint32_t phys, uint32_t old_val,
                                       uint32_t new_val, uint8_t width) {
    (void)phys; (void)old_val; (void)new_val; (void)width;
}
void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc) {
    (void)addr; (void)width; (void)writer_pc; g_parity_write_calls++;
}
int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width) {
    (void)phys; (void)value; (void)width; g_card_write_calls++; return 0;
}
void overlay_loader_note_code_write(void) {}
void overlay_loader_active_write_check(uint32_t phys, uint32_t size) {
    (void)phys; (void)size;
}
void overlay_loader_resync_validation_after_restore(void) {}
"""

TRACE_COUNTING_C = r"""
#include "harness_common.h"
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width) {
    (void)phys;
    g_trace_write_calls++;
    g_trace_old_accum ^= old_val + (uint32_t)width;
    g_trace_new_accum ^= new_val + ((uint32_t)width << 24);
}
"""

TRACE_EMPTY_C = r"""
#include "harness_common.h"
void debug_server_trace_write_check(uint32_t phys, uint32_t old_val,
                                    uint32_t new_val, uint8_t width) {
    (void)phys; (void)old_val; (void)new_val; (void)width;
}
"""

DRIVER_C = r"""
#include "harness_common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *memory_get_ram_ptr(void);
uint8_t *memory_get_scratchpad_ptr(void);
void psx_write_word(uint32_t addr, uint32_t val);
void psx_write_half(uint32_t addr, uint16_t val);
void psx_write_byte(uint32_t addr, uint8_t val);
extern uint64_t g_guest_store_count;

static uint32_t hash_bytes(const uint8_t *bytes, const uint32_t *points,
                           unsigned count, uint32_t h) {
    for (unsigned i = 0; i < count; i++) h = (h ^ bytes[points[i]]) * 16777619u;
    return h;
}

static void seed_memory(void) {
    uint8_t *ram = memory_get_ram_ptr();
    uint8_t *sp = memory_get_scratchpad_ptr();
    ram[0x1000u] = 0x10u; ram[0x1001u] = 0x32u;
    ram[0x1002u] = 0x54u; ram[0x1003u] = 0x76u;
    ram[0x1004u] = 0x98u; ram[0x1005u] = 0xBAu; ram[0x1006u] = 0xDCu;
    sp[0] = 0x11u; sp[1] = 0x22u; sp[2] = 0x33u; sp[3] = 0x44u;
    sp[4] = 0x55u; sp[5] = 0x66u; sp[6] = 0x77u;
}

static void do_six_writes(unsigned index) {
    uint32_t mirror = (index & 3u) << 21;
    uint32_t sp_segment = (index & 1u) ? 0x80000000u : 0u;
    psx_write_word(0x80001000u + mirror, 0xA1B2C3D4u);
    psx_write_word(0x1F800000u + sp_segment, 0x10203040u);
    psx_write_half(0xA0001004u + mirror, 0xCAFEu);
    psx_write_half(0x1F800004u + sp_segment, 0xBEEFu);
    psx_write_byte(0x00001006u + mirror, 0x5Au);
    psx_write_byte(0x1F800006u + sp_segment, 0x6Bu);
}

static int check_written_bytes(void) {
    static const uint8_t expected_ram[] = {0xD4, 0xC3, 0xB2, 0xA1, 0xFE, 0xCA, 0x5A, 0};
    static const uint8_t expected_sp[] = {0x40, 0x30, 0x20, 0x10, 0xEF, 0xBE, 0x6B, 0};
    for (unsigned i = 0; i < sizeof(expected_ram); ++i) {
        if (memory_get_ram_ptr()[0x1000u + i] != expected_ram[i] ||
            memory_get_scratchpad_ptr()[i] != expected_sp[i]) return 0;
    }
    return memory_get_ram_ptr()[0xFFFu] == 0;
}

static uint32_t state_hash(void) {
    static const uint32_t ram_points[] = {
        0x1000u, 0x1001u, 0x1002u, 0x1003u, 0x1004u, 0x1005u, 0x1006u,
    };
    static const uint32_t sp_points[] = {
        0x000u, 0x001u, 0x002u, 0x003u, 0x004u, 0x005u, 0x006u,
    };
    uint32_t h = 2166136261u;
    h = hash_bytes(memory_get_ram_ptr(), ram_points, 7u, h);
    h = hash_bytes(memory_get_scratchpad_ptr(), sp_points, 7u, h);
    h ^= (uint32_t)g_guest_store_count * 3u;
    h ^= g_parity_write_calls * 5u;
    h ^= g_card_write_calls * 7u;
    return h;
}

int main(int argc, char **argv) {
    unsigned loops = 1;
    if (argc == 3 && argv[1][0] == '-' && argv[1][1] == '-' &&
        argv[1][2] == 'l') {
        loops = (unsigned)strtoul(argv[2], 0, 0);
        if (loops == 0) loops = 1;
    }
    seed_memory();
    for (unsigned i = 0; i < loops; i++) do_six_writes(i);
    if (!check_written_bytes()) {
        fprintf(stderr, "incorrect RAM/scratchpad bytes or alias folding\n");
        return 1;
    }
    printf("state=%08x trace_calls=%u parity=%u card=%u stores=%llu old=%08x new=%08x\n",
           state_hash(), g_trace_write_calls, g_parity_write_calls,
           g_card_write_calls, (unsigned long long)g_guest_store_count,
           g_trace_old_accum, g_trace_new_accum);
    return 0;
}
"""


def fail(message: str) -> None:
    raise AssertionError(message)


def default_cc() -> str:
    if DEFAULT_GCC.exists():
        return str(DEFAULT_GCC).replace("\\", "/")
    for name in ("cc", "gcc", "clang"):
        path = shutil.which(name)
        if path:
            return path
    return str(DEFAULT_GCC)


def normalize_cc(cc: str) -> str:
    if "\\" in cc and len(cc) >= 3 and cc[1] == ":":
        return cc.replace("\\", "/")
    return cc


def source_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def assert_guarded(memory: str) -> None:
    if memory.count(TRACE_CALL) != 7:
        fail("memory.c should contain one extern plus six trace write call sites")
    extern = memory.find("extern void " + TRACE_CALL)
    if extern < 0:
        fail("missing trace write extern")
    cursor = extern + len("extern void " + TRACE_CALL)
    for index in range(6):
        pos = memory.find(TRACE_CALL + "(", cursor)
        if pos < 0:
            fail(f"missing trace write call site {index + 1}")
        line_start = memory.rfind("\n", 0, pos)
        guard = memory.rfind("#ifndef PSX_NO_DEBUG_TOOLS", 0, pos)
        end_before = memory.rfind("#endif", 0, pos)
        if guard < 0 or guard < end_before:
            fail(f"trace write call site {index + 1} is not production-guarded")
        if line_start - guard > 300:
            fail(f"trace write call site {index + 1} guard is too far away")
        cursor = pos + len(TRACE_CALL)


def assert_observers_preserved(memory: str) -> None:
    required = (
        ("psx_write_word_raw", "static uint16_t psx_read_half_raw", (
            "parity_trace_note_write(phys, 4, effective_store_pc())",
            "card_data_writes_check(phys, val, 4)",
            "dirty_ram_mark_kernel_write(phys)",
            "text_guard_note_write(phys, val, 4)",
            "overlay_watch_note_write(phys, 4)",
            "ram[phys]     = (uint8_t)(val)",
        )),
        ("psx_write_half_raw", "static uint8_t psx_read_byte_raw", (
            "parity_trace_note_write(phys, 2, effective_store_pc())",
            "card_data_writes_check(phys, (uint32_t)val, 2)",
            "dirty_ram_mark_kernel_write(phys)",
            "text_guard_note_write(phys, (uint32_t)val, 2)",
            "overlay_watch_note_write(phys, 2)",
            "ram[phys]     = (uint8_t)(val)",
        )),
        ("psx_write_byte_raw", None, (
            "parity_trace_note_write(phys, 1, effective_store_pc())",
            "card_data_writes_check(phys, (uint32_t)val, 1)",
            "dirty_ram_mark_kernel_write(phys)",
            "text_guard_note_write(phys, (uint32_t)val, 1)",
            "overlay_watch_note_write(phys, 1)",
            "ram[phys] = val",
        )),
    )
    for function, successor, tokens in required:
        start = memory.find(f"static void {function}")
        if start < 0:
            fail(f"missing {function}")
        end = len(memory) if successor is None else memory.find(successor, start + 1)
        if successor is not None and end < 0:
            fail(f"could not bound {function}")
        body = memory[start:end]
        last = -1
        for token in tokens:
            pos = body.find(token)
            if pos < 0:
                fail(f"{function} lost observer/store token: {token}")
            if pos <= last:
                fail(f"{function} observer/store order changed at: {token}")
            last = pos


def write_fixture(work: pathlib.Path, memory_source: pathlib.Path) -> pathlib.Path:
    for name, content in (("harness_common.h", HARNESS_COMMON_H),
                          ("stubs.c", STUBS_C),
                          ("trace_counting.c", TRACE_COUNTING_C),
                          ("trace_empty.c", TRACE_EMPTY_C),
                          ("driver.c", DRIVER_C)):
        (work / name).write_text(content, encoding="utf-8")
    return memory_source.resolve()


def build(cc: str, work: pathlib.Path, memory_source: pathlib.Path, name: str,
          defines: tuple[str, ...], trace_source: str) -> pathlib.Path:
    work = work.resolve()
    memory_copy = write_fixture(work, memory_source)
    exe = work / (name + ".exe")
    cmd = [
        cc,
        "-std=c11",
        "-O2",
        "-fno-lto",
        "-ffunction-sections",
        "-fdata-sections",
        "-I",
        str(work),
        "-I",
        str(ROOT / "runtime" / "include"),
    ]
    for define in defines:
        cmd.append(f"-D{define}")
    cmd.extend([
        str(work / "driver.c"),
        str(memory_copy),
        str(work / "stubs.c"),
        str(work / trace_source),
        "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
        "-o",
        str(exe),
    ])
    subprocess.run(cmd, cwd=work, check=True)
    return exe


def parse(line: str) -> dict[str, str]:
    return dict(part.split("=", 1) for part in line.split())


def run(exe: pathlib.Path, loops: int = 1) -> dict[str, str]:
    cmd = [str(exe)]
    if loops != 1:
        cmd.extend(["--loops", str(loops)])
    proc = subprocess.run(cmd, cwd=exe.parent, check=True, text=True,
                          stdout=subprocess.PIPE)
    return parse(proc.stdout.strip())


def run_regression(cc: str) -> None:
    with tempfile.TemporaryDirectory(prefix="psx-memory-trace-") as td:
        root = pathlib.Path(td)
        prod_dir = root / "prod"
        dbg_dir = root / "debug"
        prod_dir.mkdir()
        dbg_dir.mkdir()
        prod = build(cc, prod_dir, MEMORY_C, "current_prod",
                     ("PSX_NO_DEBUG_TOOLS",), "trace_counting.c")
        debug = build(cc, dbg_dir, MEMORY_C, "current_debug",
                      tuple(), "trace_counting.c")
        prod_out = run(prod, 4)
        debug_out = run(debug, 4)
    if prod_out["trace_calls"] != "0":
        fail("production memory.c still calls debug_server_trace_write_check")
    if debug_out["trace_calls"] != "24":
        fail("debug memory.c no longer calls all six trace write probes")
    if prod_out["state"] != debug_out["state"]:
        fail("production guard changed guest-visible write state")
    for field in ("parity", "card", "stores"):
        if prod_out[field] != debug_out[field]:
            fail(f"production guard changed {field} count")


def run_with_baseline(cc: str, baseline: pathlib.Path) -> None:
    if not baseline.exists():
        fail(f"baseline memory source does not exist: {baseline}")
    with tempfile.TemporaryDirectory(prefix="psx-memory-trace-baseline-") as td:
        root = pathlib.Path(td)
        old_dir = root / "old"
        new_dir = root / "new"
        old_dir.mkdir()
        new_dir.mkdir()
        old = build(cc, old_dir, baseline, "baseline_prod",
                    ("PSX_NO_DEBUG_TOOLS",), "trace_counting.c")
        new = build(cc, new_dir, MEMORY_C, "candidate_prod",
                    ("PSX_NO_DEBUG_TOOLS",), "trace_counting.c")
        old_out = run(old, 4)
        new_out = run(new, 4)
    if old_out["trace_calls"] != "24":
        fail("baseline production source did not execute the six trace calls")
    if new_out["trace_calls"] != "0":
        fail("candidate production source still executes trace calls")
    if old_out["state"] != new_out["state"]:
        fail("candidate production changed guest-visible state vs baseline")
    for field in ("parity", "card", "stores"):
        if old_out[field] != new_out[field]:
            fail(f"candidate production changed {field} count vs baseline")


def build_bench(cc: str, out_dir: pathlib.Path, baseline: pathlib.Path) -> None:
    if not baseline.exists():
        fail(f"baseline memory source does not exist: {baseline}")
    out_dir.mkdir(parents=True, exist_ok=True)
    old_dir = out_dir / "baseline_prod_empty"
    new_dir = out_dir / "candidate_prod_empty"
    old_dir.mkdir(parents=True, exist_ok=True)
    new_dir.mkdir(parents=True, exist_ok=True)
    old = build(cc, old_dir, baseline, "baseline_prod_empty",
                ("PSX_NO_DEBUG_TOOLS",), "trace_empty.c")
    new = build(cc, new_dir, MEMORY_C, "candidate_prod_empty",
                ("PSX_NO_DEBUG_TOOLS",), "trace_empty.c")
    print(f"baseline={old}")
    print(f"candidate={new}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=default_cc())
    parser.add_argument("--baseline-memory", type=pathlib.Path)
    parser.add_argument("--build-bench", type=pathlib.Path,
                        help="build old/candidate empty-trace binaries here")
    args = parser.parse_args()
    args.cc = normalize_cc(args.cc)

    memory = source_text(MEMORY_C)
    assert_guarded(memory)
    assert_observers_preserved(memory)

    if args.build_bench:
        if not args.baseline_memory:
            fail("--build-bench requires --baseline-memory")
        build_bench(args.cc, args.build_bench, args.baseline_memory)
        return 0

    run_regression(args.cc)
    if args.baseline_memory:
        run_with_baseline(args.cc, args.baseline_memory)
    print("PASS: actual memory.c debug trace guards preserve production state")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
    except subprocess.CalledProcessError as exc:
        print(f"FAIL: command exited {exc.returncode}: {' '.join(map(str, exc.cmd))}")
        sys.exit(1)
