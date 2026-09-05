#!/usr/bin/env python3
"""Validate production GP0 history gating against actual gpu.c.

This is a parser/register/VRAM regression with renderer stubs, not a full
backend rendering-correctness test.
"""

from __future__ import annotations

import argparse
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU_C = ROOT / "runtime" / "src" / "gpu.c"
DEFAULT_GCC = pathlib.Path(r"C:\msys64\mingw64\bin\gcc.exe")

DRIVER_C = r"""
#include "gpu.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

extern uint64_t s_frame_count;
uint32_t gpu_get_opcode_count(uint8_t op);
uint64_t gpu_get_gp0_count(void);
void gpu_get_gp0_stats(uint64_t* nop, uint64_t* fill, uint64_t* draw,
                       uint64_t* env, uint64_t* copy);
uint32_t gpu_snapshot_bytes(void);
void gpu_snapshot_write(uint8_t *p);

static uint32_t timeline_hash = 2166136261u;
static int checkpoint_failed = 0;
static FILE *trace_file;

static uint32_t mix(uint32_t h, uint32_t v) {
    return (h ^ v) * 16777619u;
}

static void checkpoint(void) {
    uint32_t n = gpu_snapshot_bytes();
    uint8_t *snap = (uint8_t *)malloc(n ? n : 1u);
    if (!snap) {
        checkpoint_failed = 1;
        return;
    }
    gpu_snapshot_write(snap);
    if (fwrite(&n, sizeof(n), 1, trace_file) != 1 ||
        fwrite(snap, 1, n, trace_file) != n) checkpoint_failed = 1;
    timeline_hash = mix(timeline_hash, n);
    for (uint32_t i = 0; i < n; i++) timeline_hash = mix(timeline_hash, snap[i]);
    free(snap);
    for (int y = 0; y < 512; y++) {
        uint16_t row[1024];
        for (int x = 0; x < 1024; x++) {
            row[x] = gpu_vram_peek(x, y);
            timeline_hash = mix(timeline_hash, row[x]);
        }
        if (fwrite(row, sizeof(row), 1, trace_file) != 1) checkpoint_failed = 1;
    }
}

static void emit_env(uint32_t word) {
    gpu_write_gp0(word);
    s_frame_count++;
    checkpoint();
}

static void emit_fill(uint32_t color, uint32_t xy, uint32_t wh) {
    gpu_write_gp0(0x02000000u | color);
    gpu_write_gp0(xy);
    gpu_write_gp0(wh);
    s_frame_count++;
    checkpoint();
}

static void emit_upload_2x1(uint32_t xy, uint32_t pixels) {
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(xy);
    gpu_write_gp0((1u << 16) | 2u);
    gpu_write_gp0(pixels);
    s_frame_count++;
    checkpoint();
}

static void emit_copy(uint32_t src_xy, uint32_t dst_xy, uint32_t wh) {
    gpu_write_gp0(0x80000000u);
    gpu_write_gp0(src_xy);
    gpu_write_gp0(dst_xy);
    gpu_write_gp0(wh);
    s_frame_count++;
    checkpoint();
}

static void emit_polyline(void) {
    gpu_write_gp0(0x4800007Fu);
    gpu_write_gp0((20u << 16) | 10u);
    gpu_write_gp0(0x55555555u);
    s_frame_count++;
    checkpoint();
}

static uint32_t hash_state(void) {
    GpuDrawArea a;
    uint64_t nop = 0, fill = 0, draw = 0, env = 0, copy = 0;
    gpu_get_draw_area(&a);
    gpu_get_gp0_stats(&nop, &fill, &draw, &env, &copy);
    uint32_t h = 2166136261u;
#define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
    MIX(gpu_get_gp0_count());
    MIX(nop); MIX(fill); MIX(draw); MIX(env); MIX(copy);
    MIX(gpu_get_opcode_count(0x00)); MIX(gpu_get_opcode_count(0x02));
    MIX(gpu_get_opcode_count(0xA0)); MIX(gpu_get_opcode_count(0xE1));
    MIX(gpu_get_opcode_count(0xE3)); MIX(gpu_get_opcode_count(0xE4));
    MIX(gpu_get_opcode_count(0xE5)); MIX(gpu_get_opcode_count(0xE6));
    MIX(a.left); MIX(a.top); MIX(a.right); MIX(a.bottom);
    MIX((uint32_t)a.offset_x); MIX((uint32_t)a.offset_y);
    MIX(gpu_vram_peek(16, 5)); MIX(gpu_vram_peek(31, 5));
    MIX(gpu_vram_peek(40, 9)); MIX(gpu_vram_peek(41, 9));
    MIX(gpu_read_gpustat());
    MIX(gpu_ws_census_seq());
    MIX(timeline_hash);
    return h;
#undef MIX
}

int main(int argc, char **argv) {
    GpuGp0RingEntry entries[16], copy_entries[16], poly_entries[16];
    uint32_t oldest = 99, newest = 99;
    if (argc != 2 || !(trace_file = fopen(argv[1], "wb"))) return 2;

    gpu_init();
    gpu_ws_census_set(0);
    checkpoint();
    gpu_set_gp0_source(0x00123400u);
    emit_env(0xE10003FFu);
    gpu_set_gp0_source(0x00123404u);
    emit_env(0xE3000000u | (3u << 10) | 2u);
    gpu_set_gp0_source(0x00123408u);
    emit_env(0xE4000000u | (80u << 10) | 90u);
    gpu_set_gp0_source(0x0012340Cu);
    emit_env(0xE5000000u | (8u << 11) | 7u);
    gpu_set_gp0_source(0x00123410u);
    emit_env(0xE6000001u);
    gpu_set_gp0_source(0x00123414u);
    emit_fill(0x0000FFu, (5u << 16) | 16u, (1u << 16) | 16u);
    gpu_set_gp0_source(0x00123418u);
    emit_upload_2x1((9u << 16) | 40u, 0x9ABC1234u);
    gpu_set_gp0_source(0x0012341Cu);
    emit_copy((9u << 16) | 40u, (10u << 16) | 42u, (1u << 16) | 2u);
    gpu_set_gp0_source(0x00123420u);
    emit_polyline();
    gpu_set_gp0_source(0x00123424u);
    emit_env(0x00000000u);
    if (fclose(trace_file) != 0) checkpoint_failed = 1;
    if (checkpoint_failed) {
        fprintf(stderr, "checkpoint allocation/write failed\n");
        return 2;
    }

    gpu_gp0_ring_frame_span(&oldest, &newest);
    int dumped = gpu_gp0_ring_dump_frame(0, entries, 16);
    int copy_dumped = gpu_gp0_ring_dump_frame(7, copy_entries, 16);
    int poly_dumped = gpu_gp0_ring_dump_frame(8, poly_entries, 16);
    printf("hash=%08x total=%llu cap=%u max=%u span=%u:%u dump0=%d "
           "src=%08x op=%02x cmd0=%08x "
           "copydump=%d copysrc=%08x copyop=%02x copycsp=%08x "
           "polydump=%d polyop=%02x polycmd0=%08x\n",
           hash_state(), (unsigned long long)gpu_gp0_ring_total(),
           gpu_gp0_ring_capacity(), gpu_gp0_ring_max_words(), oldest, newest,
           dumped, dumped > 0 ? entries[0].src_addr : 0u,
           dumped > 0 ? entries[0].opcode : 0u,
           dumped > 0 ? entries[0].cmd[0] : 0u,
           copy_dumped, copy_dumped > 0 ? copy_entries[0].src_addr : 0u,
           copy_dumped > 0 ? copy_entries[0].opcode : 0u,
           copy_dumped > 0 ? copy_entries[0].csp : 0u,
           poly_dumped, poly_dumped > 0 ? poly_entries[0].opcode : 0u,
           poly_dumped > 0 ? poly_entries[0].cmd[0] : 0u);
    return 0;
}
"""

