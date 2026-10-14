"""Execute the production Verify deadline branch with a controlled clock."""
import argparse, subprocess, tempfile
from pathlib import Path
from test_netplay_load_apply_flow import function_body
ap=argparse.ArgumentParser();ap.add_argument('--compiler',required=True);args=ap.parse_args()
runtime=Path(__file__).resolve().parents[1]
source=(runtime/'src/psx_netplay_rb.c').read_text(encoding='utf-8')
start=source.index('        if (g_verify_wait_ms == 0ull)',source.index('/* Retransmit POST while Verify waits for peer'))
end=source.index('\n        }',source.index('abort_episode_realign("verify timeout (peer POST missing)");',start))+len('\n        }')
timer=source[start:end]
helper=function_body(source,'static int rb_owing_seats_replaying(uint32_t owing, uint32_t cap)')
fixture=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define RNET_RB_QUORUM_SEATS 4
#define RB_VERIFY_TIMEOUT_MS 1000u
typedef uint32_t rnet_u32;
typedef struct { uint32_t tip[4]; } RNetSession;
static RNetSession session;
static RNetSession *sess(void) { return &session; }
static int rnet_session_remote_tip(RNetSession *s,int seat,rnet_u32 *tip) { *tip=s->tip[seat]; return 1; }
static uint32_t g_peer_progress_tip[4],missing=2,target=200;
static uint64_t g_verify_wait_ms;
static int aborts;
static void *g_rb,*g_post_q;
static uint32_t rnet_rb_quorum_missing(void *q) { (void)q;return missing; }
static uint32_t rnet_rb_get_target_tick(void *r) { (void)r;return target; }
static void abort_episode_realign(const char *why) { (void)why;++aborts; }
'''
fixture+='static int rb_owing_seats_replaying(uint32_t owing,uint32_t cap) {'+helper+'}\n'
fixture+='static void verify_timer(uint64_t now) {'+timer+'}\n'
fixture+=r'''
int main(void) {
    session.tip[1]=100;
    verify_timer(100); assert(g_verify_wait_ms==100);
    verify_timer(200); assert(g_verify_wait_ms==100); /* first observation */
    session.tip[1]=150; verify_timer(900); assert(g_verify_wait_ms==900);
    session.tip[1]=200; verify_timer(1800); assert(g_verify_wait_ms==1800);
    for (uint64_t now=1900;now<2800;now+=100) {
        session.tip[1]++; verify_timer(now); assert(g_verify_wait_ms==1800 && aborts==0);
    }
    session.tip[1]++; verify_timer(2800); assert(aborts==1 && g_verify_wait_ms==1800);
    /* One genuinely replaying owing seat can extend the wait; unrelated seats
     * cannot. Once every owing seat is beyond the cap, the deadline applies. */
    aborts=0;g_verify_wait_ms=3000;missing=6;
    g_peer_progress_tip[2]=100;session.tip[2]=101;verify_timer(3800);
    assert(g_verify_wait_ms==3800 && !aborts);
    session.tip[2]=250; session.tip[0]=10000;verify_timer(4700);
    assert(g_verify_wait_ms==3800 && !aborts);
    verify_timer(4800);assert(aborts==1);
    puts("Verify deadline: capped replay progress, live lost-POST wait and multiple owing seats PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='psx-verify-deadline-') as d:
    d=Path(d);c=d/'verify.c';exe=d/'verify.exe';c.write_text(fixture,encoding='utf-8')
    subprocess.run([args.compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
