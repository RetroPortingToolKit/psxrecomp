#!/usr/bin/env python3
"""
cycle_compare.py — native<->Beetle per-anchor guest-cycle comparator.

Arms the SAME guest-PC anchor on both backends via the `cyc_watch` command,
lets both run from boot, then dumps and diffs the recorded cycle ring per
hit_index. Detects fine per-instruction cycle DRIFT (e.g. the -8 class) that
the gross per-frame rate (~565k/frame, already matched) hides — by comparing
elapsed cycles AT THE SAME GUEST ANCHOR on both backends.

CAPTURE SEMANTICS (must be identical on both sides):
  The anchor is sampled at BLOCK ENTRY, BEFORE the anchor instruction (the
  block leader at the anchor PC) executes. The recorded `cycles` is the
  absolute guest cycle count charged for all PRIOR blocks, not including any
  cycle of the anchor block itself. Anchor must be a basic-block leader PC
  (function entry / branch target), not a mid-block PC.

The native side (psx-runtime) implements `cyc_watch` / `cyc_watch_dump` /
`cyc_watch_clear` in runtime/src/debug/debug_server.c. The Beetle side
(psx-beetle) implements the SAME commands to the SAME spec (added parent-side).

Wire format (request):
    {"cmd":"cyc_watch","pc":"0x80012345","n":16}
    {"cmd":"cyc_watch_dump"}
    {"cmd":"cyc_watch_clear"}

Wire format (cyc_watch_dump response):
    {
      "ok": true,
      "anchor": "0x80012345",        # as supplied
      "anchor_phys": "0x00012345",   # masked to physical (pc & 0x1FFFFFFF)
      "armed": 0|1,
      "max_hits": 16,
      "hits": <N recorded>,
      "entries": [
        {"hit_index": 0, "pc": "0x00012345", "cycles": 1234567},
        ...
      ]
    }

Usage:
    python cycle_compare.py <anchor_pc_hex> [--hits N] [--wait SECONDS]
                            [--native-port P] [--beetle-port P] [--host H]
                            [--no-arm] [--clear]

    <anchor_pc_hex>   Guest PC to anchor (e.g. 0x80012345). A block leader.

Options:
    --hits N          Max anchor hits to record on each side (default 16).
    --wait SECONDS    Seconds to let both backends run after arming before
                      dumping (default 5.0). Both must already be running.
    --native-port P   psx-runtime debug port (default 4500).
    --beetle-port P   psx-beetle debug port (default 4382).
    --host H          Host for both (default 127.0.0.1).
    --no-arm          Skip arming; just dump+diff what's already recorded.
    --clear           Clear both rings and exit (no arm, no dump).

Exit status:
    0  comparison completed (drift may or may not be present)
    2  a backend was unreachable or its cyc_watch command is missing
"""

import argparse
import json
import socket
import sys
import time

DEFAULT_HOST = "127.0.0.1"
DEFAULT_NATIVE_PORT = 4500
DEFAULT_BEETLE_PORT = 4382


# ---------------------------------------------------------------------------
# Transport (brace-balanced, string-aware reader; mirrors debug_client.py)
# ---------------------------------------------------------------------------

def _recv_one_json(sock):
    buf = bytearray()
    pos = 0
    depth = 0
    in_str = False
    esc = False
    started = False
    while True:
        chunk = sock.recv(1 << 20)
        if not chunk:
            break
        buf.extend(chunk)
        n = len(buf)
        while pos < n:
            if in_str:
                if esc:
                    esc = False
                    pos += 1
                    continue
                q = buf.find(b'"', pos)
                bs = buf.find(b"\\", pos)
                if q == -1 and bs == -1:
                    pos = n
                    break
                if bs != -1 and (q == -1 or bs < q):
                    esc = True
                    pos = bs + 1
                    continue
                in_str = False
                pos = q + 1
                continue
            c = buf[pos]
            if c == 0x22:        # '"'
                in_str = True
            elif c == 0x7B:      # '{'
                depth += 1
                started = True
            elif c == 0x7D:      # '}'
                depth -= 1
                if started and depth == 0:
                    return json.loads(buf[:pos + 1].decode())
            pos += 1
    if not buf:
        raise ConnectionError("empty response")
    return json.loads(buf.decode().strip())


