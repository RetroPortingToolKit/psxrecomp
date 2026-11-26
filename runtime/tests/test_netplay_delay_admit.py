#!/usr/bin/env python3
"""Exercise PSX admission against the production RNet rings and confirmations."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_netplay_commit_lineage import function

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--recomp-net", type=Path, default=ROOT / "lib/recomp-net")
    parser.add_argument("--library", type=Path, required=True)
    args = parser.parse_args()
    source = (args.source / "runtime/src/psx_netplay.c").read_text(encoding="utf-8")
    phase_end = source.index("} NpXferPhase;") + len("} NpXferPhase;")
    phases = source[source.rindex("typedef enum", 0, phase_end):phase_end]
    legacy = "g_starv.latched" in function(source, "psx_netplay_poll_admit")
    helpers = ""
    if legacy:
        start = source.index("/* Delay-sync starvation hold")
        helpers = source[start:source.index("void psx_netplay_cold_reset", start)]
    else:
        helpers = "#define PSX_CATCHUP_CAP_DEFAULT 0\n" + function(source, "np_starv_env_int")
    fixture = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session/rnet_session.c"
''' + phases + r'''
static struct {
    RNetSession *session;
    int rollback, needs_advance, mc_sync_done, local_slot, slot_count, local_save_staged;
    int load_sync_done, load_ready_replied, load_applied_local, xfer_slot;
    NpXferPhase xfer;
} g_np;
static int s_host_profile;
static struct { int mismatch; } s_content;
static int profile_ready = 1, content_ready = 1, state_pending;
static int publishes, samples, pump_calls, rollback_calls, load_calls;
static unsigned future_confirms;
static rnet_u64 now = 1000;
static RNetSession session;
static int psx_netplay_active(void) { return g_np.session != NULL; }
static void np_pump_session(void) { ++pump_calls; }
static void psx_netplay_release_pads(void) {}
static void psx_netplay_diag_tick(void) {}
static int np_host_gate_ready(const void *p) { (void)p; return profile_ready; }
static const char *np_host_gate_error(const void *p) { (void)p; return "profile"; }
static int np_content_ready(const void *p) { (void)p; return content_ready; }
static void np_sched_set_admit_stall(const char *s) { (void)s; }
static int savestate_pending(void) { return state_pending; }
static int np_try_admit_load_barrier_unconfirmed(void) { ++load_calls; return 0; }
static int psx_netplay_rb_active(void) { return 0; }
static int psx_netplay_rb_load_pending(void) { return 0; }
static int psx_netplay_rb_tip_holding(void) { return 0; }
static int psx_netplay_rb_try_admit(void) { ++rollback_calls; return 0; }
static int np_try_admit_rollback(void) { ++rollback_calls; return 0; }
static void np_rb_produce_local_tip_for_sim(uint32_t t) { (void)t; }
static void np_maybe_stage_target_save(void) {}
static void force_session_pads_connected(int n) { (void)n; }
static int psx_netplay_remote_lead(void) {
    rnet_u32 tip;
    if (!rnet_session_remote_tip(&session, 1, &tip)) return -1;
    return (int)tip - (int)session.sim_tick;
}
static int psx_netplay_input_delay(void) { return session.delay; }
static unsigned psx_netplay_sim_tick(void) { return session.sim_tick; }
static int np_delay_sync_diag_enabled(void) { return 0; }
static int psx_start_bisect_no_catchup(void) { return 0; }
static int psx_netplay_rb_fmv_desync_hold(void) { return 0; }
static int psx_netplay_rb_post_fmv_heal_sticky(void) { return 0; }
''' + helpers + function(source, "np_try_admit_gameplay") + function(
        source, "psx_netplay_poll_admit") + function(
        source, "psx_netplay_catchup_budget") + r'''
static rnet_u64 clock_ms(void *ctx) { (void)ctx; return now; }
static void sample(rnet_u32 tick, RNetInputSample *out, void *ctx) {
    (void)ctx; ++samples;
    memset(out, 0, sizeof(*out)); out->tick=tick; out->valid=1;
    out->size=1; out->bytes[0]=0x22;
}
static void publish(rnet_u32 tick, const RNetInputSample *rows, int n, void *ctx) {
    (void)ctx; assert(tick==100 && n==2);
    assert(rows[0].bytes[0]==0x11 && rows[1].bytes[0]==0x33);
    ++publishes;
}
static int observe_send(void *ctx, const rnet_u8 *bytes, size_t n) {
    RNetDecodedPacket packet; (void)ctx;
    if (rnet_proto_decode(bytes,n,session.cfg.protocol_magic,&packet)==0 &&
        packet.type==RNET_PKT_INPUT_CONFIRM && packet.confirm_sim_tick==101)
        ++future_confirms;
    return (int)n;
}
static void row(RNetInputRing *ring, rnet_u32 tick, rnet_u8 value) {
    RNetInputSample input; memset(&input,0,sizeof(input));
    input.tick=tick; input.valid=1; input.size=1; input.bytes[0]=value;
    rnet_ring_store(ring,&input);
}
static void reset(void) {
    memset(&session,0,sizeof(session)); memset(&g_np,0,sizeof(g_np));
    rnet_config_init_defaults(&session.cfg);
    session.cfg.input_delay=session.delay=18;
    session.phase=RNET_PHASE_RUNNING; session.sim_tick=100;
    session.host.sample_local=sample; session.host.publish=publish;
    session.host.now_ms=clock_ms;
    session.transport.mode=RNET_TRANSPORT_ICE;
    session.transport.ice_send=observe_send;
    g_np.session=&session; g_np.mc_sync_done=1; g_np.slot_count=2;
    profile_ready=content_ready=1; state_pending=0;
    publishes=samples=pump_calls=rollback_calls=load_calls=0; future_confirms=0;
    now=1000;
    row(&session.local_ring,100,0x11); row(&session.local_ring,101,0x44);
    row(&session.remote_rings[1],100,0x33);
''' + ("    np_starv_reset();\n" if legacy else "") + r'''
}
static void confirm_current(int corrupt) {
    RNetDecodedPacket packet; memset(&packet,0,sizeof(packet));
    packet.type=RNET_PKT_INPUT_CONFIRM; packet.input_epoch=session.input_epoch;
    packet.session_id=session.cfg.session_id;
    packet.local_slot=1; packet.confirm_sim_tick=100;
    assert(session.published_valid[100%RNET_HISTORY_LENGTH]);
    packet.confirm_hash=session.published_hash[100%RNET_HISTORY_LENGTH] ^ corrupt;
    handle_decoded(&session,&packet);
}
static void wait_four(void) {
    for (int i=0;i<4;++i) {
        assert(!psx_netplay_poll_admit()); ++now;
        assert(session.sim_tick==100 && !publishes && !g_np.needs_advance);
    }
    assert(session.last_stall==RNET_ADMIT_WAIT_CONFIRM);
}
int main(void) {
    reset(); wait_four();
    /* A newly received near-future row must be confirmed while current input
     * agreement is still pending. Remote lead1 is intentionally below D18. */
    row(&session.remote_rings[1],101,0x55);
    assert(psx_netplay_remote_lead()==1);
    assert(!psx_netplay_poll_admit());
    if (!future_confirms) {
        fputs("FAIL: full-runway hold suppressed future INPUT_CONFIRM\n",stderr);
        return 3;
    }
    confirm_current(0);
    assert(psx_netplay_poll_admit());
    assert(publishes==1 && samples==1 && g_np.needs_advance && session.sim_tick==100);
    assert(psx_netplay_poll_admit() && publishes==1 && samples==1);

    /* Four waits followed by confirmation must also resume with no future
     * remote row at all; RNet's current agreed row is the admission contract. */
    reset(); wait_four(); confirm_current(0);
    assert(psx_netplay_remote_lead()==0);
    assert(psx_netplay_poll_admit() && publishes==1 && session.sim_tick==100);

    reset(); session.remote_rings[1].slots[100%RNET_HISTORY_LENGTH].valid=0;
    for(int i=0;i<8;++i) assert(!psx_netplay_poll_admit());
    assert(session.last_stall==RNET_ADMIT_WAIT_REMOTE_INPUT && !publishes);
    reset(); wait_four();
    for(int i=0;i<4;++i) assert(!psx_netplay_poll_admit());
    assert(!publishes && !g_np.needs_advance);
    reset(); wait_four(); confirm_current(1);
    assert(!psx_netplay_poll_admit() && session.input_desync && !publishes);
    reset(); session.state_stall_sim=1; session.tx.active=1;
    assert(!psx_netplay_poll_admit() && session.last_stall==RNET_ADMIT_STATE_XFER);
    assert(!samples && !publishes);

    /* Host ownership remains ahead of RNet; no profile/content/save or
     * rollback barrier is bypassed by the ordinary delay-sync change. */
    reset(); profile_ready=0; assert(!psx_netplay_poll_admit() && !samples);
    reset(); content_ready=0; assert(!psx_netplay_poll_admit() && !samples);
    reset(); g_np.mc_sync_done=0; assert(!psx_netplay_poll_admit() && !samples);
    reset(); session.phase=RNET_PHASE_LINKING;
    assert(!psx_netplay_poll_admit() && !samples);
    reset(); g_np.xfer=NP_XFER_LOAD_APPLYING;
    assert(!psx_netplay_poll_admit() && !samples && !load_calls);
    state_pending=1; assert(!psx_netplay_poll_admit() && load_calls==1 && !samples);
    reset(); g_np.rollback=1;
    assert(!psx_netplay_poll_admit() && rollback_calls==1 && !samples);
    /* Explicit optional pacing catch-up retains its cap and never consumes
     * the D-frame input runway. This is separate from actual admission. */
    reset(); row(&session.remote_rings[1],124,0x66);
    assert(psx_netplay_catchup_budget()==0);
#ifdef _WIN32
    _putenv_s("PSX_NET_CATCHUP_CAP","2");
#else
    setenv("PSX_NET_CATCHUP_CAP","2",1);
#endif
    assert(psx_netplay_catchup_budget()==2 && session.delay==18);
    session.remote_rings[1].slots[124%RNET_HISTORY_LENGTH].valid=0;
    assert(psx_netplay_catchup_budget()==0 && session.delay==18);
    puts("production delay admission / future confirm / real unready guards: PASS");
    return 0;
}
'''
    # Public RNet keeps its session opaque. Including its unchanged production
    # TU exposes actual rings for deterministic packet delivery; the normal
    # archive supplies transport/protocol/platform helpers without new sockets.
    with tempfile.TemporaryDirectory(prefix="psx-delay-admit-") as directory:
        c = Path(directory) / "admit.c"
        exe = Path(directory) / ("admit.exe" if sys.platform == "win32" else "admit")
        c.write_text(fixture, encoding="utf-8")
        command = [args.compiler, "-std=c11", "-O1", "-Wall", "-Wextra",
                   "-Wno-unused-function", "-Wno-unused-variable", "-ffunction-sections",
                   "-fdata-sections", "-I", str(args.recomp_net / "include"),
                   "-I", str(args.recomp_net / "src"), str(c), str(args.library),
                   "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                   "-o", str(exe)]
        if sys.platform == "win32":
            command += ["-lws2_32", "-liphlpapi", "-lbcrypt"]
        subprocess.run(command, check=True)
        environment = {k: v for k, v in os.environ.items()
                       if not k.upper().startswith(("PSX_", "RNET_"))}
        subprocess.run([str(exe)], env=environment, check=True)


if __name__ == "__main__":
    main()
