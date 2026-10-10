#!/usr/bin/env python3
"""Name the host functions in stall-sampler evidence (stall_report.py covers the
interpreter/dispatch side; this file reads the native stack samples).

Inputs (runtime/src/starvation_ring.c):
  * psx_stall_report_<epoch>_<n>.json  written when the emu thread resumes
    from a host-side block of >= 1 s (no TCP client needed);
  * starvation_dump.jsonl              (its "stall_seq" lines);
  * a live game:  --port N             (TCP stall_ring).

Frames are "module+0xRVA". Frames in the game executable are mapped to the
enclosing function with the image's own symbol table (nm), as host_profile.py
does, so a Release build without debug info works. Other modules (ntdll,
GPU drivers, overlay shard DLLs) are printed as recorded.

  stall_samples.py --exe build/Game.exe psx_stall_report_1791604934_0.json
  stall_samples.py --exe build/Game.exe --port 4397

How to read it: EMU_SAMPLE entries are the emu thread's call stack while it
was blocked (identical consecutive stacks = where it sat). HOST_BLOCK is the
block length once it resumed. A HOST_BLOCK with no EMU_SAMPLE in its window
(and a SAMPLER_GAP nearby) means the whole process or machine was paused,
not the emu thread alone.
"""
import argparse
import bisect
import collections
import json
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from host_profile import find_nm, symbols  # noqa: E402


def load_entries(args):
    if args.port:
        with socket.create_connection(("127.0.0.1", args.port), 5) as s:
            s.settimeout(30)
            s.sendall((json.dumps(dict(id=1, cmd="stall_ring", count=256)) + "\n").encode())
            with s.makefile("rb") as f:
                return json.loads(f.readline())["entries"], {}
    text = open(args.input, encoding="utf-8").read()
    if text.lstrip().startswith("{\"block_us\""):
        report = json.loads(text)
        return report["entries"], {k: v for k, v in report.items() if k != "entries"}
    entries = []
    for line in text.splitlines():
        if "\"stall_seq\"" in line:
            entries.append(json.loads(line))
    return entries, {}


def namer(exe, nm):
    if not exe:
        return lambda frame: frame
    table = symbols(find_nm(nm), exe)
    rvas = [rva for rva, _ in table]
    image = os.path.basename(exe).lower()

    def name(frame):
        module, sep, off = frame.partition("+0x")
        if not sep or module.lower() != image:
            return frame
        rva = int(off, 16)
        i = bisect.bisect_right(rvas, rva) - 1
        if i < 0:
            return frame
        return "%s+0x%X" % (table[i][1], rva - table[i][0])
    return name


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="stall report JSON or starvation_dump.jsonl")
    ap.add_argument("--port", type=int, help="query a running game instead")
    ap.add_argument("--exe", help="game executable for function names")
    ap.add_argument("--nm")
    ap.add_argument("--depth", type=int, default=12)
    args = ap.parse_args()
    if not args.input and not args.port:
        ap.error("give a report/dump path or --port")
    entries, header = load_entries(args)
    name = namer(args.exe, args.nm)
    if header:
        samples = sum(1 for e in entries if e["kind"] == "EMU_SAMPLE")
        print("block %.2f s, %d emu-thread stack samples%s" % (
            header.get("block_us", 0) / 1e6, samples,
            " (none: whole process/machine was paused)" if not samples else ""))
    stacks = collections.Counter()
    for e in entries:
        frames = tuple(name(f) for f in e.get("frames", [])[:args.depth])
        if e["kind"] == "EMU_SAMPLE":
            stacks[frames] += 1
        else:
            print("%-12s gap %.3f s  frame %s  func %s" % (
                e["kind"], e["gap_us"] / 1e6, e.get("frame"), e.get("func")))
    for frames, count in stacks.most_common():
        print("\n%d sample(s):" % count)
        for f in frames:
            print("    " + f)


if __name__ == "__main__":
    main()
