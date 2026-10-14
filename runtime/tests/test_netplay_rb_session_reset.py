#!/usr/bin/env python3
"""Exercise production rematch cleanup without a game or network transport."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_netplay_load_apply_flow import function_body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    source = (Path(__file__).resolve().parents[1] / "src/psx_netplay_rb.c").read_text(encoding="utf-8")
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
    enabled = source.index("void psx_netplay_rb_start(void)\n{")
    start = function_body(source[enabled:], "void psx_netplay_rb_start(void)")
    cold = function_body(source[enabled:], "void psx_netplay_rb_cold_reset(void)")
    shutdown = function_body(source[enabled:], "void psx_netplay_rb_shutdown(void)")
    assert "psx_netplay_rb_shutdown();" in start
    assert "psx_netplay_rb_cold_reset();" in shutdown
    assert "rb_reset_session_peer_state();" in cold
    episode = function_body(source, "static void clear_episode_wire_state(void)")
    assert "rb_reset_session_peer_state" not in episode
    declarations = source[source.index("static struct {\n    int valid;\n    uint32_t epoch, load, dig_m"):
                          source.index("static uint32_t g_peer_post_digest;")]
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
    assert(!rb_owing_seats_progressed(15));
    for (int seat = 0; seat < 4; ++seat) s_session.tip[seat]++;
    assert(rb_owing_seats_progressed(15));
    rb_reset_session_peer_state();
    assert(!g_join_valid && !g_joining && !g_join_epoch && !g_join_mismatch);
    assert(!g_join_load && !g_join_target && !g_join_slot && !g_join_flags);
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