def query(host, port, cmd_dict, timeout=10.0):
    """One-shot: connect, send one command, receive one JSON object, close."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((host, port))
    try:
        s.sendall((json.dumps(cmd_dict) + "\n").encode())
        return _recv_one_json(s)
    finally:
        s.close()


# ---------------------------------------------------------------------------
# Backend helpers
# ---------------------------------------------------------------------------

class BackendError(Exception):
    pass


def _is_unknown_command(resp):
    """Heuristic: the backend answered, but doesn't know cyc_watch yet."""
    if not isinstance(resp, dict):
        return True
    if resp.get("ok") is True:
        return False
    err = str(resp.get("error", "")).lower()
    return ("unknown" in err or "unrecognized" in err or
            "no such" in err or "not found" in err or err == "")


def arm(host, port, label, anchor, hits, end=None):
    cmd = {"cmd": "cyc_watch", "pc": anchor, "n": hits}
    if end:
        cmd["end"] = end   # two-anchor REGION mode: each entry = Δcycles of one A->B pass
    try:
        resp = query(host, port, cmd)
    except (ConnectionRefusedError, TimeoutError, OSError, ConnectionError) as e:
        raise BackendError(f"{label} ({host}:{port}) unreachable: {e}")
    if _is_unknown_command(resp):
        raise BackendError(
            f"{label} ({host}:{port}) does not implement 'cyc_watch' yet "
            f"(response: {json.dumps(resp)}). The Beetle side must add the "
            f"cyc_watch / cyc_watch_dump / cyc_watch_clear commands to the "
            f"same spec as native (see header).")
    return resp


def dump(host, port, label):
    try:
        resp = query(host, port, {"cmd": "cyc_watch_dump"})
    except (ConnectionRefusedError, TimeoutError, OSError, ConnectionError) as e:
        raise BackendError(f"{label} ({host}:{port}) unreachable: {e}")
    if _is_unknown_command(resp):
        raise BackendError(
            f"{label} ({host}:{port}) does not implement 'cyc_watch_dump' yet "
            f"(response: {json.dumps(resp)}).")
    return resp


def clear(host, port, label):
    try:
        query(host, port, {"cmd": "cyc_watch_clear"})
    except (ConnectionRefusedError, TimeoutError, OSError, ConnectionError) as e:
        raise BackendError(f"{label} ({host}:{port}) unreachable: {e}")


def entries_by_index(resp):
    """Map hit_index -> absolute cycles from a cyc_watch_dump response."""
    out = {}
    for e in resp.get("entries", []):
        out[int(e["hit_index"])] = int(e["cycles"])
    return out


# ---------------------------------------------------------------------------
# Diff / report
# ---------------------------------------------------------------------------

