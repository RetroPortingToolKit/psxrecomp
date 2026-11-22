"""Compile the production SDL sampling functions against a mock pointer and real SIO."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[1]
    source = (runtime / 'src/main.cpp').read_text(encoding='utf-8')
    start = source.index('static bool mouse_seat_configured(void) {')
    end = source.index('static bool guncon_seat_configured(', start)
    production = source[start:end]
    globals_start = source.index('static double g_mouse_sensitivity')
    globals_end = source.index('/* Offline SIO sample loop bound', globals_start)
    globals_text = source[globals_start:globals_end]
    wire = (runtime / 'tests/test_sio_mouse.c').read_text(encoding='utf-8')
    wire = wire[:wire.index('#ifndef SIO_DEVICE_MOUSE')]
    # Preserve the real controller's byte/ACK poll, rather than observing a mock sink.
    preamble = r'''
#include "sio.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
extern "C" { extern uint64_t clock_now; extern uint32_t i_stat; }
using SDL_Window = int;
using SDL_MouseButtonFlags = unsigned;
constexpr unsigned SDL_BUTTON_LMASK=1, SDL_BUTTON_RMASK=4;
constexpr unsigned SDL_BUTTON_LEFT=1, SDL_BUTTON_RIGHT=3;
constexpr unsigned SDL_TOUCH_MOUSEID=~0u, SDL_PEN_MOUSEID=~1u;
struct SDL_MouseButtonEvent { unsigned windowID, which, button; };
static unsigned SDL_GetWindowID(SDL_Window*) { return 1; }
static int window, fake_x, fake_y, fake_buttons, capture_calls;
static bool focused=true, host_ui=false, capture_ok=true, menu=false, rewind_open=false, netplay=false, g_headless=false;
static bool input_guard=false;
static SDL_Window* sdl_window=&window;
struct PlayerInput { int kind=0; };
static PlayerInput g_players[PSX_MAX_PLAYERS];
static int savestate_menu_open;
static bool psx_host_ui_capture_active() { return host_ui; }
static bool psx_runtime_menu_is_open() { return menu; }
static bool psx_rewind_is_open() { return rewind_open; }
static bool psx_netplay_active() { return netplay; }
static bool host_hotkey_input_focused() { return focused; }
static bool savestate_input_guard_active() { return input_guard; }
static bool SDL_SetWindowRelativeMouseMode(SDL_Window*,bool) { ++capture_calls; return capture_ok; }
static unsigned SDL_GetRelativeMouseState(float* x,float* y) {
    *x=(float)fake_x; *y=(float)fake_y; fake_x=fake_y=0; return fake_buttons;
}
constexpr int HOST_KEYMAP_MOUSE_CAPTURE=0;
static void host_keymap_label(int,char* out,size_t n) { std::snprintf(out,n,"Ctrl+F10"); }
static void host_osd_push(const char*,int) {}
'''
    assertions = r'''
int main() {
    setup_bus();
    sample_mouse_ports();
    check(capture_calls==0,"ordinary pad never captures the pointer");
    g_players[0].kind=3;
    sio_set_port_device(0,SIO_DEVICE_MOUSE); sio_set_pad_connected(0,1);
    fake_x=5; fake_y=-3; fake_buttons=SDL_BUTTON_LMASK;
    sample_mouse_ports();
    EXPECT(0,"production SDL motion reaches native SIO",0xff,0x12,0x5a,0xff,0xf4,5,0xfd);
    latch_mouse_button_down({1,0,SDL_BUTTON_LEFT});
    fake_buttons=0;
    sample_mouse_ports();
    EXPECT(0,"released before sample still fires",0xff,0x12,0x5a,0xff,0xf4,0,0);
    EXPECT(0,"short click is consumed",0xff,0x12,0x5a,0xff,0xfc,0,0);
    latch_mouse_button_down({2,0,SDL_BUTTON_LEFT});
    latch_mouse_button_down({1,SDL_TOUCH_MOUSEID,SDL_BUTTON_LEFT});
    latch_mouse_button_down({1,SDL_PEN_MOUSEID,SDL_BUTTON_LEFT});
    input_guard=true;
    latch_mouse_button_down({1,0,SDL_BUTTON_LEFT});
    input_guard=false;
    EXPECT(0,"other windows, pseudo-mice and guarded clicks are ignored",0xff,0x12,0x5a,0xff,0xfc,0,0);
    g_mouse_sensitivity=0.5; fake_buttons=0;
    fake_x=1; sample_mouse_ports();
    EXPECT(0,"fractional motion carries",0xff,0x12,0x5a,0xff,0xfc,0,0);
    fake_x=1; sample_mouse_ports();
    EXPECT(0,"fractional carry reaches next sample",0xff,0x12,0x5a,0xff,0xfc,1,0);
    latch_mouse_button_down({1,0,SDL_BUTTON_LEFT});
    focused=false; fake_x=42; fake_buttons=SDL_BUTTON_RMASK; sample_mouse_ports();
    EXPECT(0,"lost focus releases buttons and motion",0xff,0x12,0x5a,0xff,0xfc,0,0);
    check(!g_mouse_captured,"lost focus releases capture");
    focused=true; menu=true; fake_x=42; sample_mouse_ports();
    check(!g_mouse_captured,"menu owns the pointer");
    menu=false; host_ui=true; sample_mouse_ports();
    check(!g_mouse_captured,"provider menu owns the pointer");
    host_ui=false; capture_ok=false; fake_x=42; sample_mouse_ports();
    check(!g_mouse_captured,"failed capture is not advertised as active");
    EXPECT(0,"failed capture feeds no motion",0xff,0x12,0x5a,0xff,0xfc,0,0);
    capture_ok=true; sample_mouse_ports();
    check(g_mouse_captured,"capture can recover after a failure");
    capture_ok=false; host_ui=true; fake_x=42; sample_mouse_ports();
    check(g_mouse_captured,"failed release preserves actual capture state");
    EXPECT(0,"failed release still feeds no menu input",0xff,0x12,0x5a,0xff,0xfc,0,0);
    capture_ok=true; host_ui=false; netplay=true; sample_mouse_ports();
    check(!g_mouse_captured,"netplay never captures native Mouse input");
    netplay=false; fake_x=2; fake_buttons=0; sample_mouse_ports();
    EXPECT(0,"focus restores clean input",0xff,0x12,0x5a,0xff,0xfc,1,0);
    mouse_capture_toggle();
    check(!g_mouse_captured,"capture hotkey releases pointer");
    fake_x=42; sample_mouse_ports();
    EXPECT(0,"released pointer feeds no movement",0xff,0x12,0x5a,0xff,0xfc,0,0);
    g_headless=true; mouse_capture_sync();
    check(!g_mouse_captured,"headless never captures pointer");
    std::printf("PASS: native Mouse production host sampling through SIO, %u checks\n",checks);
}
'''
    with tempfile.TemporaryDirectory(prefix='native-mouse-host-') as tmp:
        work = Path(tmp)
        c = work / 'wire.c'
        c.write_text(wire, encoding='utf-8')
        cpp = work / 'host.cpp'
        cpp.write_text(preamble + globals_text + production + assertions, encoding='utf-8')
        # The wire poll helpers belong in the C++ fixture, retaining C linkage for the SIO API.
        cpp.write_text(preamble + wire[wire.index('static unsigned checks;'):] +
                       globals_text + production + assertions, encoding='utf-8')
        c.write_text(wire[:wire.index('static unsigned checks;')].replace('static uint64_t clock_now;', 'uint64_t clock_now;'), encoding='utf-8')
        flags = ['-I' + str(runtime / 'include'), '-DPSX_MAX_PLAYERS=2', '-O2', '-UNDEBUG']
        for unit, out in [(str(c), work/'wire.o'), (str(runtime/'src/sio.c'), work/'sio.o')]:
            subprocess.run([args.cc, *flags, '-c', unit, '-o', str(out)], check=True)
        exe = work / 'native-mouse-host.exe'
        subprocess.run([args.cxx, *flags, '-DPSX_SDL3', str(cpp), str(work/'wire.o'),
                        str(work/'sio.o'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
