#!/usr/bin/env python3
"""Guard that a rollback follower never revives a dead BEGIN epoch.

test_netplay_peer_frontier.c proves the rule (netplay_rb_begin_dead). This
proves psx_netplay_rb.c applies it: every BEGIN, including a stashed one
applied later and a deferred join, passes the check before begin_follower can
refuse, stash or follow it; every follow NACK records its epoch as refused;
and the record is cleared wherever the epoch counter restarts. The follower
path needs a live session and a second peer, so the wiring is checked in the
source, as test_netplay_lan_identity_wiring.py does for the LAN lobby.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
RB = (ROOT / "runtime" / "src" / "psx_netplay_rb.c").read_text(encoding="utf-8")


def body(signature: str) -> str:
    """The text of the function defined at `signature`, to its closing brace."""
    start = RB.index(signature + "\n{")
    depth = 0
    for i in range(RB.index("{", start), len(RB)):
        if RB[i] == "{":
            depth += 1
        elif RB[i] == "}":
            depth -= 1
            if depth == 0:
                return RB[start : i + 1]
    raise AssertionError(f"unterminated body: {signature}")


follow = body("static void begin_follower(uint32_t epoch, uint32_t mismatch, uint32_t load, uint32_t target,\n"
              "                           int slot, uint8_t wire_flags)")
dead = follow.index("netplay_rb_begin_dead(&g_begin_seen, epoch, RB_EPOCH_SLOT_MASK)")
heard = follow.index("netplay_rb_begin_seen_note(&g_begin_seen, epoch, RB_EPOCH_SLOT_MASK, 0)")
assert dead < heard, "a BEGIN is judged before it is recorded as heard"
assert follow[dead:heard].rstrip().endswith("RB_EPOCH_SLOT_MASK))\n        return;"), \
    "a dead epoch is dropped"
for later in ("stash_peer_begin(", "follow_refuse_nack(", "rnet_rb_begin_episode(",
              "g_join_valid = 1"):
    for match in re.finditer(re.escape(later), follow):
        assert match.start() > heard, f"{later} is reachable before the dead-epoch check"

# Stashed BEGINs and deferred joins re-enter through begin_follower.
assert "begin_follower(epoch, mismatch, load, target, slot, flags);" in body(
    "static void apply_stashed_begin(void)")
assert RB.count("begin_follower(g_join_epoch, g_join_mismatch, g_join_load,") == 1

# Every follow NACK ends its epoch here as it does at the initiator.
nack = body("static void send_follow_nack(uint32_t epoch, uint32_t mismatch, uint32_t load,\n"
            "                             uint32_t target, int slot, int log_line)")
assert nack.index("netplay_rb_begin_seen_note(&g_begin_seen, epoch, RB_EPOCH_SLOT_MASK, 1)") < \
    nack.index("RNET_RB_SYNC_OP_NACK"), "the refusal is recorded with the NACK it sends"

# Epoch counters restart together with the record of heard epochs.
restarts = [m.start() for m in re.finditer(r"\n    g_epoch = 0;", RB)]
assert restarts, "epoch counter restart not found"
for pos in restarts:
    fn_start = RB.rindex("\n{", 0, pos)
    fn_end = RB.index("\n}\n", pos)
    assert "netplay_rb_begin_seen_reset(&g_begin_seen);" in RB[fn_start:fn_end], \
        "an epoch counter restart keeps the old record of heard epochs"

print("rollback dead-epoch BEGIN wiring: PASS")
