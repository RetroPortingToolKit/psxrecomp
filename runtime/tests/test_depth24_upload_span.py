"""Run production upload-span tracking against full-width and sliced movies.

No retail data or graphics context is needed. Usage:
  test_depth24_upload_span.py <source> --compiler <cc> --output <directory>
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def body(text, name):
    match = re.search(r'\b(?:static\s+)?void\s+' + name + r'\s*\([^;]*?\)\s*\{', text, re.S)
    if not match:
        raise AssertionError(f'{name} not found in gpu.c')
    start, depth = match.end(), 1
    for position in range(start, len(text)):
        depth += text[position] == '{'
        depth -= text[position] == '}'
        if not depth:
            return text[start:position]
    raise AssertionError(name)


FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
static uint32_t display_depth = 1, s_d24_upload_x1;
static int s_d24_present_hold;
static uint32_t s_d24_prev_disp_h, v_display_y1, v_display_y2;
static uint32_t hres1, hres2, vres, video_mode, vertical_interlace, interlace_field, reverse_flag;
static void __attribute__((unused)) gr_display_mode_changed(void) {}
static void __attribute__((unused)) depth24_reset_rows(void) {}
static int psx_gp1_display_mode_is_pal(uint32_t val) { return (int)((val >> 3) & 1u); }
static int psx_video_timing_set_pal(int pal) { return pal; }
static void depth24_note_upload(uint32_t x, uint32_t w) { NOTE }
static void gpu_depth24_upload_span_reset(void) { RESET }
static void gp1_v_display_range(uint32_t val) { RANGE }
static void gp1_display_mode(uint32_t val) { MODE }
#define CHECK(value, message) do { if (!(value)) { puts(message); return 1; } } while (0)
int main(void) {
    /* A 320-halfword still precedes a 320-RGB-pixel movie, uploaded in strips. */
    depth24_note_upload(0, 320);
    for (unsigned x = 0; x < 480; x += 24) depth24_note_upload(x, 24);
    CHECK(s_d24_upload_x1 == 480, "FAIL: narrow movie strips leave stale coverage");
    depth24_note_upload(0, 16);
    CHECK(s_d24_upload_x1 == 480, "FAIL: smaller upload collapses span");
    gpu_depth24_upload_span_reset();
    depth24_note_upload(0, 768);
    CHECK(s_d24_upload_x1 == 768, "FAIL: full-width movie coverage");
    gpu_depth24_upload_span_reset();
    s_d24_present_hold = 1;
    depth24_note_upload(0, 480);
    CHECK(s_d24_upload_x1 == 0, "FAIL: present hold admits upload");
    s_d24_present_hold = 0;
    display_depth = 0;
    depth24_note_upload(0, 480);
    CHECK(s_d24_upload_x1 == 0, "FAIL: 15-bit upload changes span");
    display_depth = 1;
    depth24_note_upload(900, 256);
    CHECK(s_d24_upload_x1 == 1024, "FAIL: span exceeds VRAM row");
    gp1_display_mode(0);
    CHECK(s_d24_upload_x1 == 0, "FAIL: leaving 24-bit retains coverage");
    gp1_display_mode(0x10);
    CHECK(s_d24_upload_x1 == 0, "FAIL: entering 24-bit retains coverage");
    depth24_note_upload(0, 480);
    gp1_v_display_range(240u << 10);
    CHECK(s_d24_upload_x1 == 480, "FAIL: initial vertical range clears coverage");
    gp1_v_display_range(128u << 10);
    CHECK(s_d24_upload_x1 == 0 && s_d24_present_hold == 3, "FAIL: changed movie band retains coverage");
    depth24_note_upload(0, 768);
    CHECK(s_d24_upload_x1 == 0, "FAIL: changed-band hold admits upload");
    puts("PASS: sliced/full movies, monotonic span, reset, hold, depth, row bound, depth and band transitions");
    return 0;
}
'''


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = (args.source / 'runtime/src/gpu.c').read_text(encoding='utf-8')
    code = FIXTURE.replace('NOTE', body(source, 'depth24_note_upload')).replace(
        'RESET', body(source, 'gpu_depth24_upload_span_reset')).replace(
        'RANGE', body(source, 'gp1_v_display_range')).replace('MODE', body(source, 'gp1_display_mode'))
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.output / 'upload_span.c'
    fixture.write_text(code, encoding='utf-8')
    exe = args.output / 'upload_span.exe'
    command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(exe)]
    compiled = subprocess.run(command, capture_output=True, text=True,
                              encoding='utf-8', errors='replace')
    result = subprocess.run([str(exe)], capture_output=True, text=True,
                              encoding='utf-8', errors='replace') if compiled.returncode == 0 else compiled
    receipt = {'source': str(args.source), 'command': command, 'compile_exit': compiled.returncode,
               'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    (args.output / 'result.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(receipt, indent=2))
    raise SystemExit(result.returncode)