STUBS_C = r"""
#include "gpu.h"
#include "gpu_render.h"
#include "cpu_state.h"
#include "color_lut.h"
#include "ws_aspect_cone_math.h"
#include "ws_ui_group.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t s_frame_count = 0;
uint32_t g_debug_last_store_pc = 0x80012340u;
uint32_t g_debug_current_func_addr = 0x80045670u;
CPUState *debug_cpu_ptr = 0;
uint32_t i_stat = 0;
uint32_t i_mask = 0;
static uint16_t *g_vram;

uint32_t debug_guest_ra(void) { return 0x8000AAAAu; }
uint32_t debug_guest_sp(void) { return 0x8010FFF0u; }
uint8_t *memory_get_ram_ptr(void) { static uint8_t ram[2 * 1024 * 1024]; return ram; }
uint16_t psx_read_half(uint32_t addr) { (void)addr; return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_read_word(uint32_t addr) { (void)addr; return 0; }
void psx_fatal_halt(const char *reason) { fprintf(stderr, "%s\n", reason); abort(); }
void crash_trace_note_gpu_fatal(uint32_t opcode, uint32_t word) { (void)opcode; (void)word; }
void event_ring_note_gpu_irq(uint32_t stat) { (void)stat; }
int g_psx_vram_dirty_tracking = 0;
int g_exec_phase = 0;
void gpu_vram_dirty_mark_all(void) {}
void gpu_vram_dirty_mark_row_impl(uint32_t row) { (void)row; }
void gpu_vram_dirty_mark_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)x; (void)y; (void)w; (void)h; }
void text_xlate_vram_upload(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ (void)x; (void)y; (void)w; (void)h; }

int ws_cull_should_keep(uint32_t addr) { (void)addr; return 1; }
int ws_ui_group_should_keep(uint32_t addr) { (void)addr; return 1; }
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t address) { return address; }
void ws_ui_group_assign(WsUiGroupItem *items, size_t count,
                        int32_t display_width, int dense_menu)
{ (void)items; (void)count; (void)display_width; (void)dense_menu; }
int32_t ws_ui_anchor_for_bounds(int32_t x, int32_t width, int32_t display_width)
{ (void)x; (void)width; return display_width / 2; }
int gte_geometry_correction_enabled(void) { return 0; }
void pgxp_set_enabled(int enabled) { (void)enabled; }
int pgxp_get_gte_sxy_checked(uint32_t slot, uint32_t packed, int require_valid,
                             int32_t *x16, int32_t *y16)
{ (void)slot; (void)packed; (void)require_valid; (void)x16; (void)y16; return 0; }
int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                            int32_t int_x, int32_t int_y,
                            int32_t *x16, int32_t *y16, uint16_t *sz)
{ (void)addr; (void)packet_word; (void)int_x; (void)int_y; (void)x16; (void)y16; (void)sz; return 0; }
int gte_precision_load_word(uint32_t addr, uint32_t packed,
                            int32_t *x16, int32_t *y16, uint16_t *z)
{ (void)addr; (void)packed; (void)x16; (void)y16; (void)z; return 0; }
void pgxp_invalidate_all(void) {}
void pgxp_invalidate_word(uint32_t addr) { (void)addr; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux)
{ (void)kind; (void)detail; (void)aux; }
int psx_get_in_exception(void) { return 0; }
int psx_netplay_active(void) { return 0; }
int sio_hold_present_for_card(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc = 0;
uint32_t psx_last_irq_check_pc = 0;
uint32_t psx_netplay_rb_sticky_bb_pc = 0;
void mod_runtime_on_vblank(void) {}
void sio_ape_card_unstick_pump(void) {}
void psx_write_half(uint32_t addr, uint16_t val) { (void)addr; (void)val; }
int mdec_recently_active(uint32_t within_frames) { (void)within_frames; return 0; }
uint32_t psx_ws_widen_angle_q12(uint32_t vanilla, int extent_pixels)
{ (void)extent_pixels; return vanilla; }
int psx_ws_aspect_cone_contains(int32_t x, int32_t z, int32_t y,
                                int32_t fx, int32_t fz, int32_t fy,
                                uint32_t threshold, int extent_pixels)
{ (void)x; (void)z; (void)y; (void)fx; (void)fz; (void)fy; (void)threshold; (void)extent_pixels; return 0; }
uint32_t sw_perspective_triangle_count(void) { return 0; }
bool screen_kind_from_name(const char* name, ScreenKind* out)
{ (void)name; if (out) *out = SCREEN_RAW; return 0; }
ColorLut* color_lut_create(const ColorSettings* settings)
{ (void)settings; return 0; }
void color_lut_destroy(ColorLut* lut) { (void)lut; }
bool color_lut_is_passthrough(const ColorLut* lut) { (void)lut; return 1; }
void color_lut_map555(const ColorLut* lut, uint16_t bgr555,
                      uint8_t* r, uint8_t* g, uint8_t* b)
{ (void)lut; if (r) *r = (uint8_t)(bgr555 & 0x1F); if (g) *g = (uint8_t)((bgr555 >> 5) & 0x1F); if (b) *b = (uint8_t)((bgr555 >> 10) & 0x1F); }
int ws_sprt_fixed_transform(int32_t *x, int32_t y, int w)
{ (void)x; (void)y; return w; }
int ws_nw_hud_shift(int32_t x, int w) { (void)x; (void)w; return 0; }
int ws_is_fb_base(int x) { (void)x; return 0; }
int ws_local_viewport_draw_target(int *base) { if (base) *base = 0; return 0; }
void ws_nw_sync_target(void) {}
int ws_active(void) { return 0; }
int ws_engaged(void) { return 0; }

void gr_init(uint16_t *vram) { g_vram = vram; }
uint16_t gr_vram_read(int x, int y) { return g_vram[(y & 511) * 1024 + (x & 1023)]; }
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *pixels)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            g_vram[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] = pixels[yy * w + xx];
}
void gr_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            g_vram[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] = color;
}
void gr_wide_clear(int x, int y, int h, uint16_t color)
{ (void)x; (void)y; (void)h; (void)color; }
void gr_wide_disable_target(void) {}
void gr_wide_configure(int width, int offset) { (void)width; (void)offset; }
void gr_wide_set_target(int base) { (void)base; }
void gr_wide_clear_margins(int base, int y, int h, uint16_t color, int mode)
{ (void)base; (void)y; (void)h; (void)color; (void)mode; }
void gr_set_draw_area(int l, int t, int r, int b) { (void)l; (void)t; (void)r; (void)b; }
void gr_set_draw_offset(int x, int y) { (void)x; (void)y; }
void gr_set_mask_bits(int set, int check) { (void)set; (void)check; }
void gr_set_texture_window(uint32_t value) { (void)value; }
void gr_set_semi_transparency(int enabled, int mode) { (void)enabled; (void)mode; }
void gr_set_color_modulation(int r, int g, int b, int raw)
{ (void)r; (void)g; (void)b; (void)raw; }
void gr_set_perspective_triangle(int enabled, float a, float b, float c)
{ (void)enabled; (void)a; (void)b; (void)c; }
void gr_set_precise_triangle(int enabled, int32_t ax, int32_t ay, int32_t bx,
                             int32_t by, int32_t cx, int32_t cy)
{ (void)enabled; (void)ax; (void)ay; (void)bx; (void)by; (void)cx; (void)cy; }
void gr_copy_rect(int sx, int sy, int dx, int dy, int w, int h)
{ (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h; }
void gr_draw_flat_rect(int x, int y, int w, int h, uint16_t color)
{ (void)x; (void)y; (void)w; (void)h; (void)color; }
void gr_draw_flat_triangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c)
{ (void)x0;(void)y0;(void)x1;(void)y1;(void)x2;(void)y2;(void)c; }
void gr_draw_gouraud_triangle(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1,int x2,int y2,uint16_t c2)
{ (void)x0;(void)y0;(void)c0;(void)x1;(void)y1;(void)c1;(void)x2;(void)y2;(void)c2; }
void gr_draw_line(int x0,int y0,int x1,int y1,uint16_t c)
{ (void)x0;(void)y0;(void)x1;(void)y1;(void)c; }
void gr_draw_shaded_line(int x0,int y0,uint16_t c0,int x1,int y1,uint16_t c1)
{ (void)x0;(void)y0;(void)c0;(void)x1;(void)y1;(void)c1; }
void gr_draw_textured_rect(int x,int y,int w,int h,int u,int v,uint16_t clut_x,uint16_t clut_y,uint16_t texpage)
{ (void)x;(void)y;(void)w;(void)h;(void)u;(void)v;(void)clut_x;(void)clut_y;(void)texpage; }
void gr_draw_textured_rect_scaled(int x,int y,int w,int h,int u0,int v0,int u1,int v1,uint16_t clut_x,uint16_t clut_y,uint16_t texpage)
{ (void)x;(void)y;(void)w;(void)h;(void)u0;(void)v0;(void)u1;(void)v1;(void)clut_x;(void)clut_y;(void)texpage; }
void gr_draw_textured_triangle(int x0,int y0,int u0,int v0,int x1,int y1,int u1,int v1,int x2,int y2,int u2,int v2,uint16_t clut_x,uint16_t clut_y,uint16_t texpage)
{ (void)x0;(void)y0;(void)u0;(void)v0;(void)x1;(void)y1;(void)u1;(void)v1;(void)x2;(void)y2;(void)u2;(void)v2;(void)clut_x;(void)clut_y;(void)texpage; }
void gr_draw_shaded_textured_triangle(int x0,int y0,int u0,int v0,uint32_t c0,int x1,int y1,int u1,int v1,uint32_t c1,int x2,int y2,int u2,int v2,uint32_t c2,uint16_t clut_x,uint16_t clut_y,uint16_t texpage,int raw)
{ (void)x0;(void)y0;(void)u0;(void)v0;(void)c0;(void)x1;(void)y1;(void)u1;(void)v1;(void)c1;(void)x2;(void)y2;(void)u2;(void)v2;(void)c2;(void)clut_x;(void)clut_y;(void)texpage;(void)raw; }
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


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


def assert_source_shape(gpu: str) -> None:
    guard = gpu.index("#ifndef PSX_NO_DEBUG_TOOLS", gpu.index("debug tools only"))
    alt = gpu.index("#else", guard)
    end = gpu.index("#endif", alt)
    debug_block = gpu[guard:alt]
    prod_block = gpu[alt:end]
    for token in ("GP0_RING_CAP", "static GpuGp0RingEntry *gp0_ring",
                  "gp0_capture_builder_chain", "g_gp0_last_copy_sp",
                  "debug_guest_ra()", "debug_guest_sp()"):
        if token not in debug_block:
            fail(f"debug GP0 history block lost {token}")
        if token in prod_block:
            fail(f"production GP0 history block still references {token}")
    for token in ("gpu_gp0_ring_total(void) { return 0; }",
                  "gpu_gp0_ring_capacity(void) { return 0; }",
                  "return GPU_GP0_RING_MAX_WORDS;",
                  "return 0;"):
        if token not in prod_block:
            fail(f"production unavailable accessor missing {token}")
    set_source = function_body(gpu, "void gpu_set_gp0_source(")
    if "gp0_next_source_addr = addr;" not in set_source:
        fail("gpu_set_gp0_source no longer preserves source tracking")
    execute = function_body(gpu, "static void gp0_execute_command(")
    order = [execute.index(token) for token in (
        "gp0_opcode_count[opcode]++",
        "gp0_ring_record(gp0_cmd_buf, gp0_words_needed)",
        "ws_bg_phase_note(opcode)",
        "ws_census_record(opcode, cvx, cvy)",
        "ws_note_overhang(opcode)",
    )]
    if order != sorted(order):
        fail("GP0 command accounting/ring/WS ordering changed")
    if "gp0_ring_record(hdr_only, 1);" not in gpu:
        fail("polyline header capture hook was removed")


def write_sources(work: pathlib.Path) -> None:
    (work / "driver.c").write_text(DRIVER_C, encoding="utf-8")
    (work / "stubs.c").write_text(STUBS_C, encoding="utf-8")


def build(cc: str, work: pathlib.Path, name: str, prod: bool) -> pathlib.Path:
    exe = work / f"{name}.exe"
    cmd = [
        cc, "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
        "-I", str(ROOT / "runtime" / "include"),
    ]
    if prod:
        cmd.append("-DPSX_NO_DEBUG_TOOLS")
    cmd.extend([
        str(work / "driver.c"),
        str(GPU_C),
        str(work / "stubs.c"),
    ])
    if platform.system() == "Darwin":
        cmd.append("-Wl,-dead_strip")
    else:
        cmd.append("-Wl,--gc-sections")
    cmd.extend(["-o", str(exe)])
    subprocess.run(cmd, cwd=work, check=True)
    return exe


def run(exe: pathlib.Path) -> dict[str, str]:
    proc = subprocess.run([str(exe), str(exe.with_suffix(".state"))],
                          cwd=exe.parent, check=True, text=True,
                          stdout=subprocess.PIPE)
    return dict(part.split("=", 1) for part in proc.stdout.strip().split())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=default_cc())
    args = parser.parse_args()
    cc = args.cc.replace("\\", "/")

    assert_source_shape(GPU_C.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="psx-gp0-history-") as td:
        work = pathlib.Path(td)
        write_sources(work)
        debug = build(cc, work, "debug", prod=False)
        prod = build(cc, work, "prod", prod=True)
        debug_out = run(debug)
        prod_out = run(prod)
        debug_state = debug.with_suffix(".state").read_bytes()
        prod_state = prod.with_suffix(".state").read_bytes()
        if not debug_state or debug_state != prod_state:
            fail("production gate changed checkpoint GPU snapshot/VRAM bytes")

    if debug_out["hash"] != prod_out["hash"]:
        fail("production GP0 history gate changed guest-visible GPU state")
    if prod_out["total"] != "0" or prod_out["cap"] != "0":
        fail("production GP0 history remains available")
    if prod_out["span"] != "0:0" or prod_out["dump0"] != "0":
        fail("production GP0 history accessors should report empty capture")
    if prod_out["copydump"] != "0" or prod_out["polydump"] != "0":
        fail("production GP0 history frame dumps should be empty")
    if debug_out["total"] != "10" or int(debug_out["cap"]) == 0:
        fail("debug GP0 history stopped recording")
    if debug_out["src"] != "00123400" or debug_out["op"] != "e1":
        fail("debug GP0 history lost source/opcode tracking")
    if debug_out["cmd0"] != "e10003ff":
        fail("debug GP0 history lost command-word capture")
    if debug_out["copydump"] != "1" or debug_out["copysrc"] != "0012341c":
        fail("debug GP0 history lost copy source tracking")
    if debug_out["copyop"] != "80" or debug_out["copycsp"] != "8010fff0":
        fail("debug GP0 history lost copy builder diagnostics")
    if debug_out["polydump"] != "1" or debug_out["polyop"] != "48":
        fail("debug GP0 history lost polyline header capture")
    if debug_out["polycmd0"] != "4800007f":
        fail("debug GP0 history lost polyline command word")
    print("PASS: actual gpu.c production GP0 history gate preserves GPU state")
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
