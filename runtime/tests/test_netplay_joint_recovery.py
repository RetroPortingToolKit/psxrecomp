#!/usr/bin/env python3
"""Execute production retained-prefix selection against real HC/snapshot rings.

This is a retention/epoch-range gate, not a guest reconvergence test. Native
changing-input peers must additionally qualify BEGIN, restore, replay and POST.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body

ROOT = Path(__file__).resolve().parents[2]


def check_transfer_ownership(compiler):
    """Execute production admission decisions, not a simulated rollback."""
    source = (ROOT / 'runtime/src/psx_netplay.c').read_text(encoding='utf-8')
    rb = (ROOT / 'runtime/src/psx_netplay_rb.c').read_text(encoding='utf-8')
    signature = 'int psx_netplay_rb_begin_rewind(uint32_t mismatch_tick, int slot)\n{'
    begin = function_body(rb[rb.rindex(signature):], signature)
    assert begin.index('psx_netplay_defer_recovery_for_transfer') < begin.index('g_platform_accepted_fork')
    signature = 'void psx_netplay_rb_pump(void)\n{'
    pump = function_body(rb[rb.rindex(signature):], signature)
    assert pump.index('psx_netplay_collective_transfer_active') < pump.index('rb_media_kf_host_drive()')
    reconcile = function_body(source, 'static void np_rollback_reconcile_wire(void)\n{')
    assert 'psx_netplay_collective_transfer_active()' in reconcile
    assert 's_deferred_rw_valid = 1' in reconcile
    for name in ('save', 'load'):
        request = function_body(source, f'int psx_netplay_request_{name}(int slot)\n{{')
        assert request.index('psx_netplay_rb_active()') < request.index('rnet_session_state_probe(')
    start = source.index('typedef enum {\n    NP_XFER_NONE')
    declarations = source[start:source.index('} NpXferPhase;', start) + len('} NpXferPhase;')]
    functions = ''
    for signature in (
        'int psx_netplay_collective_transfer_active(void)',
        'int psx_netplay_defer_recovery_for_transfer(uint32_t tick, int slot)',
        'static void np_det_selftest_tick(uint32_t done)',
        'static void np_try_hc_fork_recovery(uint32_t fork_tick)',
    ):
        functions += signature + '\n{' + function_body(source, signature + '\n{') + '}\n'
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
''' + declarations + r'''
static struct { void *session; int rollback, local_slot, slot_count; NpXferPhase xfer; } g_np;
static int s_deferred_rw_valid, s_deferred_rw_slot, s_deferred_rw_logged;
static uint32_t s_deferred_rw_tick, s_fork_tick, s_fork_first_sim, s_fork_last_attempt_sim;
static unsigned begin_calls, heal_calls;
static uint32_t sim, requested_tick;
static int active, tip_hold, load_pending;
static int psx_netplay_rb_active(void) { return active; }
static int psx_netplay_rb_tip_holding(void) { return tip_hold; }
static int psx_netplay_rb_load_pending(void) { return load_pending; }
static int psx_netplay_rb_rewind_suppressed(void) { return 0; }
static int psx_netplay_rb_platform_fork_accepted(uint32_t t) { (void)t; return 0; }
static int psx_netplay_rb_fmv_defer_rewind(void) { return 0; }
static int psx_netplay_rb_fmv_desync_hold(void) { return 0; }
static int psx_netplay_rb_post_fmv_heal_eligible(void) { return 0; }
static int psx_netplay_rb_lockstep_no_invent(void) { return 0; }
static int psx_netplay_rb_fmv_episode_unsafe(uint32_t t) { (void)t; return 0; }
static uint32_t psx_netplay_rb_baseline_fork_cap(void) { return 0; }
static uint32_t rnet_session_sim_tick(void *s) { (void)s; return sim; }
/* These are call observers. Admission decisions below are production code;
 * the existing real-ring fixture and native peers qualify actual recovery. */
static int psx_netplay_rb_begin_rewind(uint32_t t, int slot) {
    assert(slot == 1); ++begin_calls; requested_tick = t; return 1;
}
static void psx_netplay_rb_request_post_fmv_heal_kf(void) { ++heal_calls; }
''' + functions + r'''
int main(void) {
    g_np.session = &g_np; g_np.rollback = 1; g_np.slot_count = 2;
    /* The actual SAVE_COORD collision: diagnostic due at64 must not steal
     * the accepted collective operation, nor consume its next cadence. */
    g_np.xfer = NP_XFER_SAVE_COORD;
    np_det_selftest_tick(64);
    if (begin_calls != 0) { fputs("FAIL: diagnostic stole SAVE_COORD\n", stderr); return 3; }
    for (int phase = NP_XFER_MC_UPLOAD; phase <= NP_XFER_LOAD_READY; ++phase) {
        g_np.xfer = (NpXferPhase)phase;
        np_det_selftest_tick(64);
        assert(begin_calls == 0);
    }
    g_np.xfer = NP_XFER_NONE;
    np_det_selftest_tick(64);
    assert(begin_calls == 1 && requested_tick == 48);

    /* Ordinary completed-input corrections retain the earliest original
     * mismatch while the transfer owns the boundary. */
    g_np.xfer = NP_XFER_LOAD_PROBE;
    assert(psx_netplay_defer_recovery_for_transfer(80, 1));
    assert(psx_netplay_defer_recovery_for_transfer(64, 1));
    assert(psx_netplay_defer_recovery_for_transfer(72, 1));
    assert(s_deferred_rw_valid && s_deferred_rw_tick == 64 && s_deferred_rw_slot == 1);
    g_np.xfer = NP_XFER_NONE;
    assert(!psx_netplay_defer_recovery_for_transfer(96, 1));
    assert(s_deferred_rw_valid && s_deferred_rw_tick == 64);

    /* A previously observed core desync also waits without resetting its
     * persistence or spending an attempt; it can proceed after release. */
    s_fork_tick = 17; s_fork_first_sim = 84; sim = 100;
    g_np.xfer = NP_XFER_LOAD_READY;
    np_try_hc_fork_recovery(17);
    assert(begin_calls == 1 && heal_calls == 0 && s_fork_last_attempt_sim == 0);
    g_np.xfer = NP_XFER_NONE;
    np_try_hc_fork_recovery(17);
    assert(begin_calls == 2 && requested_tick == 17 && heal_calls == 1);
    np_det_selftest_tick(128);
    assert(begin_calls == 3 && requested_tick == 112);
    puts("production transfer/ordinary/desync/diagnostic ownership: PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-transfer-owner-') as directory:
        directory = Path(directory)
        c = directory / 'owner.c'
        exe = directory / 'owner.exe'
        c.write_text(fixture, encoding='utf-8')
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', str(c),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], env={**os.environ, 'PSX_RB_DET_SELFTEST': '64'}, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    args = parser.parse_args()
    check_transfer_ownership(args.compiler)
    source = (ROOT / 'runtime/src/psx_netplay_rb.c').read_text(encoding='utf-8')
    assert 'joint_accept_peer_commit()' in function_body(
        source, 'static int maybe_abandon_tip_extend(void)\n{')
    declarations = source[source.index('#define PSX_RB_SYNC_FLAG_RETAINED_LOAD'):
                          source.index('/* BASELINE that arrived before follow/begin')]
    functions = ''
    for signature in (
        'static int joint_retained_load(uint32_t tick, uint32_t mismatch)',
        'static int joint_offer_below(uint32_t ceiling, uint32_t mismatch, uint32_t *offer)',
        'static void joint_recovery_hold(uint32_t mismatch, uint32_t target, int slot)',
        'static int joint_host_transfer_load(uint32_t mismatch, uint32_t target, uint32_t *load)',
        'static uint32_t follower_frontier_hint(void)',
        'static void joint_recovery_post_match(uint32_t tip)',
        'static int joint_accept_peer_commit(void)',
        'static void apply_peer_resolved(int from, uint32_t advert)',
    ):
        functions += signature + '\n{' + function_body(source, signature + '\n{') + '}\n'
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "netplay_hash_confirm.h"
#include "netplay_peer_frontier.h"
#include "recomp_net/rollback.h"
#include "recomp_net/session.h"
#include "retcomm_rbengine/snap_ring.h"
#define NetplaySnapRing RbeSnapRing
#define netplay_snap_ring_has rbe_snap_ring_has
#define netplay_snap_ring_count rbe_snap_ring_count
#define netplay_snap_ring_oldest_tick rbe_snap_ring_oldest_tick
static NetplaySnapRing *g_snaps;
static struct { NetplayHashConfirm *hc; int *local_slot; } g_b;
static int media_kf_enabled = 1;
static int rb_media_kf_enabled(void) { return media_kf_enabled; }
static int g_boot_dig0_synced, g_agreed_valid, g_pin_valid;
static uint32_t g_agreed_span_lo, g_agreed_through, g_pin_tick;
static RNetRbSession *g_rb;
static NetplayPeerFrontier g_peer_frontier;
static uint32_t g_peer_resolved_through, g_post_target;
static int g_peer_resolved_gate_expired, g_past_frontier_sticky, g_local_post_sent;
static uint64_t g_peer_resolved_wait_t0_ms;
static int tip_hold_calls, ownership_calls, g_episode_baseline_matched;
static uint32_t g_peer_commit_epoch, g_peer_commit_tick;
static void ownership_on_post_match(uint32_t tip);
static uint32_t psx_netplay_peer_seats(void) { return 2u; }
static void enter_tip_hold(uint32_t tip) {
    (void)tip; ++tip_hold_calls;
    rnet_rb_set_phase(g_rb, nRNetRbPhaseTipHold);
}
static int inert_tick(void *ctx, uint32_t tick) { (void)ctx; (void)tick; return 1; }
static uint8_t no_row(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out) {
    (void)ctx; (void)slot; (void)tick; (void)out; return 0;
}

''' + declarations + functions + r'''
/* The observer stands in for ownership policy after the proof gate; the
 * actual production completion helper owns the latches being checked. */
static void ownership_on_post_match(uint32_t tip) {
    ++ownership_calls;
    joint_recovery_post_match(tip);
    enter_tip_hold(tip);
}

static void store(uint32_t tick) {
    uint8_t *blob = malloc(1); assert(blob); *blob = (uint8_t)tick;
    assert(rbe_snap_ring_store(g_snaps, tick, blob, 1));
}
int main(void) {
    NetplayHashConfirm hc;
    uint32_t offer = 999u;
    int local_slot = 0;
    g_snaps = rbe_snap_ring_create(40); assert(g_snaps);
    g_b.hc = &hc;
    g_b.local_slot = &local_slot;
    netplay_hc_reset(&hc);
    for (uint32_t t = 0; t <= 21; ++t) {
        netplay_hc_note_local(&hc, t, t + 77u);
        netplay_hc_note_peer(&hc, t, t + 77u);
    }
    g_boot_dig0_synced = 1;
    store(0); store(16); store(22); store(28);
    /* #475: follower has advanced to 28, HC proves 21, load 15 is gone.
     * The old wire hint is 21, although the follower cannot restore 21. */
    assert(follower_frontier_hint() == 21);
    assert(!rbe_snap_ring_has(g_snaps, follower_frontier_hint()));
    assert(joint_offer_below(15, 21, &offer) && offer == 0);
    assert(joint_retained_load(offer, 21));
    assert(!joint_retained_load(21, 21)); /* never skip the first bad input */
    assert(!joint_retained_load(22, 21));
    /* Retained does not mean verified: an unconfirmed dense snapshot is not
     * a fallback merely because its tick happens to be in this local ring. */
    store(25);
    assert(!joint_retained_load(25, 30));
    assert(joint_offer_below(27, 30, &offer) && offer == 16);
    /* No recent shared entry is required when the existing host can transfer
     * a verified prefix that covers the entire original correction. */
    assert(joint_host_transfer_load(21, 40, &offer) && offer == 16);
    local_slot = 1;
    assert(!joint_host_transfer_load(21, 40, &offer));
    local_slot = 0; media_kf_enabled = 0;
    assert(!joint_host_transfer_load(21, 40, &offer));
    media_kf_enabled = 1;
    assert(!joint_host_transfer_load(21, 80, &offer));
    joint_recovery_hold(21, 10, 1);
    assert(g_joint_target == 21);
    joint_recovery_hold(21, 24, 1);
    joint_recovery_hold(23, 28, 1);
    assert(g_joint_mismatch == 21 && g_joint_target == 28);
    joint_recovery_hold(19, 26, 0);
    assert(g_joint_mismatch == 19 && g_joint_target == 28);
    /* Explicit validity separates a real retained zero from no offer. */
    assert(PSX_RB_SYNC_FLAG_RETAINED_LOAD != PSX_RB_SYNC_FLAG_NO_RETAINED_LOAD);
    /* Engine-owned extensions may not reinterpret shared protocol flags. */
    const unsigned shared_flags = RNET_RB_SYNC_FLAG_LIGHT_TIP |
        RNET_RB_SYNC_FLAG_REREPLAY | RNET_RB_SYNC_FLAG_MEDIA_KF |
        RNET_RB_SYNC_FLAG_PREFIX;
    assert(!(PSX_RB_SYNC_FLAG_RETAINED_LOAD & shared_flags));
    assert(!(PSX_RB_SYNC_FLAG_NO_RETAINED_LOAD & shared_flags));
    rbe_snap_ring_clear(g_snaps); store(22);
    assert(!joint_offer_below(20, 21, &offer));
    /* A pinned snapshot remains loadable after the ring removes its slot. */
    g_pin_valid = 1; g_pin_tick = 0;
    assert(joint_retained_load(0, 21));
    g_pin_valid = 0; g_boot_dig0_synced = 0;
    assert(!joint_retained_load(0, 21));
    /* A progress advert, including one from before this correction, cannot
     * prove that our current POST matched or release the original mismatch. */
    RNetRbConfig cfg = {0};
    RNetRollbackVTable vt = {0};
    RNetRbCorrection corr = {0};
    cfg.slot_count = 2;
    vt.load_state = inert_tick; vt.advance_sim = inert_tick; vt.get_input_row = no_row;
    g_rb = rnet_rb_create(&cfg, &vt); assert(g_rb);
    corr.epoch_id = 24; corr.mismatch_tick = 48; corr.load_tick = 47;
    corr.target_tick = 66; corr.slot = 1; corr.initiator = 1;
    rnet_rb_begin_episode(g_rb, &corr);
    assert(rnet_rb_is_active(g_rb));
    rnet_rb_set_phase(g_rb, nRNetRbPhaseVerify);
    g_joint_recovery = 0; /* independent completion scenario */
    joint_recovery_hold(48, 66, 1);
    assert(g_joint_mismatch == 48 && g_joint_target == 66);
    g_post_target = 66; g_local_post_sent = 1;
    apply_peer_resolved(1, 96);
    assert(rnet_rb_get_phase(g_rb) == nRNetRbPhaseVerify && g_joint_recovery);
    assert(tip_hold_calls == 0);
    g_episode_baseline_matched = 1;
    g_peer_commit_epoch = 16; g_peer_commit_tick = 66;
    assert(!joint_accept_peer_commit()); /* stale epoch */
    g_peer_commit_epoch = 24; g_peer_commit_tick = 65;
    assert(!joint_accept_peer_commit());
    g_peer_commit_tick = 67;
    assert(!joint_accept_peer_commit()); /* another tip is not our POST */
    g_peer_commit_tick = 66; g_local_post_sent = 0;
    assert(!joint_accept_peer_commit()); /* local replay still incomplete */
    g_local_post_sent = 1; g_episode_baseline_matched = 0;
    assert(!joint_accept_peer_commit());
    g_episode_baseline_matched = 1; g_joint_target = 67;
    assert(!joint_accept_peer_commit()); /* original correction not covered */
    g_joint_target = 66;
    assert(joint_accept_peer_commit());
    assert(ownership_calls == 1 && !g_joint_recovery && !g_joint_transfer);
    assert(!g_joint_load_valid && !g_joint_reopen && tip_hold_calls == 1);
    assert(!joint_accept_peer_commit()); /* idempotent */
    /* A direct matching POST follows the same production completion helper. */
    joint_recovery_hold(48, 66, 1); g_joint_transfer = g_joint_load_valid = 1;
    joint_recovery_post_match(65); assert(g_joint_recovery);
    joint_recovery_post_match(66); assert(!g_joint_recovery && !g_joint_transfer);
    /* Preserve the established ordinary-episode RESOLVED behavior. */
    rnet_rb_set_phase(g_rb, nRNetRbPhaseVerify);
    apply_peer_resolved(1, 96);
    assert(rnet_rb_get_phase(g_rb) == nRNetRbPhaseTipHold && tip_hold_calls == 2);
    rnet_rb_destroy(g_rb);
    rbe_snap_ring_destroy(g_snaps);
    puts("production retained-prefix and POST/COMMIT proof: PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-joint-prefix-') as directory:
        directory = Path(directory)
        c = directory / 'prefix.c'
        exe = directory / 'prefix.exe'
        c.write_text(fixture, encoding='utf-8')
        subprocess.run([
            args.compiler, '-std=c11', '-O2', '-Wall', '-Wextra',
            '-Wno-unused-variable', '-I', str(ROOT / 'runtime/include'),
            '-I', str(ROOT / 'lib/recomp-net/include'),
            '-I', str(ROOT / 'lib/retcomm-rbengine/include'), str(c),
            str(ROOT / 'lib/recomp-net/src/rollback/rnet_hash_confirm.c'),
            str(ROOT / 'lib/recomp-net/src/rollback/rnet_rollback.c'),
            str(ROOT / 'lib/recomp-net/src/input/rnet_input_contract.c'),
            str(ROOT / 'lib/retcomm-rbengine/src/snap/rbe_snap_ring.c'),
            '-o', str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
