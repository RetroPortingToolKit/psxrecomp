#!/usr/bin/env python3
"""Execute production retained-prefix selection against real HC/snapshot rings.

This is a retention/epoch-range gate, not a guest reconvergence test. Native
changing-input peers must additionally qualify BEGIN, restore, replay and POST.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    args = parser.parse_args()
    source = (ROOT / 'runtime/src/psx_netplay_rb.c').read_text(encoding='utf-8')
    declarations = source[source.index('#define PSX_RB_SYNC_FLAG_RETAINED_LOAD'):
                          source.index('/* BASELINE that arrived before follow/begin')]
    functions = ''
    for signature in (
        'static int joint_retained_load(uint32_t tick, uint32_t mismatch)',
        'static int joint_offer_below(uint32_t ceiling, uint32_t mismatch, uint32_t *offer)',
        'static void joint_recovery_hold(uint32_t mismatch, uint32_t target, int slot)',
        'static int joint_host_transfer_load(uint32_t mismatch, uint32_t target, uint32_t *load)',
        'static uint32_t follower_frontier_hint(void)',
    ):
        functions += signature + '\n{' + function_body(source, signature + '\n{') + '}\n'
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "netplay_hash_confirm.h"
#include "recomp_net/rollback.h"
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
''' + declarations + functions + r'''
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
    rbe_snap_ring_clear(g_snaps); store(22);
    assert(!joint_offer_below(20, 21, &offer));
    /* A pinned snapshot remains loadable after the ring removes its slot. */
    g_pin_valid = 1; g_pin_tick = 0;
    assert(joint_retained_load(0, 21));
    g_pin_valid = 0; g_boot_dig0_synced = 0;
    assert(!joint_retained_load(0, 21));
    rbe_snap_ring_destroy(g_snaps);
    puts("production retained-prefix selection: PASS (old hint is not retained)");
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
            str(ROOT / 'lib/retcomm-rbengine/src/snap/rbe_snap_ring.c'),
            '-o', str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
