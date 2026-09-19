#!/usr/bin/env python3
"""t2probe.py - native-vs-interp FIRST-DIVERGENCE probe for any PSX recomp title.

Dev tooling (NOT shipped in releases). Pairs with the debug-server commands in
runtime/src/debug/debug_server.c (all PSX_NO_DEBUG_TOOLS-stripped in release):
  frame_fingerprint, record_frame, record_frame_dump, record_reads_dump
and the boot env seeds PSX_RECORD_FRAME and PSX_READ_WATCH (debug_server_init),
PSX_OVERLAY_NATIVE_OFF (overlay_loader.c, forces full dirty-interp = the oracle).

METHOD (see README.md): two deterministic runs of the same title (native vs the
interp reference) are byte-identical until a codegen/timing bug forks them.
  Layer 1  always-on per-frame fingerprint ring -> O(1) first divergent FRAME.
  Layer 2  frame-gated unified ordered recorder -> first divergent ACCESS.
  Layer 3  targeted read-watch + per-entry cycle count -> value vs pointer vs
           timing classification at the fork.

Commands:
  fpcap  <port> <out.json> [count=4096] [flo=-1] [fhi=-1]
  fpdiff <a.json> <b.json>                 first divergent frame per column
                                           (wr/pc/mmio/sp/cyc)
  recarm <port> <frame>                    arm the unified recorder (or seed
                                           PSX_RECORD_FRAME at boot - race-free)
  recdump <port> <out.json>                drain the unified ordered access log
  recdiff <a.json> <b.json>                first divergent access incl. pc
  recdiff_state <a.json> <b.json>          first divergent RAM-WRITE (addr,val
                                           only; ignores pc + reads) = real state
                                           fork. Use this, not recdiff, once the
                                           backends differ in store-PC attribution
                                           or MMIO read interleaving.

Tip: a `pc`-only difference in recdiff is usually a benign store-PC attribution
artifact (e.g. a RAM routine aliased to a byte-identical BIOS body); recdiff_state
finds the genuine machine-state fork. Per-entry `cyc` (psx_get_cycle_count)
distinguishes a timing/timer fork from a data fork.
"""
import json, sys, os

# Resolve the sibling tools/ dir (holds debug_client.py) relative to this file.
_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)
from debug_client import connect, send_cmd


def q(port, d):
    s = connect(port=int(port))
    try:
        return send_cmd(s, d)
    finally:
        s.close()


def fpcap(port, out, count=4096, flo=-1, fhi=-1):
    r = q(port, {"cmd": "frame_fingerprint", "count": int(count),
                 "frame_lo": int(flo), "frame_hi": int(fhi)})
    ents = r.get("entries", [])
    json.dump(ents, open(out, "w"))
    print(f"captured {len(ents)} frames (total={r.get('total')}) -> {out}")
    if ents:
        print(f"  frame range {ents[0]['frame']}..{ents[-1]['frame']}")


def fpdiff(a, b):
    A = json.load(open(a)); B = json.load(open(b))
    ma = {e["frame"]: e for e in A}; mb = {e["frame"]: e for e in B}
    common = sorted(set(ma) & set(mb))
    print(f"{a} frames={len(A)}  {b} frames={len(B)}  common={len(common)}"
          f" [{common[0] if common else '-'}..{common[-1] if common else '-'}]")
    cols = ["wr", "pc", "mmio", "sp", "cyc"]
    first = {c: None for c in cols}
    for f in common:
        for c in cols:
            if first[c] is None and ma[f].get(c) != mb[f].get(c):
                first[c] = f
    print("First divergent frame per column:")
    for c in cols:
        f = first[c]
        if f is None:
            print(f"  {c:>5}: (identical across common range)")
        else:
            print(f"  {c:>5}: frame {f}\n          A={ma[f].get(c)}  B={mb[f].get(c)}")
            for cc in ("wc", "mc", "sc"):
                print(f"            {cc}: A={ma[f].get(cc)} B={mb[f].get(cc)}")


def recarm(port, frame):
    print(q(port, {"cmd": "record_frame", "frame": int(frame)}))


def recdump(port, out):
    ents, off = [], 0
    while True:
        r = q(port, {"cmd": "record_frame_dump", "offset": off, "count": 4000})
        e = r.get("entries", [])
        if not e:
            break
        ents.extend(e); off += len(e)
        if off >= r.get("total", 0):
            break
    json.dump({"entries": ents}, open(out, "w"))
    kinds = {}
    for e in ents:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    print(f"dumped {len(ents)} unified entries -> {out}\n  kinds: {kinds}")


def recdiff(a, b):
    A = json.load(open(a))["entries"]; B = json.load(open(b))["entries"]
    print(f"A={len(A)} entries  B={len(B)} entries")
    n = min(len(A), len(B)); div = None
    for i in range(n):
        if (A[i]["kind"], A[i]["addr"], A[i]["val"], A[i]["pc"]) != \
           (B[i]["kind"], B[i]["addr"], B[i]["val"], B[i]["pc"]):
            div = i; break
    if div is None:
        print(f"common prefix identical; lengths differ at i={n}" if len(A) != len(B) else "identical")
        return
    print(f"FIRST DIVERGENT ACCESS at execution index i={div}")
    for i in range(max(0, div - 4), min(n, div + 4)):
        m = ">>" if i == div else "  "
        print(f"{m} i={i}\n     A {A[i]['kind']:>5} addr={A[i]['addr']} val={A[i]['val']} pc={A[i]['pc']} cyc={A[i].get('cyc')}"
              f"\n     B {B[i]['kind']:>5} addr={B[i]['addr']} val={B[i]['val']} pc={B[i]['pc']} cyc={B[i].get('cyc')}")


def recdiff_state(a, b):
    """First divergent RAM-WRITE (addr,val) ignoring pc and reads. This is the
    real state fork (what wr_hash/sp_hash measure); store-PC attribution and MMIO
    read interleaving differ benignly between backends."""
    A = [e for e in json.load(open(a))["entries"] if e["kind"] in ("ramw", "spw")]
    B = [e for e in json.load(open(b))["entries"] if e["kind"] in ("ramw", "spw")]
    print(f"A ramw+spw={len(A)}  B ramw+spw={len(B)}")
    n = min(len(A), len(B)); div = None
    for i in range(n):
        if (A[i]["kind"], A[i]["addr"], A[i]["val"]) != (B[i]["kind"], B[i]["addr"], B[i]["val"]):
            div = i; break
    if div is None:
        print(f"common write-prefix identical; lengths differ at i={n}" if len(A) != len(B) else "identical")
        return
    print(f"FIRST DIVERGENT RAM WRITE at write-index {div}")
    for i in range(max(0, div - 4), min(n, div + 5)):
        m = ">>" if i == div else "  "
        print(f"{m} w{i}  A {A[i]['kind']:>4} {A[i]['addr']}={A[i]['val']} (pc {A[i]['pc']} cyc {A[i].get('cyc')})"
              f"   B {B[i]['kind']:>4} {B[i]['addr']}={B[i]['val']} (pc {B[i]['pc']} cyc {B[i].get('cyc')})")


if __name__ == "__main__":
    cmd = sys.argv[1]
    globals()[cmd](*sys.argv[2:])