def report(anchor, native_resp, beetle_resp):
    nat = entries_by_index(native_resp)
    bet = entries_by_index(beetle_resp)

    n_phys = native_resp.get("anchor_phys", "?")
    b_phys = beetle_resp.get("anchor_phys", "?")

    print(f"=== cyc_watch compare @ anchor {anchor} ===")
    print(f"native anchor_phys={n_phys} hits={native_resp.get('hits', 0)}  "
          f"beetle anchor_phys={b_phys} hits={beetle_resp.get('hits', 0)}")
    if n_phys != b_phys:
        print(f"WARNING: anchor_phys differs between backends "
              f"(native {n_phys} vs beetle {b_phys}) — they are not "
              f"sampling the same PC.")
    if not nat:
        print("native recorded NO hits — anchor never reached on native "
              "(not a block leader, or not executed in the window).")
    if not bet:
        print("beetle recorded NO hits — anchor never reached on beetle.")

    print()
    print(f"{'hit':>4}  {'native_cycles':>16}  {'beetle_cycles':>16}  "
          f"{'delta(n-b)':>12}  {'d_native':>10}  {'d_beetle':>10}")
    print("-" * 78)

    common = sorted(set(nat) | set(bet))
    first_drift = None
    deltas = []
    prev_n = prev_b = None
    for i in common:
        n = nat.get(i)
        b = bet.get(i)
        n_s = str(n) if n is not None else "(none)"
        b_s = str(b) if b is not None else "(none)"
        if n is not None and b is not None:
            delta = n - b
            deltas.append((i, delta))
            if delta != 0 and first_drift is None:
                first_drift = (i, delta)
            d_str = str(delta)
        else:
            d_str = "(missing)"
        dn = str(n - prev_n) if (n is not None and prev_n is not None) else ""
        db = str(b - prev_b) if (b is not None and prev_b is not None) else ""
        print(f"{i:>4}  {n_s:>16}  {b_s:>16}  {d_str:>12}  {dn:>10}  {db:>10}")
        if n is not None:
            prev_n = n
        if b is not None:
            prev_b = b

    print()
    if first_drift is not None:
        idx, d = first_drift
        print(f"DRIFT ONSET: first delta != 0 at hit_index {idx} (delta {d}).")
        # Steady delta: the most common nonzero delta among the tail.
        tail = [d for (_, d) in deltas if _ >= idx]
        if tail:
            from collections import Counter
            steady, cnt = Counter(tail).most_common(1)[0]
            print(f"STEADY DELTA: {steady} (cycles, native - beetle; "
                  f"{cnt}/{len(tail)} of post-onset hits).")
        # Per-hit growth gives the drift RATE.
        if len(deltas) >= 2:
            growth = deltas[-1][1] - deltas[0][1]
            span = deltas[-1][0] - deltas[0][0]
            if span > 0:
                print(f"DRIFT RATE: ~{growth/span:+.2f} cycles per anchor hit "
                      f"(delta {deltas[0][1]} -> {deltas[-1][1]} over "
                      f"{span} hits).")
    elif deltas:
        print("NO DRIFT: native and beetle elapsed cycles match at every "
              "compared hit_index.")
    else:
        print("NO COMPARABLE HITS: at least one backend recorded nothing.")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(add_help=True, description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("anchor", nargs="?", help="anchor guest PC (region START), hex (e.g. 0x80012345)")
    p.add_argument("--end", default=None,
                   help="region END PC, hex. Enables two-anchor REGION mode: each "
                        "recorded value is Δcycles of one START->END pass (the cost of "
                        "the known code path between two dispatch points). Both anchors "
                        "must be dispatch points (function entry / CPS continuation / "
                        "dirty block). Omit for single-anchor (absolute-cycles-at-anchor).")
    p.add_argument("--hits", type=int, default=16)
    p.add_argument("--wait", type=float, default=5.0)
    p.add_argument("--native-port", type=int, default=DEFAULT_NATIVE_PORT)
    p.add_argument("--beetle-port", type=int, default=DEFAULT_BEETLE_PORT)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--no-arm", action="store_true",
                   help="skip arming; dump+diff already-recorded rings")
    p.add_argument("--clear", action="store_true",
                   help="clear both rings and exit")
    opts = p.parse_args()

    host = opts.host
    np_, bp_ = opts.native_port, opts.beetle_port

    if opts.clear:
        try:
            clear(host, np_, "native")
            clear(host, bp_, "beetle")
            print("Cleared cyc_watch rings on both backends.")
        except BackendError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 2
        return 0

    if not opts.anchor:
        p.error("anchor PC is required (unless --clear)")

    anchor = opts.anchor
    if not anchor.lower().startswith("0x"):
        anchor = "0x" + anchor

    try:
        if not opts.no_arm:
            r_n = arm(host, np_, "native", anchor, opts.hits, end=opts.end)
            r_b = arm(host, bp_, "beetle", anchor, opts.hits, end=opts.end)
            print(f"Armed native: {json.dumps(r_n)}")
            print(f"Armed beetle: {json.dumps(r_b)}")
            print(f"Letting both run for {opts.wait:.1f}s ...")
            time.sleep(opts.wait)

        native_resp = dump(host, np_, "native")
        beetle_resp = dump(host, bp_, "beetle")
    except BackendError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2

    print()
    report(anchor, native_resp, beetle_resp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
