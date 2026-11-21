#!/usr/bin/env python3
"""Exercise production rematch cleanup without a game or network transport."""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body


def check_load_timeline(compiler, source):
    cold = function_body(source, "void psx_netplay_rb_cold_reset(void)\n{")
    names = sorted(set(re.findall(r"\b(g_\w+)\s*=", cold)))
    declarations = []
    for name in names:
        found = re.search(r"^static\s+(?:int|u?int(?:8|16|32|64)_t)\s+" + name + r"\s*(?:=[^;]*)?;", source, re.M)
        assert found, name
        declarations.append(found.group(0))
    mdec = (Path(__file__).resolve().parents[1] / "src/mdec.c").read_text()
    functions = "int mdec_recently_active(uint32_t within_frames)\n{" + function_body(
        mdec, "int mdec_recently_active(uint32_t within_frames)") + "}\n"
    for signature in (
        "static int rb_fmv_media_active(void)",
        "static void rb_clear_fmv_desync_hold(const char *why)",
        "void psx_netplay_rb_clear_fmv_desync_hold(const char *why)",
        "static void rb_fmv_tick_settle(void)",
        "static int rb_in_fmv_defer_rewind_window(void)",
        "static int rb_in_fmv_lockstep_window(void)",
        "void psx_netplay_rb_cold_reset(void)",
        "void psx_netplay_rb_restart_after_load(void)",
    ):
        # Skip declarations and disabled-netplay stubs.
        match = list(re.finditer(re.escape(signature) + r"\s*\n\{", source))[-1]
        functions += signature + "\n{" + function_body(source[match.start():], signature) + "}\n"
    constants = "\n".join(re.findall(r"^#define RB_FMV_(?:SETTLE_TICKS|LOCKSTEP_MIN|LOCKSTEP_MAX|LOCKSTEP_CONFIRM|MDEC_HYSTERESIS)\b[^\n]*", source, re.M))
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + constants + "\n" + "\n".join(declarations) + r'''
typedef struct { uint32_t tick; } RNetSession;
typedef struct { int reading, mode_reg; } CDROMDebugState;
static RNetSession session;
static void *g_rb;
static struct { void *hc; } g_b;
static int media, restart_calls, restart_clear;
static uint64_t psx_cycle_count, mdec_last_color_decode_cycle;
static RNetSession *sess(void) { return &session; }
static uint32_t rnet_session_sim_tick(RNetSession *s) { return s->tick; }
static int rnet_rb_is_tip_holding(void *p) { (void)p; return 0; }
static uint32_t rnet_rb_get_target_tick(void *p) { (void)p; return 0; }
static void finalize_tip_hold(void) { assert(0); }
static void cdrom_debug_snapshot(CDROMDebugState *s) {
    memset(s,0,sizeof(*s));s->reading=1;s->mode_reg=0xe0;
}
static int gpu_display_is_depth24(void) { return media; }
static int cdrom_xa_stream_active(void) { return 0; }
static int cdrom_fmv_stream_pending(void) { return 1; }
static int gpu_vblank_present_pending(void) { return 0; }
static void netplay_hc_prime_after(void *p, uint32_t t) { (void)p; (void)t; }
static void rb_fmv_update_lockstep_gate(uint32_t t) { (void)t; }
static void rb_fmv_desync_invent_maybe_expire(uint32_t t) { (void)t; }
static int rb_media_kf_enabled(void) { return 0; }
static int rb_post_fmv_heal_sticky_invent_hold(uint32_t t) { (void)t; return 0; }
static void rb_clear_post_fmv_platform_nondet(const char *why) { (void)why; }
static void clear_rewind_cooldown(const char *why) { (void)why; }
static void tip_dense_reset(void) {}
static void rb_reset_session_peer_state(void) {}
static void clear_episode_wire_state(void) {}
static void clear_baseline_pin(void) {}
void psx_netplay_rb_cold_reset(void);
/* Call observer for the existing ring/episode constructor. Its full wiring
 * and real post-load episode are checked separately; frame reset below is
 * the actual production cold-reset body, without a copied deadline policy. */
static void rb_start(int clear_guest_resume) {
    ++restart_calls;restart_clear=clear_guest_resume;
    psx_netplay_rb_cold_reset();
}
''' + functions + r'''
int main(int argc, char **argv) {
    (void)argv;
    session.tick=0;
    /* First ordinary sample after a user load: historical device activity is
     * nonzero, but its age in restored guest cycles is expired. Resetting
     * only the network clock must not fabricate a fresh movie. */
    psx_cycle_count=60000000;mdec_last_color_decode_cycle=5000000;
    g_fmv_settle_until=5240;g_fmv_lockstep_until=5396;
    g_fmv_media_end_sim=5216;g_fmv_dense_through=5460;
    g_fmv_media_lo=4000;g_fmv_media_hi=5216;g_auth_snap_through=5586;
    if(argc>1) psx_netplay_rb_clear_fmv_desync_hold("netplay load sync");
    else psx_netplay_rb_restart_after_load();
    if(rb_in_fmv_defer_rewind_window() || rb_in_fmv_lockstep_window() ||
       g_auth_snap_through) {
        fputs("FAIL: old frame domain survived coordinated load\n",stderr);return 3;
    }
    assert(restart_calls==1 && restart_clear==0);
    assert(!g_fmv_media_end_sim && !g_fmv_media_hi && !g_fmv_dense_through);
    assert(!g_was_in_fmv && !mdec_recently_active(RB_FMV_MDEC_HYSTERESIS));
    assert(psx_cycle_count==60000000 && mdec_last_color_decode_cycle==5000000);
    /* No load: the current deadline is still a real protection window. */
    session.tick=100;g_fmv_settle_until=150;g_fmv_lockstep_until=200;
    assert(rb_in_fmv_defer_rewind_window() && rb_in_fmv_lockstep_window());
    assert(g_fmv_settle_until==150 && restart_calls==1);
    /* A restored active movie remains protected by the ordinary observer.
     * Its eventual end arms fresh deadlines in the new clock domain. */
    media=1;session.tick=0;
    psx_netplay_rb_restart_after_load();
    assert(rb_in_fmv_defer_rewind_window() && rb_in_fmv_lockstep_window());
    assert(g_was_in_fmv);
    media=0;session.tick=1;
    assert(rb_in_fmv_defer_rewind_window());
    assert(g_fmv_settle_until==1+RB_FMV_SETTLE_TICKS);
    assert(g_fmv_lockstep_until==1+RB_FMV_LOCKSTEP_MIN);
    session.tick=1+RB_FMV_SETTLE_TICKS;
    assert(!rb_in_fmv_defer_rewind_window());
    /* A genuinely recent restored decode also remains a real media input,
     * even without depth24 output or active XA. */
    session.tick=0;mdec_last_color_decode_cycle=psx_cycle_count;
    psx_netplay_rb_restart_after_load();
    assert(rb_in_fmv_defer_rewind_window() && rb_in_fmv_lockstep_window());
    assert(g_was_in_fmv);
    puts("production coordinated-load timeline reset and restored-media protection: PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="psx-rb-load-era-") as directory:
        path = Path(directory) / "load.c"
        exe = Path(directory) / "load.exe"
        path.write_text(fixture)
        subprocess.run([compiler, "-std=c11", "-O2", str(path), "-o", str(exe)], check=True)
        assert subprocess.run([str(exe), "--legacy-desync-clear"]).returncode == 3
        subprocess.run([str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / "src/psx_netplay_rb.c").read_text()
    reset = function_body(source, "static void rb_reset_session_peer_state(void)")
    progress = function_body(source, "static int rb_owing_seats_progressed(uint32_t owing)")
    replaying = function_body(source, "static int rb_owing_seats_replaying(uint32_t owing, uint32_t cap)")
    verify = source[source.index("/* Retransmit POST while Verify waits for peer"):]
    verify = verify[:verify.index("poll_tip_hold_finalize();")]
    # A Live seat past the episode target must not keep re-arming the POST wait.
    assert "rb_owing_seats_replaying(rnet_rb_quorum_missing(&g_post_q)," in verify
    assert "rnet_rb_get_target_tick(g_rb)" in verify
    # Verify the executable helper is wired into shutdown/cold reset, not into
    # per-episode clearing: early BASELINE packets must survive opening BEGIN.
    enabled = source.index("static void rb_start(int clear_guest_resume)\n{")
    start = function_body(source[enabled:], "static void rb_start(int clear_guest_resume)")
    cold = function_body(source[enabled:], "void psx_netplay_rb_cold_reset(void)")
    shutdown = function_body(source[enabled:], "static void rb_shutdown(int clear_guest_resume)")
    assert "rb_shutdown(clear_guest_resume);" in start
    assert "psx_netplay_rb_cold_reset();" in shutdown
    assert start.index("rb_shutdown(clear_guest_resume)") < start.index("netplay_snap_ring_create(")
    assert "netplay_snap_ring_destroy(g_snaps)" in shutdown
    for body in (start, shutdown):
        assert re.search(r"if \(clear_guest_resume\)\s+psx_irq_clear_resume_latches\(\);", body)
    assert "rb_start(0);" in function_body(source[enabled:], "void psx_netplay_rb_restart_after_load(void)")
    assert "rb_start(1);" in function_body(source[enabled:], "void psx_netplay_rb_start(void)")
    assert "rb_shutdown(1);" in function_body(source[enabled:], "void psx_netplay_rb_shutdown(void)")
    application = (Path(__file__).resolve().parents[1] / "src/psx_netplay.c").read_text()
    commit = function_body(application, "static void np_commit_load_sync(void)\n{")
    assert commit.index("rnet_session_hard_resync(") < commit.index("psx_netplay_rb_restart_after_load();")
    assert "psx_netplay_host_profile" not in commit
    check_load_timeline(args.compiler, source)
    assert "rb_reset_session_peer_state();" in cold
    episode = function_body(source, "static void clear_episode_wire_state(void)")
    assert "rb_reset_session_peer_state" not in episode
    declarations = source[source.index("static struct {\n    int valid;\n    uint32_t epoch, load, dig_m"):
                          source.index("static uint32_t g_peer_post_digest;")]
    declarations += source[source.index("#define PSX_RB_SYNC_FLAG_RETAINED_LOAD"):
                           source.index("/* BASELINE that arrived before follow/begin")]
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define RNET_RB_QUORUM_SEATS 4
typedef uint32_t rnet_u32;
typedef struct { uint32_t tip[4]; } RNetSession;
static RNetSession s_session;
static RNetSession *sess(void) { return &s_session; }
static int rnet_session_remote_tip(RNetSession *s, int seat, rnet_u32 *tip) {
    *tip = s->tip[seat]; return 1;
}
''' + declarations + "\nstatic void rb_reset_session_peer_state(void) {" + reset + "}\n" + \
        "static int rb_owing_seats_progressed(uint32_t owing) {" + progress + "}\n" + \
        "static int rb_owing_seats_replaying(uint32_t owing, uint32_t cap) {" + replaying + r'''}
int main(void) {
    for (int seat = 0; seat < 4; ++seat) {
        g_stash_bl_seat[seat].valid = 1;
        g_stash_bl_seat[seat].epoch = 9;
        s_session.tip[seat] = 900;
    }
    g_join_valid = g_joining = 1;
    g_join_epoch = g_join_mismatch = g_join_load = g_join_target = 900;
    g_join_slot = 3; g_join_flags = 7;
    g_joint_transfer = 1;
    g_joint_recovery = g_joint_load_valid = g_joint_reopen = 1;
    g_joint_mismatch = 21; g_joint_target = 28; g_joint_load = 16; g_joint_slot = 1;
    assert(!rb_owing_seats_progressed(15));
    for (int seat = 0; seat < 4; ++seat) s_session.tip[seat]++;
    assert(rb_owing_seats_progressed(15));
    rb_reset_session_peer_state();
    assert(!g_join_valid && !g_joining && !g_join_epoch && !g_join_mismatch);
    assert(!g_join_load && !g_join_target && !g_join_slot && !g_join_flags);
    assert(!g_joint_transfer);
    assert(!g_joint_recovery && !g_joint_load_valid && !g_joint_reopen);
    assert(!g_joint_mismatch && !g_joint_target && !g_joint_load && !g_joint_slot);
    for (int seat = 0; seat < 4; ++seat) {
        assert(!g_stash_bl_seat[seat].valid && !g_stash_bl_seat[seat].epoch);
        s_session.tip[seat] = 1;
    }
    assert(!rb_owing_seats_progressed(15));
    for (int seat = 0; seat < 4; ++seat) {
        s_session.tip[seat]++;
        assert(rb_owing_seats_progressed(1u << seat));
    }
    assert(!rb_owing_seats_progressed(15));
    /* Verify wait: progress up to the target re-arms, a seat already past
     * the target (Live, its POST lost) does not. */
    s_session.tip[1] = 100; assert(rb_owing_seats_replaying(2u, 200));
    s_session.tip[1] = 150; assert(rb_owing_seats_replaying(2u, 200));
    s_session.tip[1] = 250; assert(!rb_owing_seats_replaying(2u, 200));
    s_session.tip[1] = 260; assert(!rb_owing_seats_replaying(2u, 200));
    rb_reset_session_peer_state();
    rb_reset_session_peer_state(); /* shutdown/reset is idempotent */
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="psx-rb-reset-") as tmp:
        path = Path(tmp) / "reset.c"
        exe = Path(tmp) / "reset.exe"
        path.write_text(fixture)
        subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("production peer state cleanup and four-seat rematch progress: PASS")


if __name__ == "__main__":
    main()
