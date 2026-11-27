#!/usr/bin/env python3
"""Exercise production rollback PC selection with the V82 poisoned-pin context."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--revision', help='Read production functions from this Git revision')
    args = parser.parse_args()

    def read(path):
        if args.revision:
            return subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                            f'{args.revision}:{path}']).decode()
        return (ROOT / path).read_text(encoding='utf-8')

    picker = function(read('runtime/src/psx_netplay_rb.c'),
                      'static uint32_t pick_snap_resume_pc(')
    hint = function(read('runtime/src/psx_netplay.c'),
                    'uint32_t psx_netplay_snap_resume_hint(')
    verify = function(read('runtime/src/psx_netplay_rb.c'),
                      'static void enter_verify_at_tip(uint32_t done)\n{')
    verify = verify[:verify.index('    /* av/cd computed once')] + '    ++published;\n}\n'
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { uint32_t gpr[32], pc; } CPUState;
static uint32_t irq_pc, compiled_pc, dirty_pc, g_last_good_bb_pc;
static int boot, site, unsafe;
static int rb_resume_pc_ok(uint32_t pc) {
    return pc && !(pc&3u) && pc!=0x80000080u && pc!=0xbfc00180u;
}
static int psx_is_dispatchable(uint32_t pc) { return rb_resume_pc_ok(pc); }
static int rb_pc_is_bios(uint32_t pc) { return (pc&0xfff00000u)==0xbfc00000u; }
static uint32_t rb_canonicalize_resume_pc(uint32_t pc) { return pc; }
static int rb_boot_snap_tick(void) { return boot; }
static uint32_t psx_last_irq_check_pc(void) { return irq_pc; }
static uint32_t psx_compiled_irq_resume_pc(void) { return compiled_pc; }
static int psx_irq_resume_context_snapshot_site(void) { return site; }
static uint32_t psx_irq_resume_context_snapshot_pc(void) { return dirty_pc; }
static int psx_irq_resume_context_snapshot_safe_at(uint32_t pc) {
    return !unsafe && (!site || (site!=5 && pc==dirty_pc));
}
''' + picker + '\n' + hint + r'''
typedef struct { int unused; } RNetSession;
static RNetSession session;
static CPUState current;
static int owner, *g_rb=&owner, aborted, published;
static RNetSession *sess(void) { return &session; }
static CPUState *cpu(void) { return &current; }
static int pin_baseline_from_cpu(uint32_t tick) {
    (void)tick;return pick_snap_resume_pc(&current,0)!=0;
}
static void abort_episode_realign(const char *why) { (void)why;++aborted; }
''' + verify + r'''
int main(void) {
    CPUState c={0};
    /* Actual matched pin7075: registers/RA belong to 5DE3C, not terrain28AA8. */
    c.pc=0x8005de3cu;c.gpr[29]=0x801fff08u;c.gpr[30]=0x80070000u;
    c.gpr[31]=0x8005de3cu;irq_pc=g_last_good_bb_pc=0x80028aa8u;
    if(pick_snap_resume_pc(&c,psx_netplay_snap_resume_hint(&c))!=c.pc ||
       pick_snap_resume_pc(&c,0)!=c.pc) {
        fputs("FAIL: current CPU context replaced with stale terrain PC\n",stderr);
        return 5;
    }
    /* A known BB-edge hint and a published dirty transfer are authoritative. */
    assert(pick_snap_resume_pc(&c,0x80050000u)==0x80050000u);
    site=1;dirty_pc=0x80051000u;
    assert(psx_netplay_snap_resume_hint(&c)==dirty_pc);
    assert(pick_snap_resume_pc(&c,0x80050000u)==dirty_pc);
    site=5;assert(pick_snap_resume_pc(&c,0)==0); /* nested call-return */
    site=1;unsafe=1;assert(pick_snap_resume_pc(&c,0)==0); /* host/mod call */
    site=0;assert(pick_snap_resume_pc(&c,0)==0);
    current=c;enter_verify_at_tip(7075);
    assert(aborted==1&&published==0); /* unsafe state cannot authorize POST */
    unsafe=0;enter_verify_at_tip(7075);
    assert(aborted==1&&published==1);
    unsafe=0;c.pc=0;compiled_pc=0x80052000u;
    assert(pick_snap_resume_pc(&c,psx_netplay_snap_resume_hint(&c))==compiled_pc);
    /* A rematch boot snapshot cannot pick prior-match main-RAM residue. */
    boot=1;c.pc=0x8005de3cu;compiled_pc=0xbfc00100u;
    assert(pick_snap_resume_pc(&c,0)==compiled_pc);
    compiled_pc=0;assert(pick_snap_resume_pc(&c,0)==0);
    puts("production rollback current/dirty/parked/unsafe/boot resume controls PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-rb-resume-') as directory:
        source = Path(directory) / 'resume.c'
        exe = Path(directory) / 'resume-test.exe'
        source.write_text(fixture, encoding='utf-8')
        subprocess.run([args.compiler, '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-UNDEBUG', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
