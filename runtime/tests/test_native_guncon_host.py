"""Exercise the production pointer sampler through the real GunCon wire."""
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
    source = (runtime/'src/main.cpp').read_text(encoding='utf-8')
    start = source.index('static bool guncon_seat_configured(void) {')
    end = source.index('\n}\n', source.index('static void sample_guncon_ports(void) {',start))+3
    production = source[start:end]
    wire = (runtime/'tests/test_sio_guncon.c').read_text(encoding='utf-8')
    wire = wire[:wire.index('#ifndef SIO_DEVICE_GUNCON')]
    header = (runtime/'include/psx_keybinds.h').read_text(encoding='utf-8')
    enum_start = header.index('enum PsxGunconControl {')
    controls = header[enum_start:header.index('};',enum_start)+2]
    preamble = r'''
#include "sio.h"
#include "guncon_map.h"
#include <cstdio>
#include <cstring>
extern "C" { extern uint64_t clock_now; extern uint32_t i_stat; }
using SDL_Window=int;
using SDL_Cursor=int;
using Uint8=uint8_t;
static int window, mx=320, my=240, width=640, height=480;
static bool focused=true, menu=false, rewind_open=false, netplay=false;
static bool g_headless=false, g_mouse_captured=false;
static SDL_Window* sdl_window=&window;
struct PlayerInput { int kind=0; };
static PlayerInput g_players[PSX_MAX_PLAYERS];
static int savestate_menu_open=0, g_video_aspect_num=4, g_video_aspect_den=3;
static bool host_hotkey_input_focused() { return focused; }
static bool psx_host_ui_capture_active() { return menu; }
static bool psx_rewind_is_open() { return rewind_open; }
static bool psx_netplay_active() { return netplay; }
constexpr int SDL_SYSTEM_CURSOR_CROSSHAIR=1;
static SDL_Cursor* SDL_CreateSystemCursor(int) { return &window; }
static void SDL_SetCursor(SDL_Cursor*) {}
static SDL_Cursor* SDL_GetDefaultCursor() { return &window; }
static unsigned SDL_GetMouseState(float* x,float* y) { *x=(float)mx;*y=(float)my;return 0; }
static const Uint8* SDL_GetKeyboardState(int*) { static Uint8 keys[512];return keys; }
static void SDL_GetWindowSize(SDL_Window*,int* w,int* h) { *w=width;*h=height; }
static void gpu_get_crtc_debug(uint32_t* x1,uint32_t* x2,uint32_t* y1,uint32_t* y2,uint32_t* h1,uint32_t* h2) {
    *x1=600;*x2=3160;*y1=16;*y2=256;*h1=*h2=0;
}
static int psx_video_timing_is_pal() { return 0; }
static unsigned held_controls;
static int psx_keybinds_guncon_held(const Uint8*,int control) { return !!(held_controls&(1u<<control)); }
'''
    assertions = r'''
int main() {
    setup_bus();
    g_players[1].kind=4;
    sio_set_port_device(1,SIO_DEVICE_GUNCON);sio_set_pad_connected(1,1);
    held_controls=1u<<PSX_GC_TRIGGER;
    sample_guncon_ports();
    EXPECT(1,"production centre aim and trigger reach port 2",0xff,0x63,0x5a,0xff,0xdf,0x0d,1,0x88,0);
    focused=false;sample_guncon_ports();
    EXPECT(1,"focus loss releases gun and reports no light",0xff,0x63,0x5a,0xff,0xff,1,0,10,0);
    focused=true;menu=true;sample_guncon_ports();
    EXPECT(1,"host menu owns the pointer",0xff,0x63,0x5a,0xff,0xff,1,0,10,0);
    menu=false;netplay=true;sample_guncon_ports();
    EXPECT(1,"native gun disabled during netplay",0xff,0x63,0x5a,0xff,0xff,1,0,10,0);
    netplay=false;g_mouse_captured=true;sample_guncon_ports();
    EXPECT(1,"relative Mouse capture excludes absolute gun",0xff,0x63,0x5a,0xff,0xff,1,0,10,0);
    g_mouse_captured=false;held_controls=1u<<PSX_GC_OFFSCREEN_SHOT;sample_guncon_ports();
    EXPECT(1,"offscreen reload keeps trigger with no light",0xff,0x63,0x5a,0xff,0xdf,1,0,10,0);
    held_controls=0;width=1280;height=720;mx=1;my=360;sample_guncon_ports();
    EXPECT(1,"pointer in pillarbox reports no light",0xff,0x63,0x5a,0xff,0xff,1,0,10,0);
    check(sio_get_port_device(0)==SIO_DEVICE_PAD,"other physical port stays a pad");
    std::printf("PASS: GunCon production host sampling through SIO, %u checks\n",checks);
}
'''
    with tempfile.TemporaryDirectory(prefix='native-guncon-host-') as tmp:
        work=Path(tmp)
        c=work/'wire.c'
        c.write_text(wire[:wire.index('static unsigned checks;')].replace('static uint64_t clock_now;', 'uint64_t clock_now;'),encoding='utf-8')
        cpp=work/'host.cpp'
        cpp.write_text(preamble+controls+wire[wire.index('static unsigned checks;'):]+production+assertions,encoding='utf-8')
        flags=['-I'+str(runtime/'include'),'-DPSX_MAX_PLAYERS=2','-O2','-UNDEBUG']
        for unit,obj in [(c,work/'wire.o'),(runtime/'src/sio.c',work/'sio.o')]:
            subprocess.run([args.cc,*flags,'-c',str(unit),'-o',str(obj)],check=True)
        exe=work/'host.exe'
        subprocess.run([args.cxx,*flags,'-DPSX_SDL3',str(cpp),str(work/'wire.o'),str(work/'sio.o'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)


if __name__ == '__main__':
    main()
