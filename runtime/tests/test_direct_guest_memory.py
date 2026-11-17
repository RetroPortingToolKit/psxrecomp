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

    # Range commands and init mutate only these counts, including assigning the
    # same count after replacing bounds. Each site must invalidate derived pages.
    import re
    mutations = list(re.finditer(
        r"(?m)^[ \t]*(?:(?:int slot = )?s_wtrace_(?:boot_|trans_)?range_count"
        r"(?:\+\+|--| = [^;\n]+);[^\n]*)$", source))
    assert len(mutations) == 14
    for mutation in mutations:
        assert source[mutation.end():].splitlines()[1].strip() == \
            "debug_server_write_interest_invalidate();"

    # Publish the fast gate at actual watch/LS owner transitions, never from
    # g_ls_mode or listener presence. Keep every mutation covered by this seam.
    interp = (root / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
    header = (root / "runtime/include/cpu_state.h").read_text(encoding="utf-8")
    assert "#define PSX_DEBUG_BLOCK_OBSERVE(pc) debug_server_cyc_observe(pc)" in header
    assert "#define PSX_DEBUG_BLOCK_OBSERVE(pc) ((void)0)" in header
    for emitter in ("code_generator.cpp", "full_function_emitter.cpp"):
        generated = (root / "recompiler/src" / emitter).read_text(encoding="utf-8")
        assert "PSX_DEBUG_BLOCK_OBSERVE(0x{:08X}u);" in generated
    for text, pattern, refresh, expected in (
        (source, r"(?m)^[ \t]*s_(?:cyc_watch|pc_probe)_armed = [01];[^\n]*$",
         "debug_server_watch_interest_refresh();", 9),
        (interp, r"(?m)^[ \t]*s_lsf_active = [01];[^\n]*$",
         "ls_debug_block_interest_refresh();", 3),
    ):
        changes = list(re.finditer(pattern, text))
        assert len(changes) == expected, (pattern, len(changes))
        for change in changes:
            assert text[change.end():].splitlines()[1].strip() == refresh

    def owner_body(name):
        match = re.search(r"(?m)^(?:static )?void " + name + r"\([^;]*?\)\s*\{", interp)
        assert match, name
        start = match.start()
        brace = interp.index("{", start)
        end, depth = brace + 1, 1
        while depth:
            depth += (interp[end] == "{") - (interp[end] == "}")
            end += 1
        return interp[start:end]

    preamble = r'''
#include "cpu_state.h"
#include "psx_memory.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PSX_DEBUG_COLD
#define REC_KIND_RAM_W 2
#define REC_KIND_SP_W 12
static CPUState cpu;
CPUState *debug_cpu_ptr=&cpu;
static int s_fmv_quiet,g_psx_render_pass_active,s_fp;
static int64_t s_rec_frame=0;
static int s_cyc_watch_armed,s_pc_probe_armed,ls_armed;
static unsigned events[32],n,watch_entries,probe_entries,ls_calls,nd_reads,boot_total;
volatile uint32_t g_psx_last_block,g_psx_debug_block_interest;
static uint32_t g_debug_last_store_pc;
static void *s_wtrace_trans,*s_wtrace_boot,*s_wtrace;
static int s_wtrace_trans_range_count,s_wtrace_boot_range_count,s_wtrace_range_count;
typedef struct {uint32_t lo,hi;} FixtureRange;
static FixtureRange s_wtrace_ranges[2],s_wtrace_boot_ranges[2],s_wtrace_trans_ranges[2];
static int boot_full;
static unsigned boot_stored;
static int in_ranges(const FixtureRange *ranges,int count,uint32_t p){
 for(int i=0;i<count;i++)if(p>=ranges[i].lo&&p<ranges[i].hi)return 1;
 return 0;
}
static void event(unsigned e){assert(n<32);events[n++]=e;}
uint32_t g_psx_ram_size=0x200000u;
static int psx_fp_write_eligible(uint32_t p,uint32_t size){return p<size;}
static void psx_fp_note_quiet(int *p){(void)p;event(10);}
static int is_card_critical_addr(uint32_t p){return p==0x100;}
static void card_trace_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)p;assert(o==3&&v==9&&w==4);event(0);}
static void fp_record_write(uint32_t p,uint32_t v,uint32_t pc){(void)p;(void)v;(void)pc;event(1);}
static void rec_event(uint8_t kind,uint32_t p,uint32_t v,uint32_t pc,uint32_t ra){(void)p;(void)v;(void)pc;(void)ra;event(kind);}
static void wtrace_all_record(uint32_t p,uint32_t v,uint8_t w){(void)p;(void)v;(void)w;event(3);}
static void wtrace_transition_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){assert(o!=v&&w==4);if(in_ranges(s_wtrace_trans_ranges,s_wtrace_trans_range_count,p))event(4);}
static void wtrace_boot_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)o;(void)v;(void)w;if(!in_ranges(s_wtrace_boot_ranges,s_wtrace_boot_range_count,p))return;boot_total++;if(!boot_full){boot_stored++;event(5);}}
static void wtrace_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)p;(void)o;(void)v;(void)w;event(6);}
static void cyc_watch_observe(uint32_t p){(void)p;watch_entries++;if(s_cyc_watch_armed)event(7);}
static void pc_probe_observe(uint32_t p){(void)p;probe_entries++;if(s_pc_probe_armed)event(8);}
static void (*fixture_ls_owner)(uint32_t,CPUState*);
void ls_at_leader(uint32_t p,CPUState *c){assert(p==g_psx_last_block&&c==&cpu);ls_calls++;if(ls_armed)event(9);if(fixture_ls_owner)fixture_ls_owner(p,c);}
static unsigned sreg_fixture_mode,sreg_word_reads,sreg_half_reads,sreg_effects,sreg_read_n;
static uint32_t sreg_reads[32],sreg_root=0x80000100u;
int g_ls_mode,g_ls_replay_active,g_ram_read_watch_active,g_dma_exec_depth,g_host_store_depth;
volatile int g_ds_recording;
static uint32_t sreg_sr;
const uint32_t *g_psx_memory_sr_ptr=&sreg_sr;
void (*g_overlay_flush_pending_cycles)(void);
static void sreg_foreign_owner(void){}
static int sreg_observed(void){return g_ls_mode||g_ls_replay_active||g_ram_read_watch_active||g_ds_recording;}
uint32_t psx_read_word(uint32_t p){
 nd_reads++;if(!sreg_fixture_mode)return 0;
 assert(sreg_read_n<32);sreg_reads[sreg_read_n++]=p;sreg_word_reads++;if(sreg_observed())sreg_effects++;
 if(p==0x108u)return sreg_root;
 if(p==0x80000100u)return 0x8000E100u;
 if(p==0x1F8001D4u)return 0x80020000u;
 if(p==0x80020000u)return 0x12345678u;
 return p^0x13579BDFu;
}
uint16_t psx_read_half(uint32_t p){assert(sreg_read_n<32);sreg_reads[sreg_read_n++]=p;sreg_half_reads++;if(sreg_observed())sreg_effects++;return (uint16_t)(p^0xABCDu);}
static uint32_t sreg_custom_word(uint32_t p){return psx_read_word(p);}
static uint16_t sreg_custom_half(uint32_t p){return psx_read_half(p);}
static uint32_t trace_read_word(CPUState *c,uint32_t p){assert(c==&cpu);return psx_read_word(p);}
static uint32_t trace_read_half(CPUState *c,uint32_t p){assert(c==&cpu);return psx_read_half(p);}
static void sreg_reset_reads(void){sreg_word_reads=sreg_half_reads=sreg_effects=sreg_read_n=0;}

void psx_host_write_word(uint32_t p,uint32_t v){(void)p;(void)v;}
void psx_nd_note_wood_batch_ot_tagged(uint32_t p,uint32_t t){(void)p;(void)t;}
void psx_nd_note_wood_batch_ot(uint32_t p){(void)p;}
'''
    cases = r'''
int main(void){
 debug_server_trace_write_check(0x200,3,9,4);
 assert(n==3&&events[0]==1&&events[1]==2&&events[2]==3);
 s_rec_frame=-1;n=0;debug_server_trace_write_check(0x200,3,9,4);
 assert(n==2&&events[0]==1&&events[1]==3);s_rec_frame=0;n=0;
 s_wtrace_trans=s_wtrace_boot=s_wtrace=(void*)1;
 s_wtrace_trans_range_count=s_wtrace_boot_range_count=s_wtrace_range_count=1;
 s_wtrace_ranges[0]=(FixtureRange){0x100,0x200};
 s_wtrace_boot_ranges[0]=s_wtrace_trans_ranges[0]=s_wtrace_ranges[0];
 debug_server_write_interest_invalidate();n=0;
 debug_server_trace_write_check(0x100,3,9,4);
 assert(n==7);for(unsigned i=0;i<7;i++)assert(events[i]==i);
 n=0;debug_server_trace_write_check(0x110,9,9,4);
 assert(n==5&&events[0]==1&&events[1]==2&&events[2]==3&&events[3]==5&&events[4]==6);
 assert(boot_total==2&&boot_stored==2);
 boot_full=1;n=0;debug_server_trace_write_check(0x110,9,9,4);
 assert(boot_total==3&&boot_stored==2&&n==4&&events[3]==6);
 boot_full=0;
 n=0;g_psx_render_pass_active=1;debug_server_trace_write_check(0x100,3,9,4);assert(n==0);
 g_psx_render_pass_active=0;s_fmv_quiet=1;debug_server_trace_write_check(0x100,3,9,4);assert(n==1&&events[0]==10);
 s_fmv_quiet=0;s_wtrace_trans=s_wtrace_boot=s_wtrace=NULL;
 debug_server_write_interest_invalidate();n=0;
 debug_server_trace_write_check(0x1F800000,3,9,4);assert(n==3&&events[1]==REC_KIND_SP_W);
 /* Conservative pages only route: exact half-open ranges still decide. */
 s_wtrace_trans=s_wtrace_boot=s_wtrace=(void*)1;
 s_wtrace_range_count=2;s_wtrace_boot_range_count=s_wtrace_trans_range_count=1;
 s_wtrace_ranges[0]=(FixtureRange){0x1fff,0x2001};
 s_wtrace_ranges[1]=(FixtureRange){0x2000,0x2004};
 s_wtrace_trans_ranges[0]=(FixtureRange){0x2000,0x2004};
 s_wtrace_boot_ranges[0]=(FixtureRange){0x2ffe,0x3000};
 debug_server_write_interest_invalidate();
 assert(debug_server_write_interest_lookup(0x1000)==4);
 assert(debug_server_write_interest_lookup(0x2000)==7);
 assert(debug_server_write_interest_lookup(0x3000)==0);
 n=0;debug_server_trace_write_check(0x1ffe,3,9,4);assert(n==3);
 n=0;debug_server_trace_write_check(0x1fff,3,9,4);assert(n==4&&events[3]==6);
 n=0;debug_server_trace_write_check(0x2000,3,9,4);
 assert(n==5&&events[3]==4&&events[4]==6); /* overlap records once */
 n=0;debug_server_trace_write_check(0x2004,3,9,4);assert(n==3);
 n=0;debug_server_trace_write_check(0x2fff,3,9,4);assert(n==4&&events[3]==5);
 n=0;debug_server_trace_write_check(0x3000,3,9,4);assert(n==3);
 /* Same-count replacement and deletion/compaction must take effect now. */
 s_wtrace_ranges[0]=(FixtureRange){0x7000,0x7100};
 s_wtrace_ranges[1]=(FixtureRange){0x70f0,0x7200};
 debug_server_write_interest_invalidate();
 assert(!(debug_server_write_interest_lookup(0x1fff)&4));
 n=0;debug_server_trace_write_check(0x70ff,3,9,4);assert(n==4&&events[3]==6);
 s_wtrace_ranges[0]=s_wtrace_ranges[1];s_wtrace_range_count--;
 debug_server_write_interest_invalidate();
 n=0;debug_server_trace_write_check(0x7000,3,9,4);assert(n==3);
 s_wtrace_range_count=0;debug_server_write_interest_invalidate();
 assert(debug_server_write_interest_lookup(0x70ff)==0);
 /* Unrepresented/raw aliases stay conservative; the actual memory fixture
    separately checks real cached/uncached RAM aliases and old-value effects. */
 assert(debug_server_write_interest_lookup(0x80002000)==3);
 assert(debug_server_write_interest_lookup(0xa0002000)==3);
 s_wtrace_boot_range_count=s_wtrace_trans_range_count=0;s_wtrace_range_count=1;
 s_wtrace_ranges[0]=(FixtureRange){0x1f8003ff,0x1f800400};
 debug_server_write_interest_invalidate();
 n=0;debug_server_trace_write_check(0x1f8003fe,3,9,4);assert(n==3);
 n=0;debug_server_trace_write_check(0x1f8003ff,3,9,4);
 assert(n==4&&events[1]==REC_KIND_SP_W&&events[3]==6);
 assert(debug_server_write_interest_lookup(0x1f800400)==4); /* conservative outside scratch */
 s_wtrace_range_count=0;debug_server_write_interest_invalidate();
 assert(debug_server_write_interest_lookup(0x1f8003ff)==0);
 n=0;PSX_DEBUG_BLOCK_OBSERVE(0x80012340);assert(g_psx_last_block==0x12340&&!ls_calls&&!watch_entries&&!probe_entries);
 /* First LS leader is admitted while g_ls_mode is still zero. */
 ls_armed=1;g_psx_debug_block_interest=PSX_DEBUG_BLOCK_LOCKSTEP;
 PSX_DEBUG_BLOCK_OBSERVE(0x80012344);assert(n==1&&events[0]==9&&ls_calls==1);
 s_cyc_watch_armed=s_pc_probe_armed=1;debug_server_watch_interest_refresh();
 assert(g_psx_debug_block_interest==7);n=0;PSX_DEBUG_BLOCK_OBSERVE(0x80012348);
 assert(n==3&&events[0]==7&&events[1]==8&&events[2]==9);
 s_cyc_watch_armed=s_pc_probe_armed=ls_armed=0;debug_server_watch_interest_refresh();
 assert(g_psx_debug_block_interest==PSX_DEBUG_BLOCK_LOCKSTEP);g_psx_debug_block_interest=0;cpu.gpr[31]=0x100;n=0;
 PSX_DEBUG_BLOCK_OBSERVE(0x8006AAF0);assert(nd_reads==2&&watch_entries==2&&probe_entries==2);
 /* Every unarmed ND address still reaches the exact optional body. */
 static const uint32_t nd_pc[]={0x8006AB3C,0x8006ACFC,0x8006AD08,0x80069CC4};
 for(unsigned i=0;i<4;i++){unsigned calls=ls_calls,w=watch_entries,p=probe_entries;
  PSX_DEBUG_BLOCK_OBSERVE(nd_pc[i]);assert(g_psx_last_block==(nd_pc[i]&0x1FFFFFFFu)&&ls_calls==calls+1&&watch_entries==w+1&&probe_entries==p+1);}
 s_fmv_quiet=1;unsigned before=ls_calls;g_psx_debug_block_interest=PSX_DEBUG_BLOCK_CYCLE;
 PSX_DEBUG_BLOCK_OBSERVE(0x80012350);
 assert(g_psx_last_block==0x12350&&ls_calls==before);
 /* The12-PC admission is pure; actual focused records keep every field/order. */
 s_fmv_quiet=0;g_psx_debug_block_interest=0;sreg_fixture_mode=1;cpu.read_word=psx_read_word;cpu.read_half=psx_read_half;
 sreg_reset_reads();sreg_trace_record(0x80012340u);assert(!sreg_word_reads&&!sreg_half_reads&&!s_sreg_trace_seq);
 int *owners[]={&g_ls_mode,&g_ls_replay_active,&g_ram_read_watch_active,&g_dma_exec_depth,&g_host_store_depth,&g_psx_render_pass_active};
 for(unsigned i=0;i<sizeof owners/sizeof owners[0];i++){
  *owners[i]=1;sreg_reset_reads();sreg_trace_record(0x80012340u);
  assert(sreg_word_reads==2&&!sreg_half_reads&&!s_sreg_trace_seq);
  assert(sreg_reads[0]==0x108u&&sreg_reads[1]==0x80000100u);
  if(i<3)assert(sreg_effects==2);*owners[i]=0;
 }
 g_ds_recording=1;sreg_reset_reads();sreg_trace_record(0x80012340u);assert(sreg_word_reads==2&&sreg_effects==2);g_ds_recording=0;
 cpu.cop0[12]=0x10000;sreg_reset_reads();sreg_trace_record(0x80012340u);assert(sreg_word_reads==2);cpu.cop0[12]=0;
 sreg_sr=0x10000;sreg_reset_reads();sreg_trace_record(0x80012340u);assert(sreg_word_reads==2);sreg_sr=0;
 g_overlay_flush_pending_cycles=sreg_foreign_owner;sreg_reset_reads();sreg_trace_record(0x80012340u);assert(sreg_word_reads==2);g_overlay_flush_pending_cycles=NULL;
 cpu.read_word=sreg_custom_word;sreg_reset_reads();sreg_trace_record(0x80012340u);assert(sreg_word_reads==2);cpu.read_word=psx_read_word;
 cpu.read_half=sreg_custom_half;sreg_reset_reads();sreg_trace_record(0x80012340u);assert(sreg_word_reads==2);cpu.read_half=psx_read_half;
 g_ram_read_watch_active=1;sreg_root=0;sreg_reset_reads();sreg_trace_record(0x80012340u);assert(sreg_word_reads==1&&sreg_effects==1);sreg_root=0x80000100;g_ram_read_watch_active=0;
 static const uint32_t focus[]={0x800171D4,0x8001A51C,0x8001A670,0x8001A774,0x8001A954,0x8001CE80,0x8001DE24,0x80021C24,0x80021CC8,0x800222B8,0x800223E0,0x8005B40C};
 cpu.gpr[29]=0x80030000;cpu.gpr[31]=0x80044444;
 for(unsigned r=0;r<8;r++)cpu.gpr[16+r]=0x6000+r;
 for(unsigned r=0;r<4;r++)cpu.gpr[4+r]=0xA000+r;
 for(unsigned i=0;i<12;i++){
  sreg_reset_reads();sreg_trace_record(focus[i]);assert(s_sreg_trace_seq==i+1&&sreg_word_reads==11&&sreg_half_reads==2);
  SregTraceEntry *e=&s_sreg_trace[i];assert(e->seq==i&&e->tcb==0x8000E100&&e->func==focus[i]&&e->frame==7&&e->ra==cpu.gpr[31]&&e->sp==cpu.gpr[29]);
  assert(e->s0==0x6000&&e->s1==0x6001&&e->s2==0x6002&&e->s3==0x6003&&e->s4==0x6004&&e->s5==0x6005&&e->s6==0x6006&&e->s7==0x6007);
  assert(e->prev_s0==(i?0x6000:0)&&e->prev_s1==(i?0x6001:0)&&e->prev_s2==(i?0x6002:0)&&e->prev_s3==(i?0x6003:0)&&e->reason==(i?4:6));
  assert(e->a0==0xA000&&e->a1==0xA001&&e->a2==0xA002&&e->a3==0xA003);
  assert(e->task_ptr==0x80020000&&e->task_state==0x12345678&&e->task_mode==((0x80020000u+72u)^0xABCDu)%65536u&&e->task_submode==((0x80020000u+74u)^0xABCDu)%65536u);
  assert(e->stack10==((cpu.gpr[29]+0x10u)^0x13579BDFu)&&e->stack14==((cpu.gpr[29]+0x14u)^0x13579BDFu)&&e->stack18==((cpu.gpr[29]+0x18u)^0x13579BDFu)&&e->stack1c==((cpu.gpr[29]+0x1Cu)^0x13579BDFu));
  assert(e->stack20==((cpu.gpr[29]+0x20u)^0x13579BDFu)&&e->stack28==((cpu.gpr[29]+0x28u)^0x13579BDFu)&&e->stack40==((cpu.gpr[29]+0x40u)^0x13579BDFu));
  assert(sreg_reads[0]==0x108&&sreg_reads[1]==0x80000100&&sreg_reads[2]==0x1F8001D4&&sreg_reads[3]==0x80020000&&sreg_reads[4]==0x80020048&&sreg_reads[5]==0x8002004A);
 }
 g_ds_recording=g_ram_read_watch_active=1;sreg_reset_reads();sreg_trace_record(focus[0]);assert(s_sreg_trace_seq==13&&sreg_word_reads==11&&sreg_half_reads==2&&sreg_effects==13);g_ds_recording=g_ram_read_watch_active=0;
 sreg_root=0;sreg_reset_reads();sreg_trace_record(focus[0]);assert(sreg_word_reads==1&&s_sreg_trace_seq==13);sreg_root=0x80000100;
 debug_cpu_ptr=NULL;sreg_reset_reads();sreg_trace_record(focus[0]);assert(!sreg_word_reads&&s_sreg_trace_seq==13);debug_cpu_ptr=&cpu;
 /* Use the real LS owner/leader bodies; replay internals are unchanged seams. */
 fixture_ls_owner=fixture_ls_owner_at_leader;sreg_fixture_mode=0;s_ls_record_only=s_lsf_record_only=1;
 s_frame_count=7;unsigned quiet=ls_calls;PSX_DEBUG_BLOCK_OBSERVE(0x80012340);
 assert(ls_calls==quiet&&!g_ls_mode);
 ls_set_window(7,8);assert(g_psx_debug_block_interest==PSX_DEBUG_BLOCK_LOCKSTEP);
 PSX_DEBUG_BLOCK_OBSERVE(0x80012340);assert(g_ls_mode==1&&s_ls_prev_valid&&s_ls_block==0x12340&&s_ls_R0.gpr[16]==cpu.gpr[16]);
 PSX_DEBUG_BLOCK_OBSERVE(0x80012344);assert(g_ls_mode==1&&s_ls_div.blocks_checked==1&&s_ls_block==0x12344);
 s_frame_count=9;PSX_DEBUG_BLOCK_OBSERVE(0x80012348);assert(!g_ls_mode&&!s_ls_prev_valid);
 /* Keep armed window admission outside its frame range, until explicit clear. */
 assert(g_psx_debug_block_interest==PSX_DEBUG_BLOCK_LOCKSTEP);ls_set_window(0,0);
 assert(!g_psx_debug_block_interest);quiet=ls_calls;PSX_DEBUG_BLOCK_OBSERVE(0x8001234C);assert(ls_calls==quiet);
 ls_func_set_window(10,12);assert(!g_psx_debug_block_interest);s_frame_count=10;
 ls_func_enter(0x80012000,&cpu);assert(s_lsf_active&&!s_lsf_recording&&!g_ls_mode&&g_psx_debug_block_interest==4);
 PSX_DEBUG_BLOCK_OBSERVE(0x80012000);assert(s_lsf_recording&&g_ls_mode==1&&s_lsf_entry==0x80012000);
 ls_func_exit(0x80012000,&cpu,1);assert(!s_lsf_active&&!g_ls_mode&&!g_psx_debug_block_interest&&s_lsf_div.segments_checked==1);
 ls_func_enter(0x80012000,&cpu);ls_func_exit(0x80012004,&cpu,1);
 assert(!s_lsf_active&&!g_psx_debug_block_interest&&s_lsf_div.skipped_conflict==1);
 ls_func_enter(0x80012000,&cpu);ls_func_exit(0x80012000,&cpu,0);
 assert(!s_lsf_active&&!g_psx_debug_block_interest&&s_lsf_div.skipped_unhandled==1);
 fixture_in_exception=1;ls_func_enter(0x80012000,&cpu);assert(!s_lsf_active&&!g_psx_debug_block_interest&&s_lsf_div.skipped_irq==1);fixture_in_exception=0;
 g_ls_replay_active=1;ls_func_enter(0x80012000,&cpu);assert(!s_lsf_active&&!g_psx_debug_block_interest&&s_lsf_div.skipped_conflict==2);g_ls_replay_active=0;
 /* Watch publication preserves LS ownership and LS transitions preserve watches. */
 s_pc_probe_armed=1;debug_server_watch_interest_refresh();ls_set_window(10,12);assert(g_psx_debug_block_interest==6);
 ls_set_window(0,0);assert(g_psx_debug_block_interest==2);s_pc_probe_armed=0;debug_server_watch_interest_refresh();assert(!g_psx_debug_block_interest);
 /* Unknown/DLL/interpreter callers retain the original public fallback. */
 quiet=ls_calls;debug_server_cyc_observe(0x80012340);assert(ls_calls==quiet+1&&g_psx_last_block==0x12340);
 puts("PASS actual observer routing: inline block publication, quiet zero callbacks, all ND addresses, watch publication, real LS first/end-window/function owners, fallback; existing pages/S-register records retained");
}
'''
    routing = source[source.index("/* Derived optional-write routing"):
                     source.index("/* Only optional recorder dispatch is cold.")]
    def typedef(name):
        end = source.index("} " + name + ";") + len("} " + name + ";")
        return source[source.rfind("typedef struct", 0, end):end]

    sreg_declarations = "\n".join((
        next(line for line in source.splitlines() if line.startswith("#define SREG_TRACE_CAP ")),
        typedef("SregTraceEntry"), typedef("SregLastEntry"),
        "static SregTraceEntry s_sreg_trace[SREG_TRACE_CAP];",
        "static uint64_t s_sreg_trace_seq; static SregLastEntry s_sreg_last[32];",
        "static uint64_t s_frame_count=7;")) + "\n"
    owner_declarations = r'''
static uint32_t s_ls_frame_lo,s_ls_frame_hi,s_lsf_frame_lo,s_lsf_frame_hi;
static int s_ls_record_only,s_lsf_record_only,s_lsf_active,s_lsf_recording,s_lsf_saw_irq;
static int s_ls_prev_valid,s_ls_trace_n,s_ls_trace_idx,s_ls_overflow,s_ls_mismatch,s_ls_m_kind;
static CPUState s_ls_R0,s_lsf_R0;
static uint32_t s_ls_block,s_lsf_entry,s_lsf_dispatch_entry,s_lsf_frame;
static struct {int found;uint64_t blocks_checked;} s_ls_div;
static struct {int found;uint64_t segments_checked,skipped_irq,skipped_overflow,skipped_unhandled,skipped_conflict,skipped_disabled;} s_lsf_div;
int g_ls_dirty_observe;
static int fixture_in_exception;
int psx_get_in_exception(void){return fixture_in_exception;}
static void ls_replay_and_compare(CPUState *c,uint32_t f,uint32_t p){(void)c;(void)f;(void)p;assert(0);}
static void lsf_replay_and_compare(CPUState *c,uint32_t f,uint32_t p){(void)c;(void)f;(void)p;assert(0);}
'''
    owners = owner_declarations + "\n".join(owner_body(name) for name in (
        "ls_debug_block_interest_refresh", "ls_set_window", "ls_func_set_window",
        "ls_func_enter", "ls_func_exit", "ls_at_leader"))
    owners = owners.replace("void ls_at_leader(", "static void fixture_ls_owner_at_leader(")
    actual = sreg_declarations + routing + "\n".join(body(name) for name in (
        "debug_server_watch_interest_refresh",
        "debug_server_cyc_observe_optional", "debug_server_cyc_observe",
        "debug_server_trace_write_optional", "history_record_live_write", "debug_server_trace_write_check",
        "sreg_trace_focus_func", "sreg_last_slot", "sreg_trace_quiet_bus", "sreg_trace_record"))
    path = work / "existing-observer-dispatch.c"
    path.write_text(preamble + actual + owners + cases, encoding="utf-8")
    for opt in ("-O0", "-O2"):
        exe = work / ("observer.exe" if os.name == "nt" else "observer")
        subprocess.run([cc, "-std=c11", opt, "-UNDEBUG", "-Werror",
                        "-I", str(root / "runtime/include"), str(path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def check_history_retention(root, work, cc):
    """Actual existing initializer/recorders: late join, order and same build contract."""
    source = (root / "runtime/src/debug_server.c").read_text(encoding="utf-8")

    def body(name):
        start = source.rfind("\n", 0, source.index(name + "(")) + 1
        # Some public functions have earlier calls. Find the actual definition.
        import re
        match = re.search(r"(?m)^(?:static )?(?:inline )?(?:PSX_DEBUG_COLD )?(?:void|int) " + name +
                          r"\([^;]*?\)\s*\{", source)
        assert match, name
        start = match.start()
        brace = source.index("{", start)
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]

    def typedef(name):
        end = source.index("} " + name + ";") + len("} " + name + ";")
        return source[source.rfind("typedef struct", 0, end):end]

    constants = "\n".join(line for line in source.splitlines() if line.startswith((
        "#define WRITE_TRACE_", "#define WTRACE_", "#define MMIO_TRACE_CAP ",
        "#define GP1_TRACE_CAP ", "#define FP_RING_CAP ")))
    preamble = r"""
#include "cpu_state.h"
#include "psx_memory.h"
#include "frame_fingerprint.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define DEFAULT_DEBUG_PORT 4470
#define PSX_DEBUG_COLD
#define REC_KIND_RAM_W 0
#define REC_KIND_SP_W 1
#define REC_KIND_MMIO_W 2
#define REC_KIND_MMIO_R 3
"""
    declarations = r"""
static CPUState cpu;CPUState *debug_cpu_ptr=&cpu;
uint32_t g_psx_ram_size=0x200000u;
static uint32_t g_debug_last_store_pc=0x1234,g_debug_current_func_addr=0x5678;
static uint64_t s_frame_count=7;
static int s_fmv_quiet,g_psx_render_pass_active;
int g_dma_exec_depth,g_dma_cur_ch;uint32_t g_dma_initiator_pc;
static PsxFrameFingerprint s_fp=PSX_FRAME_FINGERPRINT_INIT;
static FpEntry s_fp_ring[FP_RING_CAP];static uint32_t s_fp_head;static uint64_t s_fp_total;
static WriteTraceEntry *s_wtrace,*s_wtrace_boot;
static WriteTraceAllEntry *s_wtrace_all;static WriteTraceTransEntry *s_wtrace_trans;
static MmioTraceEntry *s_mmio_trace,*s_gp1_trace,*s_mmio_rtrace;
static uint64_t s_wtrace_seq,s_wtrace_boot_total,s_wtrace_all_seq,s_wtrace_trans_seq;
static uint64_t s_mmio_trace_seq,s_gp1_trace_seq,s_mmio_rtrace_seq;
static uint32_t s_wtrace_head,s_wtrace_boot_count,s_wtrace_all_head,s_wtrace_trans_head;
static uint32_t s_mmio_trace_head,s_gp1_trace_head,s_mmio_rtrace_head;
static struct {uint32_t lo,hi;} s_wtrace_ranges[WTRACE_MAX_RANGES],
 s_wtrace_boot_ranges[WTRACE_BOOT_MAX_RANGES],s_wtrace_trans_ranges[WTRACE_TRANS_MAX_RANGES],
 s_mmio_rtrace_ranges[8];
static int s_wtrace_range_count,s_wtrace_boot_range_count,s_wtrace_trans_range_count,s_mmio_rtrace_range_count;
static int s_wtrace_env_ranges,s_write_history_initialized;static char s_wtrace_env_error[96];
static int64_t s_rec_frame=-1;static uint32_t g_capture_freeze_frame;
static uint32_t i_stat=3,i_mask=4;static unsigned bindings,armed_events;
uint64_t psx_get_cycle_count(void){return 900;}
static void pgxp_set_trace_sources(uint32_t *pc,uint64_t *frame){assert(pc==&g_debug_last_store_pc&&frame==&s_frame_count);bindings++;}
static uint32_t trace_read_word(CPUState *c,uint32_t a){assert(c==&cpu);return a^0x13579BDFu;}
static int is_card_critical_addr(uint32_t p){(void)p;return 0;}
static void card_trace_record(uint32_t p,uint32_t o,uint32_t v,uint8_t w){(void)p;(void)o;(void)v;(void)w;abort();}
static void rec_event(uint8_t k,uint32_t a,uint32_t v,uint32_t p,uint32_t r){(void)k;(void)a;(void)v;(void)p;(void)r;armed_events++;}
"""
    routing = source[source.index("/* Derived optional-write routing"):
                     source.index("/* Only optional recorder dispatch is cold.")]
    functions = "\n".join(body(name) for name in (
        "fp_record_write", "fp_record_mmio", "fp_snapshot", "capture_frozen",
        "wtrace_fill_entry", "wtrace_record", "wtrace_boot_record", "wtrace_all_record",
        "wtrace_transition_record", "debug_server_trace_write_optional",
        "history_record_live_write", "debug_server_trace_write_check",
        "debug_server_trace_mmio_write", "debug_server_trace_mmio_read",
        "wtrace_apply_env_ranges", "debug_server_history_init", "debug_server_history_frame"))
    # Listener setup cannot reset histories on a late/failed connection.
    listener = body("debug_server_init")
    assert listener.index("debug_server_history_init();") < listener.index("socket(")
    assert "s_wtrace_all_seq  = 0" not in listener
    assert "debug_server_history_init();" in body("debug_server_set_cpu")
    main_source = (root / "runtime/src/main.cpp").read_text(encoding="utf-8")
    assert "#endif\n    debug_server_history_frame();" in main_source
    assert "debug_server_set_fmv_quiet(mdec_recently_active(2));\n#ifndef PSX_NO_DEBUG_TOOLS" in main_source
    cases = r"""
int main(void){
 cpu.gpr[31]=0x80001234;cpu.gpr[29]=0x80010000;cpu.gpr[16]=19;
 debug_server_history_init();assert(bindings==1 && s_write_history_initialized);
 assert(WRITE_TRACE_ALL_CAP==(1<<22) && sizeof(WriteTraceAllEntry)==32);
 assert(s_wtrace_all && s_wtrace_boot && s_wtrace_trans && s_mmio_trace && s_mmio_rtrace && s_gp1_trace);
 debug_server_trace_write_check(0x97420,1,2,4);
 debug_server_trace_write_check(0x1F8001CC,3,4,2);
 assert(s_wtrace_all_seq==2 && s_wtrace_all_head==2 && !armed_events);
 assert(s_wtrace_all[0].seq==0 && s_wtrace_all[0].addr==0x97420 && s_wtrace_all[0].new_val==2);
 assert(s_wtrace_all[0].pc==0x1234 && s_wtrace_all[0].ra==0x80001234 && s_wtrace_all[0].frame==7 && s_wtrace_all[0].w==4);
 assert(s_wtrace_all[1].seq==1 && s_wtrace_all[1].addr==0x1F8001CC && s_wtrace_all[1].w==2);
 assert(s_wtrace_boot_total==1 && s_wtrace_boot_count==1 && s_wtrace_trans_seq==2);
 assert(s_wtrace_trans[0].old_val==1 && s_wtrace_trans[0].new_val==2 && s_wtrace_trans[0].s0==19);
 assert(s_wtrace_trans[0].stk20==((0x80010000u+20)^0x13579BDFu));
 void *retained=s_wtrace_all;debug_server_history_init();
 assert(s_wtrace_all==retained && s_wtrace_all_seq==2 && s_wtrace_trans_seq==2 && bindings==1);
 /* Real ring-boundary producer behavior, preserving the fixed4M event window. */
 s_wtrace_all_seq=WRITE_TRACE_ALL_CAP-1u;s_wtrace_all_head=WRITE_TRACE_ALL_CAP-1u;
 debug_server_trace_write_check(0x100000,4,5,1);debug_server_trace_write_check(0x100001,5,6,1);
 assert(s_wtrace_all_seq==WRITE_TRACE_ALL_CAP+1u && s_wtrace_all_head==1);
 assert(s_wtrace_all[WRITE_TRACE_ALL_CAP-1u].seq==WRITE_TRACE_ALL_CAP-1u && s_wtrace_all[0].seq==WRITE_TRACE_ALL_CAP);
 debug_server_trace_mmio_write(0x1F801814,0xAA55,4);
 debug_server_trace_mmio_read(0x1F801800,0x23,1);
 assert(s_mmio_trace_seq==1 && s_gp1_trace_seq==1 && s_mmio_rtrace_seq==1);
 assert(s_mmio_trace[0].val==0xAA55 && s_mmio_trace[0].ra==0x80001234 && s_mmio_trace[0].frame==7);
 assert(s_gp1_trace[0].val==0xAA55 && s_mmio_rtrace[0].val==0x23 && s_mmio_rtrace[0].width==1);
 debug_server_history_frame();assert(s_fp_total==1 && s_fp_ring[0].frame==7);
 uint64_t writes=s_wtrace_all_seq;PsxFrameFingerprint fp=s_fp;
 g_psx_render_pass_active=1;debug_server_trace_write_check(0x97420,2,3,4);
 debug_server_trace_mmio_write(0x1F801814,0,4);debug_server_trace_mmio_read(0x1F801800,0,1);
 assert(s_wtrace_all_seq==writes && s_mmio_trace_seq==1 && s_mmio_rtrace_seq==1 && !memcmp(&s_fp,&fp,sizeof fp));
 g_psx_render_pass_active=0;g_capture_freeze_frame=7;
 debug_server_trace_mmio_write(0x1F801814,0,4);debug_server_trace_mmio_read(0x1F801800,0,1);
 assert(s_mmio_trace_seq==1 && s_mmio_rtrace_seq==1);
 printf("PASS actual shared history: standalone/idempotent init, ordered late records,4M wrap, MMIO/FP/pass/freeze; heap_bytes=%zu\n",
  WRITE_TRACE_ALL_CAP*sizeof(WriteTraceAllEntry)+(WRITE_TRACE_CAP+WRITE_TRACE_BOOT_CAP)*sizeof(WriteTraceEntry)+WRITE_TRACE_TRANS_CAP*sizeof(WriteTraceTransEntry)+(2u*MMIO_TRACE_CAP+GP1_TRACE_CAP)*sizeof(MmioTraceEntry));
}
"""
    path = work / "existing-history-retention.c"
    path.write_text(preamble + constants + "\n" + "\n".join(typedef(name) for name in (
        "WriteTraceEntry", "WriteTraceAllEntry", "WriteTraceTransEntry", "MmioTraceEntry", "FpEntry")) +
        declarations + routing + functions + cases, encoding="utf-8")
    for opt in ("-O0", "-O2"):
        for production in (True, False):
            exe = work / ("history.exe" if os.name == "nt" else "history")
            argv = [cc, "-std=c11", opt, "-UNDEBUG", "-Werror", "-I", str(root / "runtime/include")]
            if production:
                argv.append("-DPSX_NO_DEBUG_TOOLS=1")
            subprocess.run(argv + [str(path), "-o", str(exe)], check=True)
            env = dict(os.environ)
            env.pop("PSX_WTRACE_BOOT_RANGES", None)
            subprocess.run([str(exe)], check=True, env=env)


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
            check_history_retention(root, work, args.cc)
    print("PASS: actual canonical/native data access and dynamic observers (O0/O2, production/debug)")


if __name__ == "__main__":
    main()
