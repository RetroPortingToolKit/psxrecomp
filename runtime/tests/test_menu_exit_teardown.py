"""Run the production teardown against a late, real overlay draw callback."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_netplay_load_apply_flow import function_body

parser = argparse.ArgumentParser()
parser.add_argument('--cc', required=True)
parser.add_argument('--cxx', required=True)
parser.add_argument('--main-source', type=Path)
args = parser.parse_args()
runtime = Path(__file__).resolve().parents[1]
source = (args.main_source or runtime/'src/main.cpp').read_text(encoding='utf-8')
fixture = r'''
#include "psx_menu_host.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#c); std::exit(1); } } while(0)
static const PsxMenuProvider* g_ovl = nullptr;
static bool g_ovl_on, g_ovl_pause_held;
static std::atomic<int> g_ovl_pause_wanted{0}, g_ovl_exit{0};
static bool fallback;
static int detach_calls;
static bool detach_posts;
static std::atomic<bool> entered{false}, release_draw{false}, draw_finished{false};
extern "C" void psx_runtime_menu_set_fallback_enabled(int on) { fallback = on != 0; }
'''
for signature in ['void mh_request_exit(int how)', 'static void recomp_overlay_detach(void)']:
    fixture += signature + ' {' + function_body(source, signature+' {') + '}\n'
fixture += r'''
static void detached(void*) {
    CHECK(!psx_host_overlay_has_draw());
    CHECK(!psx_host_ui_capture_active());
    CHECK(psx_host_pause_depth() == 0);
    CHECK(draw_finished.load());
    ++detach_calls;
    if (detach_posts) mh_request_exit(PSX_MENU_EXIT_TO_FRONTEND);
}
static void late_draw(int, int, void* context) {
    entered = true;
    while (!release_draw.load()) std::this_thread::yield();
    if (context) mh_request_exit(PSX_MENU_EXIT_TO_FRONTEND);
    draw_finished = true;
}
static void run_case(bool late_request, bool detach_request) {
    PsxMenuProvider provider{};
    provider.abi_version = PSX_MENU_ABI_VERSION;
    provider.detach = detached;
    g_ovl = &provider;
    g_ovl_on = true;
    g_ovl_exit = 0;
    detach_posts = detach_request;
    detach_calls = 0;
    fallback = false;
    entered = release_draw = draw_finished = false;
    psx_host_set_ui_capture(1);
    g_ovl_pause_held = psx_host_pause_push("test menu") != 0;
    g_ovl_pause_wanted = 1;
    psx_host_overlay_set_draw_cb(late_draw, late_request ? &provider : nullptr);
    std::thread draw([] { psx_host_overlay_draw(640, 480); });
    while (!entered.load()) std::this_thread::yield();
    // The runtime consumes the first exit while a provider draw is in flight.
    mh_request_exit(PSX_MENU_EXIT_TO_FRONTEND);
    CHECK(g_ovl_exit.exchange(0) == PSX_MENU_EXIT_TO_FRONTEND + 1);
    // The old draw posts after consumption. Production teardown must fence it
    // before disposing of this session's pending transition.
    release_draw = true;
    recomp_overlay_detach();
    draw.join();
    CHECK(g_ovl_exit.exchange(0) == 0);
    CHECK(detach_calls == 1 && !g_ovl && !g_ovl_on && fallback);
    CHECK(g_ovl_pause_wanted == 0 && !g_ovl_pause_held);
    recomp_overlay_detach();
    CHECK(detach_calls == 1);
    // A valid request owned by the next attached session must still work.
    g_ovl = &provider;
    g_ovl_on = true;
    detach_posts = false;
    mh_request_exit(PSX_MENU_EXIT_RELAUNCH);
    CHECK(g_ovl_exit.exchange(0) == PSX_MENU_EXIT_RELAUNCH + 1);
    recomp_overlay_detach();
    CHECK(detach_calls == 2 && g_ovl_exit == 0);
}
int main() {
    run_case(true, false);
    run_case(false, true);
    run_case(true, true);
    std::puts("menu exit teardown: late draw/detach requests discarded; next-session request and pause cleanup PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='psx-menu-exit-') as directory:
    directory = Path(directory)
    cpp, obj, exe = directory/'fixture.cpp', directory/'overlay.o', directory/'fixture.exe'
    cpp.write_text(fixture, encoding='utf-8')
    subprocess.run([args.cc, '-std=c11', '-O2', '-I'+str(runtime/'include'),
                    '-c', str(runtime/'src/host_overlay.c'), '-o', str(obj)], check=True)
    subprocess.run([args.cxx, '-std=c++17', '-O2', '-pthread', '-I'+str(runtime/'include'),
                    str(cpp), str(obj), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
