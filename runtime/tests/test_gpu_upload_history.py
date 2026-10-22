"""Exercise production upload-history capture across its retention limit."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import platform


p=argparse.ArgumentParser()
p.add_argument('--cc',default='gcc')
p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'src/gpu.c')
a=p.parse_args()
s=a.source.read_text(encoding='utf-8')
start=s.index('        /* Capture first few data words for debug */')
end=s.index('        /* Each word contains two',start)
capture=s[start:end]
harness='#include <assert.h>\n#include "'+a.source.resolve().as_posix()+'"\n#include "'+str(Path(__file__).resolve().parent/'gpu_exec_stubs.inc').replace('\\','/')+'"\n' +'\nstatic void capture(uint32_t val) {\n'+capture+'}\n'+r"""
int main(void) {
 gp0_cmd_buf[1]=7|(9u<<16);gp0_cmd_buf[2]=4|(1u<<16);
 for(int i=0;i<A0_HISTORY_CAP;i++) {
  gp0_exec_cpu_to_vram();assert(a0_capture_slot==i);
  capture(0x12345678);capture(0xabcdef01);
  assert(a0_history[i].word_count==2);
 }
 A0HistEntry retained[A0_HISTORY_CAP];memcpy(retained,a0_history,sizeof(retained));
 for(int upload=0;upload<16;upload++) {
  gp0_exec_cpu_to_vram();assert(a0_capture_slot==-1);
  assert(vram_write_remaining==2 && gp0_state==GP0_VRAM_WRITE);
  capture(0xdeadbeef);capture(0xdeadbeef);
 }
 assert(a0_history_count==A0_HISTORY_CAP && !memcmp(retained,a0_history,sizeof(retained)));
 /* Largest legal upload is bounded to 262144 data words in a fresh slot. */
 a0_history_count=0;gp0_cmd_buf[2]=0;gp0_exec_cpu_to_vram();
 assert(vram_write_remaining==262144);
 for(unsigned i=0;i<vram_write_remaining;i++)capture(i);
 assert(a0_history[0].word_count==262144);
 for(unsigned i=0;i<4;i++)assert(a0_history[0].first_words[i]==i);
 puts("PASS retained history freezes at capacity; uploads remain admitted; maximum upload count fits");
}
"""
with tempfile.TemporaryDirectory() as d:
 root=Path(d);c=root/'history.c';c.write_text(harness,encoding='utf-8')
 for opt in ('-O0','-O2'):
  exe=root/(opt+'.exe')
  subprocess.run([a.cc,opt,'-std=c11','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-Wno-unused-function',
                  '-I',str(a.source.resolve().parents[1]/'include'),
                  str(c),str(a.source.parent/'gpu_timeline.c'),str(a.source.parent/'psx_video_timing.c'),
                  '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                  '-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
