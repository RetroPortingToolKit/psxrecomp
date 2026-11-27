#!/usr/bin/env python3
"""Execute the real FRAME_COMMIT owner with real input/HC rings."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^[^\n;{}]*\b" + re.escape(name) +
                      r"\([^;{}]*\)\s*\n\{", source, re.M)
    assert match, name
    signature = match.group(0)[:-1].rstrip()
    return signature + "\n{" + function_body(source[match.start():], signature) + "}\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--recomp-net", type=Path, default=ROOT / "lib/recomp-net")
    args = parser.parse_args()
    source = (args.source / "runtime/src/psx_netplay.c").read_text(encoding="utf-8")
    rb_source = (args.source / "runtime/src/psx_netplay_rb.c").read_text(encoding="utf-8")
    abort_name = "psx_netplay_rb_abort_resim_core_mismatch"
    abort_source = function(rb_source[rb_source.rindex("int " + abort_name):], abort_name)
    lineage = "static void np_flush_confirmed_frame_commits" in source
    media_prime = "void psx_netplay_frame_commit_prime_after" in source
    if lineage:
        reconcile = function_body(source, "static void np_rollback_reconcile_wire(void)")
        assert reconcile.index("np_invalidate_execution_from(t)") < reconcile.index("netplay_ih_promote(")
        assert "np_flush_confirmed_frame_commits();" in reconcile
        rb = (args.source / "runtime/src/psx_netplay_rb.c").read_text(encoding="utf-8")
        enter = function_body(rb, "static void maybe_enter_replay(void)\n{")
        assert enter.index("g_peer_baseline_digest != g_local_baseline_digest") < enter.index("psx_netplay_execution_anchor(load)") < enter.index("rnet_rb_set_phase(g_rb, nRNetRbPhaseReplay)")
        rereplay = function_body(rb, "static void arm_rereplay_after_load(uint32_t reload_tick)")
        assert rereplay.index("if (!g_episode_baseline_matched)") < rereplay.index("psx_netplay_execution_anchor(reload_tick)")
        sealed = function_body(rb, "static void publish_sealed_sio(uint32_t tick)")
        assert "rnet_rb_seat_row_authoritative(g_rb, slot, tick)" in sealed
    declarations = source[source.index("#define NP_PART_RING"):
                          source.index("static void np_part_ring_reset")]
    helpers = function(source, "np_part_ring_reset")
    if lineage:
        for name in ("psx_netplay_execution_anchor", "np_part_ring_execution",
                     "np_note_executed_pad", "np_execution_inputs_confirmed",
                     "np_invalidate_execution_from"):
            helpers += function(source, name)
    if media_prime:
        helpers += function(source, "psx_netplay_frame_commit_prime_after")
        settle = function_body(rb_source, "static void rb_fmv_tick_settle(void)")
        assert "psx_netplay_frame_commit_prime_after(sim);" in settle
    for name in ("np_part_ring_put", "np_part_ring_get", "np_check_core_diverge"):
        helpers += function(source, name)
    if lineage:
        helpers += function(source, "np_flush_confirmed_frame_commits")
    helpers += function(source, "np_emit_frame_commit") + function(source, "note_sim_pad")
    if lineage:
        helpers += function(source, "np_rb_apply_frame_slot")
        helpers += function(rb_source, "realign_invalidate_evidence")
        if "static void realign_applied" in rb_source:
            helpers += function(rb_source, "realign_applied")
            apply = function_body(rb_source, "static int try_apply_pending_load(")
            assert "realign_applied(loaded_tick);" in apply
            schedule = function_body(rb_source, "static void schedule_live_realign(")
            assert "realign_applied(" not in schedule
        else:
            helpers += "static void realign_applied(uint32_t t) { realign_invalidate_evidence(t); }\n"
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "psx_netplay.h"
#include "netplay_state_digest.h"
#include "netplay_input_hist.h"
#include "netplay_hash_confirm.h"
#include "netplay_sim_pad_cache.h"
#include "crc32.h"
#define NP_SLOT_CAP 2
static struct { CPUState *cpu; void *session; int slot_count,rollback; NetplayHashConfirm hc; NetplayInputHist ih; } g_np;
static PsxNetplaySimPadCache s_sim_pad_cache;
static uint32_t wanted_core;
static int sent,boot_gate,transfer,abort_calls,recovery_calls,media_depth24,media_hot;
static uint32_t last_sent_tick,last_sent_core;
static RNetRbSession *g_rb;
static struct { NetplayHashConfirm *hc; } g_b;
static uint32_t g_agreed_through,g_agreed_span_lo,g_peer_resolved_through;
static uint32_t g_peer_frontier,g_local_advertised_through,g_auth_snap_through;
static int g_agreed_valid;
static void *g_snaps;
static void netplay_peer_frontier_clamp(uint32_t *p,uint32_t t) { if(*p>t)*p=t; }
static uint32_t netplay_snap_ring_drop_after(void *p,uint32_t t) { (void)p;(void)t;return 0; }
int psx_netplay_collective_transfer_active(void) { return transfer; }
int psx_netplay_rb_boot_dig0_gate(void) { return boot_gate; }
void psx_netplay_rb_boot_dig0_note_local(uint32_t core) { (void)core; }
int psx_netplay_rb_tip_holding(void) { return 0; }
int psx_netplay_rb_is_resimulating(void) {
    return g_rb && rnet_rb_is_active(g_rb) && rnet_rb_get_phase(g_rb)==nRNetRbPhaseReplay;
}
int psx_netplay_rb_ignore_peer_frame_commit(uint32_t t,uint32_t h) { (void)t;(void)h;return 0; }
static CPUState *cpu(void) { return NULL; }
static void log_cpu_digest_split(CPUState *c,const char *tag) { (void)c;(void)tag; }
static void abort_episode_realign(const char *why) { (void)why;++abort_calls; }
static void np_try_hc_fork_recovery(uint32_t tick) { (void)tick;++recovery_calls; }
int rnet_session_send_rb_frame_commit(void *session,uint32_t tick,uint32_t core) {
    (void)session;++sent;last_sent_tick=tick;last_sent_core=core;return 1;
}
int gpu_display_is_depth24(void) { return media_depth24; }
int mdec_recently_active(unsigned n) { (void)n;return media_hot; }
void psx_netplay_rb_cpu_for_present_digest(CPUState *out,const CPUState *in) { *out=*in; }
void netplay_core_digest_parts(const CPUState *cpu,NetplayCoreParts *parts) {
    (void)cpu;memset(parts,0,sizeof(*parts));parts->core=wanted_core;
}
uint32_t netplay_av_digest(void) { return 0; }
uint32_t netplay_cdrom_digest(void) { return 0; }
uint32_t netplay_spu_digest(void) { return 0; }
uint32_t netplay_mdec_digest(void) { return 0; }
static void np_log_live_digest(uint32_t t,const NetplayCoreParts*p,uint32_t a,uint32_t c,uint32_t s,uint32_t m,uint32_t x,const char*tag) {
    (void)t;(void)p;(void)a;(void)c;(void)s;(void)m;(void)x;(void)tag;
}
static void force_session_pads_connected(int n) { (void)n; }
static void apply_pad_slot(int slot,const PsxNetPad *pad) { (void)slot;(void)pad; }
''' + abort_source + declarations + helpers + r'''
static int inert_tick(void *ctx,uint32_t t) { (void)ctx;(void)t;return 1; }
static uint8_t no_row(void *ctx,int32_t seat,uint32_t t,RNetRbFrame *out) {
    (void)ctx;(void)seat;(void)t;(void)out;return 0;
}
static void make_episode(RNetRbPhase phase) {
    RNetRbConfig cfg={0};RNetRollbackVTable vt={0};RNetRbCorrection correction={0};
    if(g_rb)rnet_rb_destroy(g_rb);
    cfg.slot_count=2;vt.load_state=inert_tick;vt.advance_sim=inert_tick;vt.get_input_row=no_row;
    g_rb=rnet_rb_create(&cfg,&vt);assert(g_rb);
    correction.epoch_id=8;correction.mismatch_tick=1;correction.load_tick=0;
    correction.target_tick=2;correction.slot=1;correction.initiator=1;
    rnet_rb_begin_episode(g_rb,&correction);assert(rnet_rb_is_active(g_rb));
    rnet_rb_set_phase(g_rb,phase);
}
static PsxNetPad pad(uint16_t buttons) {
    PsxNetPad p={buttons,0x80,0x80,0x80,0x80,1,1};return p;
}
static void publish(int seat,uint32_t tick,uint16_t buttons,int predicted) {
    PsxNetPad p=pad(buttons);RNetRbFrame row;
    netplay_ih_pad_to_frame(&p,tick,(uint8_t)predicted,&row);
    assert(netplay_ih_put(&g_np.ih,seat,&row));
NOTE_PAD
}
static void promote(int seat,uint32_t tick,uint16_t buttons) {
    PsxNetPad p=pad(buttons);RNetRbFrame row;
    netplay_ih_pad_to_frame(&p,tick,0,&row);assert(netplay_ih_promote(&g_np.ih,seat,&row));
}
static void finish(uint32_t tick,uint32_t core) { wanted_core=core;np_emit_frame_commit(tick); }
static void reset(void) {
    static CPUState cpu;
    memset(&g_np,0,sizeof(g_np));g_np.cpu=&cpu;g_np.session=&g_np;g_np.slot_count=2;g_np.rollback=1;
    netplay_hc_reset(&g_np.hc);netplay_ih_reset(&g_np.ih,2);np_part_ring_reset();
    g_b.hc=&g_np.hc;
    psx_netplay_sim_pad_cache_reset(&s_sim_pad_cache);sent=0;boot_gate=0;transfer=0;abort_calls=0;recovery_calls=0;media_depth24=0;media_hot=0;
    if(g_rb){rnet_rb_destroy(g_rb);g_rb=NULL;}
}
int main(void) {
    reset();
    /* Original boot agreement remains available even before input metadata. */
    finish(0,0x1234);assert(sent==1&&last_sent_tick==0);
    netplay_hc_note_peer(&g_np.hc,0,0x1234);
    boot_gate=1;
    publish(0,1,0xffff,0);publish(1,1,0xbfff,1);
    finish(1,0xdead);
    if(sent!=1) { fputs("FAIL: speculative execution entered FRAME_COMMIT\n",stderr);return 3; }
#if HAS_LINEAGE
    assert(!g_np.hc.local_valid[1]);
    /* Current history is authoritative but describes a different execution.
     * Neither that old core nor a correct-pad descendant is truth. */
    promote(1,1,0xffff);boot_gate=0;
    np_flush_confirmed_frame_commits();assert(sent==1);
    publish(0,2,0xffff,0);publish(1,2,0xffff,0);finish(2,0xbeef);assert(sent==1);
    np_invalidate_execution_from(1);assert(!np_part_ring_get(1)&&!np_part_ring_get(2));
    /* Actual sealed replay from the accepted baseline reconstructs lineage. */
    psx_netplay_execution_anchor(0);
    netplay_hc_prime_after(&g_np.hc,0);
    np_rb_apply_frame_slot(0,1,0xffff,0,0,1,0x80,0x80,1);
    np_rb_apply_frame_slot(1,1,0xffff,0,0,1,0x80,0x80,1);
    finish(1,0x1111);
    assert(sent==2&&last_sent_tick==1&&last_sent_core==0x1111);
    publish(0,2,0xffff,0);publish(1,2,0xffff,0);finish(2,0x2222);
    assert(sent==3&&last_sent_tick==2);
    /* A genuine confirmed-state mismatch still runs the original detector. */
    netplay_hc_note_peer(&g_np.hc,1,0x9999);np_check_core_diverge();
    assert(s_core_diverge_logged&&recovery_calls==1);
    /* Replay/Verify keeps its immediate abort-before-ordinary-log call. */
    for(int verify=0;verify<2;++verify) {
        make_episode(verify?nRNetRbPhaseVerify:nRNetRbPhaseReplay);
        s_core_diverge_logged=0;abort_calls=0;np_check_core_diverge();
        assert(abort_calls==1&&!s_core_diverge_logged);
        assert(rnet_rb_get_phase(g_rb)==nRNetRbPhaseVerify);
    }
    reset();finish(0,0x1234);netplay_hc_note_peer(&g_np.hc,0,0x1234);
    /* A correct prediction is certified later without another execution. */
    publish(0,1,0xffff,0);publish(1,1,0xbfff,1);finish(1,0xaaaa);assert(sent==1);
    promote(1,1,0xbfff);np_flush_confirmed_frame_commits();assert(sent==2&&last_sent_core==0xaaaa);
    /* Later same-tick publication cannot rewrite captured execution. */
    reset();finish(0,0x1234);netplay_hc_note_peer(&g_np.hc,0,0x1234);
    publish(0,1,0xffff,0);publish(1,1,0xbfff,1);finish(1,0xdead);assert(sent==1);
    publish(1,1,0xffff,0);np_flush_confirmed_frame_commits();assert(sent==1);
    /* Collective transfer owns its boundary, including staged old ticks. */
    reset();finish(0,0x1234);netplay_hc_note_peer(&g_np.hc,0,0x1234);
    transfer=1;publish(0,1,0xffff,0);publish(1,1,0xffff,0);finish(1,0x1111);assert(sent==1);
    transfer=0;np_flush_confirmed_frame_commits();assert(sent==2);
    /* Session/user-load cleanup retires host-only lineage as well. */
    np_part_ring_reset();assert(s_commit_next_tick==0&&!s_commit_anchor_ready);
    /* A successful live realign also discards the old execution timeline.
     * Its next FRAME_COMMIT must come from the newly executed successor,
     * even when the old timeline already published farther ahead. */
    reset();finish(0,0x1234);netplay_hc_note_peer(&g_np.hc,0,0x1234);
    for(uint32_t t=1;t<=3;++t) {
        publish(0,t,0xffff,0);publish(1,t,0xffff,0);finish(t,0x1000+t);
        netplay_hc_note_peer(&g_np.hc,t,0x1000+t);
    }
    assert(sent==4&&s_commit_next_tick==4);
    realign_applied(1);
    if(s_commit_next_tick!=2||np_part_ring_get(2)||np_part_ring_get(3)) {
        fputs("FAIL: applied realign retained discarded execution descendants\n",stderr);return 6;
    }
    publish(0,2,0xffff,0);publish(1,2,0xffff,0);finish(2,0x2202);
    assert(sent==5&&last_sent_tick==2&&last_sent_core==0x2202);
    assert(!netplay_hc_confirm_through(&g_np.hc,2));
    netplay_hc_note_peer(&g_np.hc,2,0x2202);
    assert(netplay_hc_confirm_through(&g_np.hc,2));
    /* Delay-sync has no rollback movie-exit re-prime. Every actual executed
     * state must be committed, including a short hot24-bit movie interval;
     * otherwise one absent slot stalls the producer forever after wrapping. */
    reset();g_np.rollback=0;
    for(uint32_t tick=0;tick<256;++tick) {
        media_depth24=media_hot=(tick>=51&&tick<60);
        publish(0,tick,0xffff,0);publish(1,tick,0xffff,0);
        finish(tick,0x1000+tick);
        if(sent!=(int)tick+1) {
            fprintf(stderr,"FAIL: delay movie dropped actual FRAME_COMMIT tick %u\n",tick);
            return 4;
        }
        netplay_hc_note_peer(&g_np.hc,tick,0x1000+tick);
        assert(netplay_hc_confirm_through(&g_np.hc,tick));
    }
    assert(s_commit_next_tick==256&&netplay_hc_resolved_through(&g_np.hc)==255);
    /* The existing rollback owner still defers live hot-movie work and
     * continues to emit the real states reconstructed during Replay. */
    reset();finish(0,0x1234);netplay_hc_note_peer(&g_np.hc,0,0x1234);
    media_depth24=media_hot=1;
    publish(0,1,0xffff,0);publish(1,1,0xffff,0);finish(1,0x1111);
    assert(sent==1&&!np_part_ring_get(1));
    make_episode(nRNetRbPhaseReplay);finish(1,0x1111);
    assert(sent==2&&last_sent_tick==1&&last_sent_core==0x1111);
    /* A long live movie intentionally omits commits. Its established HC
     * re-prime must also retire that producer gap; wrapping the local ring
     * must not strand every later real, authoritative execution. */
    reset();finish(0,0x1234);netplay_hc_note_peer(&g_np.hc,0,0x1234);
    publish(0,1,0xffff,0);publish(1,1,0xffff,0);finish(1,0x1001);
    netplay_hc_note_peer(&g_np.hc,1,0x1001);assert(s_commit_anchor_ready);
    media_depth24=media_hot=1;
    for(uint32_t tick=2;tick<=200;++tick) {
        publish(0,tick,0xffff,0);publish(1,tick,0xffff,0);finish(tick,0x1000+tick);
    }
    assert(sent==2&&s_commit_next_tick==2);
    media_depth24=media_hot=0;netplay_hc_prime_after(&g_np.hc,200);
#if HAS_MEDIA_PRIME
    psx_netplay_frame_commit_prime_after(200);
#endif
    publish(0,201,0xffff,0);publish(1,201,0xbfff,1);finish(201,0x2201);
    assert(sent==2); /* re-prime never certifies a prediction */
    promote(1,201,0xbfff);np_flush_confirmed_frame_commits();
    if(sent!=3||last_sent_tick!=201||last_sent_core!=0x2201) {
        fputs("FAIL: rollback movie gap stranded post-movie FRAME_COMMIT\n",stderr);return 5;
    }
    assert(!netplay_hc_confirm_through(&g_np.hc,201));
    netplay_hc_note_peer(&g_np.hc,201,0x2201);
    assert(netplay_hc_confirm_through(&g_np.hc,201));
#if HAS_MEDIA_PRIME
    reset();psx_netplay_frame_commit_prime_after(200);
    assert(s_commit_next_tick==0&&!s_commit_anchor_ready); /* boot gate retained */
    finish(0,0x1234);netplay_hc_note_peer(&g_np.hc,0,0x1234);
    publish(0,1,0xffff,0);publish(1,1,0xffff,0);finish(1,0x1001);
    g_np.rollback=0;psx_netplay_frame_commit_prime_after(200);
    assert(s_commit_next_tick==2); /* delay has no omitted-movie boundary */
#endif
    puts("confirmed ancestry / promotion / replay / real mismatch controls PASS");
#endif
    return 0;
}
'''
    fixture = fixture.replace("NOTE_PAD", "    note_sim_pad(seat,tick,&p,!predicted);"
                              if lineage else "    note_sim_pad(seat,tick,&p);")
    with tempfile.TemporaryDirectory(prefix="psx-commit-lineage-") as directory:
        c = Path(directory) / "test.c"
        exe = Path(directory) / "test.exe"
        c.write_text(fixture, encoding="utf-8")
        command = [args.compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                   "-Wno-unused-function", "-DPSX_HAS_RECOMP_NET",
                   f"-DHAS_LINEAGE={int(lineage)}", "-I", str(ROOT / "runtime/include"),
                   f"-DHAS_MEDIA_PRIME={int(media_prime)}",
                   "-I", str(args.recomp_net / "include"), str(c),
                   str(ROOT / "runtime/src/netplay_input_hist.c"),
                   str(ROOT / "runtime/src/psx_net_pad.c"),
                   str(ROOT / "runtime/src/crc32.c"),
                   str(args.recomp_net / "src/input/rnet_input_hist.c"),
                   str(args.recomp_net / "src/rollback/rnet_hash_confirm.c"),
                   str(args.recomp_net / "src/rollback/rnet_rollback.c"),
                   str(args.recomp_net / "src/input/rnet_input_contract.c"),
                   "-o", str(exe)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="")
        assert result.returncode == 0, f"production commit fixture returned {result.returncode}"
        assert "FIRST CORE DIVERGE" in result.stderr, "real confirmed mismatch was not observed"


if __name__ == "__main__":
    main()
