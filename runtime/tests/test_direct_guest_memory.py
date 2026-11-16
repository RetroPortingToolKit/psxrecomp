"""Exercise native data accesses against production memory/observer code."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

import test_memory_debug_trace_guard as memory_fixture


def remove_function(source, name):
    match = re.search(r"\n(?:void|int) " + name + r"\([^;]*?\)\s*\{", source, re.S)
    assert match, name
    a = match.start()
    brace = source.index("{", a)
    depth = 1
    b = brace + 1
    while depth:
        depth += (source[b] == "{") - (source[b] == "}")
        b += 1
    return source[:a] + "\n" + source[b:]


def check_observer_dispatch(root, work, cc):
    """Exercise the real debug-server bodies at the existing recorder seams."""
    source = (root / "runtime/src/debug_server.c").read_text(encoding="utf-8")

    def body(name):
        start = source.rfind("\n", 0, source.index(name + "(")) + 1
        brace = source.index("{", start)
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]

    preamble = r'''
#include "cpu_state.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define PSX_DEBUG_COLD
#define REC_KIND_RAM_W 2
#define REC_KIND_SP_W 12
static CPUState cpu;
CPUState *debug_cpu_ptr=&cpu;
static int s_fmv_quiet,g_psx_render_pass_active,s_fp;
static int s_cyc_watch_armed,s_pc_probe_armed,ls_armed;
static unsigned events[32],n,watch_entries,probe_entries,ls_calls,nd_reads,boot_total;
static uint32_t g_psx_last_block,g_debug_last_store_pc;
static void *s_wtrace_trans,*s_wtrace_boot,*s_wtrace;
static int s_wtrace_trans_range_count,s_wtrace_boot_range_count,s_wtrace_range_count;
static struct {uint32_t lo,hi;} s_wtrace_ranges[2];
static void event(unsigned e){assert(n<32);events[n++]=e;}
uint32_t psx_ram_live_bytes(void){return 0x200000;}
static int psx_fp_write_eligible(uint32_t p,uint32_t size){return p<size;}
static void psx_fp_note_quiet(int *p){(void)p;event(10);}
static int is_card_critical_addr(uint32_t p){return p==0x100;}
static void card_trace_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)p;assert(o==3&&v==9&&w==4);event(0);}
static void fp_record_write(uint32_t p,uint32_t v,uint32_t pc){(void)p;(void)v;(void)pc;event(1);}
static void rec_event(uint8_t kind,uint32_t p,uint32_t v,uint32_t pc,uint32_t ra){(void)p;(void)v;(void)pc;(void)ra;event(kind);}
static void wtrace_all_record(uint32_t p,uint32_t v,uint8_t w){(void)p;(void)v;(void)w;event(3);}
static void wtrace_transition_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)p;assert(o!=v&&w==4);event(4);}
static void wtrace_boot_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)p;(void)o;(void)v;(void)w;boot_total++;event(5);}
static void wtrace_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)p;(void)o;(void)v;(void)w;event(6);}
static void cyc_watch_observe(uint32_t p){(void)p;watch_entries++;if(s_cyc_watch_armed)event(7);}
static void pc_probe_observe(uint32_t p){(void)p;probe_entries++;if(s_pc_probe_armed)event(8);}
void ls_at_leader(uint32_t p,CPUState *c){assert(p==g_psx_last_block&&c==&cpu);ls_calls++;if(ls_armed)event(9);}
uint32_t psx_read_word(uint32_t p){(void)p;nd_reads++;return 0;}
void psx_host_write_word(uint32_t p,uint32_t v){(void)p;(void)v;}
void psx_nd_note_wood_batch_ot_tagged(uint32_t p,uint32_t t){(void)p;(void)t;}
void psx_nd_note_wood_batch_ot(uint32_t p){(void)p;}
'''
    cases = r'''
int main(void){
 debug_server_trace_write_check(0x200,3,9,4);
 assert(n==3&&events[0]==1&&events[1]==2&&events[2]==3);
 s_wtrace_trans=s_wtrace_boot=s_wtrace=(void*)1;
 s_wtrace_trans_range_count=s_wtrace_boot_range_count=s_wtrace_range_count=1;
 s_wtrace_ranges[0].lo=0x100;s_wtrace_ranges[0].hi=0x200;n=0;
 debug_server_trace_write_check(0x100,3,9,4);
 assert(n==7);for(unsigned i=0;i<7;i++)assert(events[i]==i);
 n=0;debug_server_trace_write_check(0x110,9,9,4);
 assert(n==5&&events[0]==1&&events[1]==2&&events[2]==3&&events[3]==5&&events[4]==6);
 assert(boot_total==2); /* matching totals must survive a full boot ring */
 n=0;g_psx_render_pass_active=1;debug_server_trace_write_check(0x100,3,9,4);assert(n==0);
 g_psx_render_pass_active=0;s_fmv_quiet=1;debug_server_trace_write_check(0x100,3,9,4);assert(n==1&&events[0]==10);
 s_fmv_quiet=0;s_wtrace_trans=s_wtrace_boot=s_wtrace=NULL;n=0;
 debug_server_trace_write_check(0x1F800000,3,9,4);assert(n==3&&events[1]==REC_KIND_SP_W);
 n=0;debug_server_cyc_observe(0x80012340);assert(g_psx_last_block==0x12340&&ls_calls==1&&!watch_entries&&!probe_entries);
 ls_armed=1;debug_server_cyc_observe(0x80012344);assert(n==1&&events[0]==9&&ls_calls==2);
 s_cyc_watch_armed=s_pc_probe_armed=1;n=0;debug_server_cyc_observe(0x80012348);
 assert(n==3&&events[0]==7&&events[1]==8&&events[2]==9);
 s_cyc_watch_armed=s_pc_probe_armed=ls_armed=0;cpu.gpr[31]=0x100;n=0;
 debug_server_cyc_observe(0x8006AAF0);assert(nd_reads==2&&watch_entries==2&&probe_entries==2);
 s_fmv_quiet=1;unsigned before=ls_calls;debug_server_cyc_observe(0x80012350);
 assert(g_psx_last_block==0x12350&&ls_calls==before);
 puts("PASS actual observer routing: mandatory rings/order, optional live flags, ND and first lockstep leader");
}
'''
    actual = "\n".join(body(name) for name in (
        "debug_server_cyc_observe_optional", "debug_server_cyc_observe",
        "debug_server_trace_write_optional", "debug_server_trace_write_check"))
    path = work / "existing-observer-dispatch.c"
    path.write_text(preamble + actual + cases, encoding="utf-8")
    for opt in ("-O0", "-O2"):
        exe = work / ("observer.exe" if os.name == "nt" else "observer")
        subprocess.run([cc, "-std=c11", opt, "-UNDEBUG", "-Werror",
                        "-I", str(root / "runtime/include"), str(path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=memory_fixture.default_cc())
    parser.add_argument("--headroom-disabled", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    # Reuse the existing production-memory fixture. Link the real parity and
    # card modules rather than replacing their arming/provenance semantics.
    stubs = remove_function(memory_fixture.STUBS_C, "parity_trace_note_write")
    stubs = remove_function(stubs, "card_data_writes_check")
    stubs = stubs.replace("void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) {\n"
                          "    (void)biu; (void)addr; (void)value; abort();\n}",
                          "unsigned g_fixture_isc;\n"
                          "void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) {\n"
                          "    (void)biu; (void)addr; (void)value; ++g_fixture_isc;\n}")
    assert "unsigned g_fixture_isc" in stubs
    with tempfile.TemporaryDirectory(prefix="psx-direct-memory-") as td:
        work = Path(td)
        (work / "harness_common.h").write_text(memory_fixture.HARNESS_COMMON_H, encoding="utf-8")
        (work / "stubs.c").write_text(stubs, encoding="utf-8")
        trace = memory_fixture.TRACE_COUNTING_C.replace(
            'void debug_server_trace_write_check(',
            'void fixture_note_trace(uint32_t, uint32_t, uint32_t, uint8_t);\n'
            'void debug_server_trace_write_check(').replace(
            '    (void)phys;',
            '    fixture_note_trace(phys, old_val, new_val, width);')
        assert 'fixture_note_trace(phys, old_val, new_val, width);' in trace
        (work / "trace.c").write_text(trace, encoding="utf-8")
        policies = ((1, True), (1, False)) if args.headroom_disabled else ((0, True), (1, True), (1, False))
        for direct, production in policies:
            for opt in ("-O0", "-O2"):
                exe = work / ("fixture.exe" if os.name == "nt" else "fixture")
                argv = [args.cc, "-std=c11", opt, "-ffunction-sections", "-fdata-sections",
                        "-DPSX_NATIVE_CPU_SPANS=1", "-DPSX_DIRECT_GUEST_MEMORY=" + str(direct),
                        "-I", str(work), "-I", str(root / "runtime/include")]
                if production:
                    argv += ["-DPSX_NO_DEBUG_TOOLS=1"]
                if args.headroom_disabled:
                    argv += ["-DPSX_DISABLE_HEADROOM_COUNTERS=1"]
                argv += [str(root / "runtime/tests/test_direct_guest_memory.c")]
                argv += [str(root / "runtime/src" / name) for name in
                         ("memory.c", "psx_ram_geometry.c", "kernel_patch_ranges.c",
                          "parity_trace.c", "card_data_writes.c")]
                argv += [str(work / "stubs.c"), str(work / "trace.c"),
                         "-Wl,--gc-sections", "-o", str(exe)]
                subprocess.run(argv, check=True)
                subprocess.run([str(exe)], check=True)
        if not args.headroom_disabled:
            check_observer_dispatch(root, work, args.cc)
    print("PASS: actual canonical/native data access and dynamic observers (O0/O2, production/debug)")


if __name__ == "__main__":
    main()
