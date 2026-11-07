"""Run real RFE host checks with observable guest-read and pure-peek paths."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(text, name):
    found = re.search(r'^[\w *]+\b'+name+r'\([^;]*?\)\s*\{', text, re.M)
    if not found:
        raise ValueError('function not found: '+name)
    return text[found.start():text.index('\n}', found.end())+2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--src-root', type=Path,
                        default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    irq = (args.src_root/'src/interrupts.c').read_text(encoding='utf-8-sig')
    traps = (args.src_root/'src/traps.c').read_text(encoding='utf-8-sig')
    check = function(irq, 'rfe_outside_window_unwind')
    resume = function(traps, 'psx_scheduler_rfe_resume')
    fixture = r'''
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef struct { uint32_t pc; } CPUState;
static int g_call_unit_depth, g_psx_dispatch_depth, g_dirty_interp_active;
static uint32_t s_compiled_interrupt_resume_pc;
static unsigned guest_reads, peeks, flushes, logged, failures;
static int allowed, shadow, replay, mismatch;
static const uint32_t pcb=0xE000, tcb=0xE100, target=0x80020000;
static jmp_buf g_scheduler_jmpbuf;
static struct { uint32_t target_tcb,resume_pc; int reason; } g_sched_escape;
enum { PSX_RUN_RESUME_CURRENT=3 };
static uint32_t host_word(uint32_t address) {
    if (address==0x108u) return pcb;
    if (address==pcb) return tcb;
    if (address==tcb+0x88u) return mismatch ? target+4u : target;
    return 0;
}
uint32_t psx_read_word(uint32_t address) {
    ++guest_reads;
    return replay ? 0xDEADBEEFu : host_word(address);
}
int psx_peek_word(uint32_t address,uint32_t *out) {
    ++peeks; *out=host_word(address); return 1;
}
static int same_guest_pc(uint32_t a,uint32_t b) { return a==b; }
static int psx_scheduler_can_resume_at(uint32_t pc) { return allowed && pc==target; }
int overlay_loader_shadow_native_thread_switch_bail(void) { return shadow; }
static void psx_cyc_batch_flush(void) { ++flushes; }
static uint32_t psx_current_tcb_ptr(CPUState *cpu) {
    (void)cpu; return psx_read_word(psx_read_word(0x108u));
}
static void sched_escape_ring_log(CPUState *cpu,int reason,uint32_t current,
                                 uint32_t next,uint32_t pc) {
    (void)cpu;
    if (reason!=PSX_RUN_RESUME_CURRENT || current!=tcb || next || pc!=target) ++failures;
    ++logged;
}
RESUME
CHECK
static void run_case(int active,int nested,int replaying,int wrong_pc,int bail) {
    CPUState cpu={0}; cpu.pc=target;
    allowed=active;g_psx_dispatch_depth=nested?2:1;g_call_unit_depth=0;
    replay=replaying;mismatch=wrong_pc;shadow=bail;
    guest_reads=peeks=flushes=logged=0;
    int escaped=0;
    if (!setjmp(g_scheduler_jmpbuf)) rfe_outside_window_unwind(&cpu);
    else escaped=1;
    if (guest_reads) {
        fprintf(stderr,"host checks consumed guest stream: %u reads\n",guest_reads);++failures;
    }
    if ((!active || !nested) && peeks) {
        fprintf(stderr,"inactive or flat guard inspected RAM\n");++failures;
    }
    int expected=active && nested && !wrong_pc && !bail;
    if (escaped!=expected || logged!=(unsigned)expected || flushes!=(unsigned)expected) {
        fprintf(stderr,"wrong escape/log/flush decision\n");++failures;
    }
    if (expected && (g_sched_escape.resume_pc!=target ||
        g_sched_escape.reason!=PSX_RUN_RESUME_CURRENT || g_sched_escape.target_tcb)) ++failures;
}
int main(void) {
    run_case(0,1,0,0,0); run_case(1,0,0,0,0);
    run_case(1,1,0,0,0); run_case(1,1,1,0,0);
    run_case(1,1,1,1,0); run_case(1,1,1,0,1);
    printf("six host inspection controls, %u failures\n",failures);
    return failures ? 1 : 0;
}
'''
    fixture = fixture.replace('\nRESUME\n', '\n'+resume+'\n').replace('\nCHECK\n', '\n'+check+'\n')
    options = dict(stdin=subprocess.DEVNULL, capture_output=True, text=True,
                   encoding='utf-8', errors='replace', timeout=60)
    with tempfile.TemporaryDirectory() as directory:
        unit=Path(directory)/'host_reads.c'
        exe=Path(directory)/'host_reads.exe'
        unit.write_text(fixture,encoding='utf-8')
        for opt in ['-O0','-O2']:
            result=subprocess.run([args.cc,'-std=c11',opt,str(unit),'-o',str(exe)],**options)
            if result.returncode:
                raise RuntimeError(result.stdout+result.stderr)
            result=subprocess.run([str(exe)],**options)
            print(opt,result.stdout,end='')
            if result.stderr:print(result.stderr,end='')
            if result.returncode:return result.returncode
    return 0


if __name__=='__main__':
    raise SystemExit(main())
