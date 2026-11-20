"""Execute the production GL 15/24-bit ownership bodies against a one-word fake VRAM.

Needs no game data, GL context or build tree: the real function bodies are
lifted out of gpu.c, gpu_gl_renderer.c and gpu_render.c and compiled into a
small fixture. Covers the ownership handoff around GP1(08h):

  * GP1(08h) runs the backend transition at once, so a GP0 copy issued after
    a 24-bit to 15-bit switch survives the later CPU upload;
  * the first 24-bit CPU upload is not overwritten by the entry readback;
  * a GP0 fill issued while 24-bit scanout is active reaches the CPU mirror
    that 24-bit presents read;
  * a backend without the hook keeps the lazy path.

Usage: test_depth24_transition.py <repository root> --compiler <cc> --output <dir>
"""
from pathlib import Path
import argparse
import json
import re
import subprocess


def body(text, name):
    match = re.search(r'\b(?:static\s+)?(?:void|int)\s+' + name + r'\s*\([^;]*?\)\s*\{', text, re.S)
    if not match:
        return None
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
#include <stdlib.h>
#include "psx_video_timing.h"
int psx_video_timing_set_pal(int pal) { return pal; }
static uint32_t hres1, hres2, vres, video_mode, display_depth;
static uint32_t vertical_interlace, interlace_field, reverse_flag, s_d24_upload_x1;
static int s_depth24_skip_up, s_up_nrects, s_d24_skip_fb, s_cpu_auth_dual, s_raster_ok = 1;
static int s_hd_native_authority, s_pass_active, s_fg_drawing, s_depth_used;
static uint16_t cpu, fbo, queued;
static unsigned clears, flushes, syncs, invalidates;
static char events[32]; static unsigned event_n;
static void event(char c) { if (event_n + 1 < sizeof events) { events[event_n++] = c; events[event_n] = 0; } }
static void expect(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s; events=%s cpu=%04x fbo=%04x\n", message, events, cpu, fbo); exit(1); }
}
#define DCLR_FILL 1
#define DEPTH_REQUEST_CLEAR(reason) ((void)(reason))
static int gpu_display_is_depth24(void) { return (int)display_depth; }
static int ctx_depth24(void) { return gpu_display_is_depth24(); }
static int pass_refuse_write(const char *op, int x, int y, int w, int h) { (void)op; (void)x; (void)y; (void)w; (void)h; return 0; }
static int native_draw_begin(void) { return 0; }
static void native_draw_end(int previous) { (void)previous; }
static void ensure_cpu(void) { cpu = fbo; ++syncs; event('S'); }
static void rect_clear(int *r) { *r = 0; }
static void depth24_clear_skipped_fb(void) {
    if (s_d24_skip_fb) { fbo = 0; ++clears; s_d24_skip_fb = 0; event('C'); }
}
static void flush_cpu_upload(void) {
    if (s_up_nrects) { fbo = queued; s_up_nrects = 0; ++flushes; event('U'); }
}
static void gpu_depth24_upload_span_reset(void) { s_d24_upload_x1 = 0; }
/* gpu.c may also reset its per-row movie coverage on a depth change. */
static inline void depth24_reset_rows(void) {}
static void gl_renderer_invalidate_present(void) { ++invalidates; }
static void sw_fill_rect(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; cpu = c; }
static void gpu_fill(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; fbo = c; }
static void sw_vram_transfer_in(int x, int y, int w, int h, const uint16_t *d) { (void)x; (void)y; (void)w; (void)h; cpu = d[0]; }
static void depth24_upload_policy(void) { @POLICY@ }
static int cpu_raster_required(void) { @RASTER@ }
static struct Backend { void (*display_mode_changed)(void); } backend = { @CALLBACK@ };
static struct Backend *g_b = &backend;
static void gr_display_mode_changed(void) { @DISPATCH@ }
static void gp1_display_mode(uint32_t val) { @MODE@ }
static void glb_fill_rect(int x, int y, int w, int h, uint16_t c) { @FILL@ }
static void upload_first_movie_word(uint16_t word) {
    /* The ordering prefix of glb_vram_transfer_in, up to the skip/stage split. */
    const uint16_t d[1] = { word };
    int x = 0, y = 0, w = 1, h = 1;
    @UPLOAD_PREFIX@
}
int main(void) {
    (void)g_b; (void)gr_display_mode_changed;
    (void)s_fg_drawing; /* read by cpu_raster_required only while frame generation exists */
    fbo = 0x1234;
    gp1_display_mode(0x10);
    expect(syncs == 1 && cpu == 0x1234, "GP1(08h) entry synchronizes immediately");
    upload_first_movie_word(0x7e7e);
    expect(cpu == 0x7e7e && syncs == 1, "first 24-bit upload is not overwritten by the entry readback");
    glb_fill_rect(0, 0, 1, 1, 0x0000);
    expect(cpu == 0x0000 && fbo == 0x0000, "fill during 24-bit scanout reaches the CPU mirror");
    s_d24_skip_fb = 1; cpu = 0x5678;
    queued = 0x2222; s_up_nrects = 1;
    gp1_display_mode(0x10);
    expect(s_up_nrects == 1 && syncs == 1, "same mode preserves queued texture");
    gp1_display_mode(0);
    fbo = 0x8842; event('P'); /* later GP0 copy restores a texture word */
    depth24_upload_policy(); /* later CPU upload */
    expect(fbo == 0x8842, "post-mode-switch GP0 copy survives later upload");
    expect(clears == 1 && flushes == 1 && invalidates == 1,
           "exit clears before landing queued texture exactly once");
    cpu = 0x1111;
    glb_fill_rect(0, 0, 1, 1, 0x3333);
    expect(cpu == 0x1111 && fbo == 0x3333, "15-bit fill stays FBO-only");
    gp1_display_mode(0);
    expect(clears == 1 && fbo == 0x3333, "same-depth repeat does not clear");
    backend.display_mode_changed = 0;
    gp1_display_mode(0x10);
    expect(syncs == 1, "backend without callback remains a no-op");
    fbo = 0x4444;
    upload_first_movie_word(0x5555); /* lazy entry path still syncs before writing */
    expect(syncs == 2 && cpu == 0x5555, "lazy entry readback does not overwrite the upload");
    puts("PASS: GP1 entry/exit, first upload, 24-bit fill, queued textures, GP0 copy, optional backend");
    return 0;
}
'''


def run(source, compiler, output):
    src = source / 'runtime/src'
    gpu = (src / 'gpu.c').read_text(encoding='utf-8')
    gl = (src / 'gpu_gl_renderer.c').read_text(encoding='utf-8')
    facade = (src / 'gpu_render.c').read_text(encoding='utf-8')
    parts = {
        'POLICY': body(gl, 'depth24_upload_policy'),
        'RASTER': body(gl, 'cpu_raster_required'),
        'MODE': body(gpu, 'gp1_display_mode'),
        'FILL': body(gl, 'glb_fill_rect'),
        'DISPATCH': body(facade, 'gr_display_mode_changed') or '',
        'CALLBACK': 'depth24_upload_policy' if re.search(
            r'\.display_mode_changed\s*=\s*depth24_upload_policy', gl) else '0',
    }
    transfer = body(gl, 'glb_vram_transfer_in')
    assert all(parts[k] for k in ('POLICY', 'RASTER', 'MODE', 'FILL')) and transfer
    # Keep only the statements before the skip/stage decision.
    parts['UPLOAD_PREFIX'] = transfer.split('if (s_depth24_skip_up', 1)[0]
    code = FIXTURE
    for key, value in parts.items():
        code = code.replace('@' + key + '@', value)
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / 'transition_fixture.c'
    fixture.write_text(code, encoding='utf-8')
    exe = output / 'transition_fixture.exe'
    args = [str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I' + str(source / 'runtime/include'), str(fixture), '-o', str(exe)]
    compile_run = subprocess.run(args, text=True, encoding='utf-8', errors='replace', capture_output=True)
    result = (subprocess.run([str(exe)], text=True, encoding='utf-8', errors='replace', capture_output=True)
              if not compile_run.returncode else compile_run)
    receipt = {'source': str(source), 'command': args, 'compile_exit': compile_run.returncode,
               'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    (output / 'result.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(receipt, indent=2))
    return result.returncode


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path)
    p.add_argument('--compiler', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    raise SystemExit(run(a.source, a.compiler, a.output))
