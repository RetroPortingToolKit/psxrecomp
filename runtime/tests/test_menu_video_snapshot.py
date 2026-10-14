"""Execute production menu getters while the main thread publishes settings."""
import argparse, subprocess, tempfile
from pathlib import Path
from test_netplay_load_apply_flow import function_body

ap=argparse.ArgumentParser()
ap.add_argument('--cxx',required=True)
args=ap.parse_args()
runtime=Path(__file__).resolve().parents[1]
source=(runtime/'src/main.cpp').read_text(encoding='utf-8')
fixture=r'''
#include "host_overlay.h"
#include <mutex>
#include <atomic>
#include <thread>
#include <cassert>
#include <cstdio>
static std::mutex g_ovl_mu;
static PsxHostVideoSettings g_ovl_video_current{},g_ovl_video_boot{},g_video_boot{}, live{};
static bool g_ovl_netplay=false;
static std::atomic<int> netplay{0},wrong_thread_reads{0};
static std::thread::id main_id;
static bool g_ovl_video_pending=false;
static PsxHostVideoSettings g_ovl_video_post{};
#define PSX_MENU_RESTART_AFTER_MATCH (1u<<31)
static int psx_netplay_active() { return netplay.load(); }
extern "C" void psx_video_current(PsxHostVideoSettings *out) {
    if (std::this_thread::get_id()!=main_id) { ++wrong_thread_reads; return; }
    *out=live;
}
extern "C" unsigned psx_host_video_restart_bits(const PsxHostVideoSettings *b,const PsxHostVideoSettings *n) {
    return b->renderer!=n->renderer ? PSX_VIDEO_RESTART_RENDERER : 0;
}
extern "C" unsigned psx_host_video_live_changes(const PsxHostVideoSettings *b,const PsxHostVideoSettings *n) {
    return b->texture_filter!=n->texture_filter ? PSX_VIDEO_LIVE_TEXTURE_FILTER : 0;
}
'''
for signature in ['void mh_refresh_snapshot()', 'void mh_video_get(PsxHostVideoSettings* out)',
                  'unsigned mh_restart_bits(const PsxHostVideoSettings& next)',
                  'unsigned mh_video_apply(const PsxHostVideoSettings* next)']:
    fixture += signature+' {'+function_body(source,signature)+'}\n'
fixture += r'''
static PsxHostVideoSettings values(int v) { return {v,v,v,v,v,v,v,v,v,v,v,v,v}; }
int main() {
    main_id=std::this_thread::get_id();
    live=values(1); g_video_boot=values(1); mh_refresh_snapshot();
    std::atomic<int> reads{0};
    std::thread draw([&] {
        for (int i=0;i<50000;++i) {
            PsxHostVideoSettings got{};
            mh_video_get(&got);
            assert(got.fullscreen==got.texture_filter && got.texture_filter==got.renderer &&
              got.renderer==got.frame_generation && got.frame_generation==got.supersampling &&
              got.supersampling==got.render_thread && got.render_thread==got.present_thread);
            const auto next=values(2);
            const unsigned restart=mh_video_apply(&next);
            assert(restart & PSX_VIDEO_RESTART_RENDERER);
            ++reads;
        }
    });
    for (int i=0;i<50000;++i) { live=values(1+i%8); netplay=i%2; mh_refresh_snapshot(); }
    draw.join();
    assert(reads==50000 && wrong_thread_reads==0 && g_ovl_video_pending);
    puts("menu video snapshot: coherent draw-thread reads and main-thread ownership PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='psx-menu-snapshot-') as d:
    d=Path(d); cpp=d/'snapshot.cpp'; exe=d/'snapshot.exe'
    cpp.write_text(fixture,encoding='utf-8')
    subprocess.run([args.cxx,'-std=c++17','-O2','-pthread','-I'+str(runtime/'include'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
