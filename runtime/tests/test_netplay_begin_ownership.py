#!/usr/bin/env python3
"""Run production BEGIN rejection/arbitration with recorded owner mutations."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_netplay_commit_lineage import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--source', type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source / 'runtime/src/psx_netplay_rb.c').read_text(encoding='utf-8')
    start = source.index('    /* New epoch while TipHold:')
    fixed = source.find('    /* Reject retired BEGINs before')
    if fixed >= 0:
        start = min(start, fixed)
    end = source.index('    (void)rb_in_fmv_defer_rewind_window();', start)
    owner = source[start:end]
    peer_helpers = function(source, 'clear_tip_hold_rereplay_pending')
    if 'static void finalize_peer_tip_hold_commit' in source:
        peer_helpers += function(source, 'finalize_peer_tip_hold_commit')
        peer_step = 'finalize_peer_tip_hold_commit(snap_tick);'
        assert 'finalize_peer_tip_hold_commit(snap_tick);' in source[source.index('static int maybe_abandon_tip_extend'):]
    else:
        start = source.index('        /* Clamp commit to snap/from')
        peer_step = source[start:source.index('        return 1;', start)]
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "netplay_peer_frontier.h"
typedef enum { nRNetRbPhaseSealInputs=1, nRNetRbPhaseAwaitingBaseline=2,
               nRNetRbPhaseTipHold=5 } RNetRbPhase;
typedef struct { int active, following; uint32_t epoch; RNetRbPhase phase;
                 uint32_t target; int corrected_slot; } Owner;
static Owner state, *g_rb=&state;
static uint32_t g_last_commit_epoch,g_agreed_through;
static int g_agreed_valid=1,g_joining,g_episode_snap_applied,g_abort_wire_class;
static int aborted,finalized,nacked,stashed,followed;
static int g_tip_hold_rereplay_pending,g_tip_hold_block_quiet;
static uint32_t g_tip_hold_rereplay_from,g_tip_hold_rereplay_mismatch,g_tip_extend_from_tick;
static int local_slot,queued,queued_slot;static uint32_t queued_tick,committed_target;
static struct { int *local_slot; } g_b={&local_slot};
static void clear_tip_hold_rereplay_pending(void);
#define RB_EPOCH_SLOT_MASK 7u
#define RNET_RB_ABORT_CLASS_REALIGN 0
static int rnet_rb_is_active(Owner *r) { return r->active; }
static int rnet_rb_is_tip_holding(Owner *r) { return r->active&&r->phase==nRNetRbPhaseTipHold; }
static int rnet_rb_is_from_peer_notify(Owner *r) { return r->following; }
static uint32_t rnet_rb_get_epoch_id(Owner *r) { return r->epoch; }
static RNetRbPhase rnet_rb_get_phase(Owner *r) { return r->phase; }
static uint32_t rnet_rb_get_target_tick(Owner *r) { return r->target; }
static int rnet_rb_get_corrected_slot(Owner *r) { return r->corrected_slot; }
static void finalize_tip_hold(void) {
    ++finalized;g_last_commit_epoch=state.epoch;state.active=0;
    committed_target=state.target;
    if(g_tip_hold_rereplay_pending&&g_tip_hold_rereplay_from<committed_target)
        committed_target=g_tip_hold_rereplay_from;
    clear_tip_hold_rereplay_pending();
}
static int psx_netplay_requeue_correction(uint32_t tick,int slot) {
    assert(!state.active&&!g_tip_hold_rereplay_pending);++queued;queued_tick=tick;queued_slot=slot;return 1;
}
static void abort_episode(const char *why) { (void)why;++aborted;state.active=0; }
static void clear_rewind_cooldown(const char *why) { (void)why; }
static void follow_refuse_nack(uint32_t e,uint32_t m,uint32_t l,uint32_t t,int s,const char *why) {
    (void)e;(void)m;(void)l;(void)t;(void)s;(void)why;++nacked;
}
static void stash_peer_begin(uint32_t e,uint32_t m,uint32_t l,uint32_t t,int s,uint8_t f) {
    (void)e;(void)m;(void)l;(void)t;(void)s;(void)f;++stashed;
}
static void begin(uint32_t epoch,uint32_t load,int joint) {
    uint32_t mismatch=load+1,target=load+4;int slot=0;uint8_t wire_flags=0;
''' + owner + r'''
    ++followed;
}
''' + peer_helpers + '\nstatic void peer_commit(uint32_t snap_tick) {\n' + peer_step + r'''
}
static void reset(RNetRbPhase phase,uint32_t committed) {
    state=(Owner){1,0,25,phase,3894,1};g_last_commit_epoch=committed;
    g_agreed_through=1171;g_agreed_valid=1;g_joining=0;
    aborted=finalized=nacked=stashed=followed=0;
    clear_tip_hold_rereplay_pending();g_tip_extend_from_tick=0;queued=0;queued_tick=0;queued_slot=-1;committed_target=0;
}
int main(void) {
    reset(nRNetRbPhaseSealInputs,16);begin(16,1166,0);
    if(aborted||finalized||!state.active||state.epoch!=25||nacked!=1) {
        fputs("FAIL: retired BEGIN destroyed the active episode\n",stderr);return 5;
    }
    reset(nRNetRbPhaseTipHold,16);begin(16,1166,0);
    assert(!aborted&&!finalized&&state.active&&state.phase==nRNetRbPhaseTipHold&&nacked==1);
    /* Numeric age is not evidence: a valid lower-numbered, lower-seat BEGIN wins. */
    reset(nRNetRbPhaseSealInputs,8);begin(16,1171,0);
    assert(aborted==1&&!finalized&&!nacked&&followed==1);
    /* The current epoch's retransmit must remain a harmless keepalive. */
    reset(nRNetRbPhaseSealInputs,16);state.epoch=16;begin(16,1166,0);
    assert(state.active&&!aborted&&!finalized&&!nacked&&!followed&&!stashed);
    reset(nRNetRbPhaseSealInputs,8);begin(24,1166,0);
    assert(state.active&&!aborted&&!finalized&&nacked==1);
    /* Existing joining / retained-joint exceptions remain usable. */
    reset(nRNetRbPhaseSealInputs,8);g_joining=1;begin(16,1166,0);
    assert(aborted==1&&!nacked&&followed==1);
    reset(nRNetRbPhaseSealInputs,8);begin(16,1166,1);
    assert(aborted==1&&!nacked&&followed==1);
    /* Recorded Windows edge: peer commits3892 after late held-X extended
     * target to3894. Clearing that epoch must retain correction3893/seat1. */
    reset(nRNetRbPhaseTipHold,16);g_tip_hold_rereplay_pending=1;
    g_tip_hold_rereplay_from=g_tip_extend_from_tick=3892;g_tip_hold_rereplay_mismatch=3893;
    peer_commit(3892);
    if(finalized!=1||committed_target!=3892||queued!=1||queued_tick!=3893||queued_slot!=1) {
        fputs("FAIL: peer COMMIT discarded the pending corrected span\n",stderr);return 6;
    }
    /* A peer's higher endpoint cannot certify our unreplayed suffix. */
    reset(nRNetRbPhaseTipHold,16);g_tip_hold_rereplay_pending=1;
    g_tip_hold_rereplay_from=g_tip_extend_from_tick=3892;g_tip_hold_rereplay_mismatch=3893;
    peer_commit(3894);assert(committed_target==3892&&queued==1&&queued_tick==3893);
    /* No deferred execution: preserve ordinary clean finalization. */
    reset(nRNetRbPhaseTipHold,16);peer_commit(3894);
    assert(finalized==1&&committed_target==3894&&!queued);
    puts("production retired-BEGIN / TipHold / legitimate arbitration controls PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-begin-owner-') as directory:
        c = Path(directory)/'test.c'
        exe = Path(directory)/'test.exe'
        c.write_text(fixture, encoding='utf-8')
        subprocess.run([args.compiler,'-std=c11','-O2','-Wall','-Wextra','-UNDEBUG',
                        '-I',str(ROOT/'runtime/include'),str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    main()
