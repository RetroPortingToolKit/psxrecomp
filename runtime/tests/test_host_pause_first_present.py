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

if __name__=='__main__':main()
