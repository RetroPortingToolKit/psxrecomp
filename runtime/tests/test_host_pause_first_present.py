#!/usr/bin/env python3
"""Execute the production host-pause presenter before and after a held frame."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_netplay_load_apply_flow import function_body

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
int g_gl_active, g_vk_active, s_sw_hold_valid;
void *sdl_renderer, *sdl_texture;
int s_sw_hold_src, s_sw_hold_dst;
static int held, holds, blanks, vk_blanks, ticks, suspended;
uint32_t SDL_GetTicks() { return 7; }
void psx_rewind_present_tick(uint32_t t) { ticks += t == 7; }
void gl_renderer_set_interpolation_suspended(int x) { suspended = x; }
int gl_renderer_present_hold_last() { ++holds; return held; }
void gl_renderer_present_blank() { ++blanks; }
void vk_renderer_present_blank() { ++vk_blanks; }
void SDL_SetRenderDrawColor(void*,int,int,int,int) {}
void SDL_RenderClear(void*) {}
void SDL_RenderCopy(void*,void*,void*,void*) {}
void host_osd_draw_sdl(void*) {}
void SDL_RenderPresent(void*) {}
void software_present_with_telemetry(void* renderer) { SDL_RenderPresent(renderer); }
"""
CHECKS = r"""
int main() {
    g_gl_active = 1;
    held = 0; rewind_pause_present();
    if (holds != 1 || blanks != 1 || ticks != 1 || !suspended) {
        fprintf(stderr, "No held frame: menu draw must still get a blank present\n"); return 1;
    }
    held = 1; rewind_pause_present();
    if (holds != 2 || blanks != 1 || ticks != 2) return 2;
    g_gl_active = 0; g_vk_active = 1; rewind_pause_present();
    if (holds != 2 || blanks != 1 || vk_blanks != 1) return 3;
    puts("host pause: first-frame fallback and existing held-frame path PASS");
    return 0;
}
"""


def check_window_resize(cxx, folder, source):
    body = function_body(source, 'extern "C" int psx_debug_window_size(int w, int h)')
    function = 'extern "C" int psx_debug_window_size(int w, int h) {' + body + '}'
    prelude = r"""
#include <cassert>
void *sdl_window = reinterpret_cast<void *>(1);
static bool held, resize_ok = true;
static int resized, released, acquired;
int rt_held() { return held; }
void rt_acquire(const char *) { if (!held) { held = true; ++acquired; } }
void rt_release() { assert(held); held = false; ++released; }
#ifdef PSX_SDL3
bool SDL_SetWindowSize(void *, int, int) {
    assert(held); ++resized; return resize_ok;
}
#else
void SDL_SetWindowSize(void *, int, int) { assert(held); ++resized; }
#endif
"""
    checks = r"""
int main() {
    assert(!psx_debug_window_size(1, 480) && !resized && !acquired);
    assert(psx_debug_window_size(640, 480));
    assert(!held && acquired == 1 && released == 1 && resized == 1);
    held = true;
    assert(psx_debug_window_size(800, 600));
    assert(held && acquired == 1 && released == 1 && resized == 2);
    held = false;
#ifdef PSX_SDL3
    resize_ok = false;
    assert(!psx_debug_window_size(640, 480));
    assert(!held && acquired == 2 && released == 2 && resized == 3);
#endif
}
"""
    for sdl in (2, 3):
        cpp = folder / f'window-resize-sdl{sdl}.cpp'
        exe = folder / f'window-resize-sdl{sdl}.exe'
        cpp.write_bytes((prelude + function + checks).encode())
        defines = ['-DPSX_SDL3=1'] if sdl == 3 else []
        subprocess.run([cxx, '-std=c++17', *defines, str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('window resize: SDL2/SDL3 result and held-context ownership PASS')


def main():
    a=argparse.ArgumentParser();a.add_argument('--cxx',required=True);args=a.parse_args()
    source=(Path(__file__).resolve().parents[1]/'src/main.cpp').read_text(encoding='utf-8')
    body=function_body(source,'static void rewind_pause_present(void)')
    fixed='static void rewind_pause_present(void) {'+body+'}'
    old=fixed.replace('if (!gl_renderer_present_hold_last()) gl_renderer_present_blank();',
                      '(void)gl_renderer_present_hold_last();')
    assert old != fixed
    with tempfile.TemporaryDirectory() as folder:
        p=Path(folder)
        for label,code in [('fixed',fixed),('before',old)]:
            cpp=p/(label+'.cpp');exe=p/(label+'.exe')
            cpp.write_text(PRELUDE+code+CHECKS,encoding='utf-8')
            subprocess.run([args.cxx,'-std=c++17',str(cpp),'-o',str(exe)],check=True)
            r=subprocess.run([str(exe)],capture_output=True,text=True)
            if label=='fixed':
                assert r.returncode==0,r.stdout+r.stderr
                print(r.stdout.strip())
            else:
                assert r.returncode==1,'Regression must reject the old missing fallback'
                print('pre-fix presenter: regression rejects missing first-frame draw')
        check_window_resize(args.cxx, p, source)

if __name__=='__main__':main()
