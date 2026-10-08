#!/usr/bin/env python3
"""Name the hot host functions of a running psxrecomp game.

Reads the always-on host CPU sampler (TCP host_profile, runtime/src/
host_sampler.c) over a window of the ring and maps each executable-relative
address to the function containing it, using the image's own symbol table
(nm), so a Release build without debug info profiles too.

  host_profile.py --port 4531 --exe game.exe --frames 300 [--pass 1] [--top 40]

--frames N: the last N guest frames. --pass 1 / 0: only samples inside /
outside render passes. Prints per-function sample counts and shares.
"""
import argparse
import bisect
import json
import os
import shutil
import socket
import subprocess
import sys


def query(port, **kw):
    with socket.create_connection(("127.0.0.1", port), 5) as s:
        s.settimeout(30)
        s.sendall((json.dumps(dict(id=1, cmd="host_profile", **kw)) + "\n").encode())
        with s.makefile("rb") as f:
            return json.loads(f.readline())


def find_nm(explicit):
    if explicit:
        return explicit
    for name in ("llvm-nm", "nm"):
        p = shutil.which(name)
        if p:
            return p
    sys.exit("no nm on PATH; pass --nm")


def preferred_base(exe, out):
    """The address nm's symbol values are relative to: a PE image's
    ImageBase (optional header), a Mach-O executable's __mh_execute_header,
    0 for a position-independent ELF."""
    with open(exe, "rb") as f:
        head = f.read(4096)
    if head[:2] == b"MZ":
        pe = int.from_bytes(head[0x3C:0x40], "little")
        magic = int.from_bytes(head[pe + 24:pe + 26], "little")
        if magic == 0x20B:   # PE32+
            return int.from_bytes(head[pe + 24 + 24:pe + 24 + 32], "little")
        return int.from_bytes(head[pe + 24 + 28:pe + 24 + 32], "little")
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[2] in ("__mh_execute_header", "_mh_execute_header"):
            return int(parts[0], 16)
    return 0


def image_size(exe):
    """SizeOfImage of a PE executable, or None."""
    with open(exe, "rb") as f:
        head = f.read(4096)
    if head[:2] != b"MZ":
        return None
    pe = int.from_bytes(head[0x3C:0x40], "little")
    return int.from_bytes(head[pe + 24 + 56:pe + 24 + 60], "little")


def symbols(nm, exe):
    """(rva, name) sorted, from the image's text symbols."""
    out = subprocess.run([nm, "--defined-only", exe], capture_output=True,
                         text=True, check=True).stdout
    base = preferred_base(exe, out)
    table = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in ("T", "t"):
            table.append((int(parts[0], 16) - base, parts[2]))
    table.sort()
    return table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--since", type=int, default=-1)
    ap.add_argument("--pass", dest="in_pass", type=int, default=-1)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--nm")
    a = ap.parse_args()
    kw = {"top": 400}
    if a.frames > 0:
        kw["frames"] = a.frames
    if a.since >= 0:
        kw["since"] = a.since
    if a.in_pass >= 0:
        kw["pass"] = a.in_pass
    r = query(a.port, **kw)
    if not r.get("ok") or not r.get("supported"):
        sys.exit("sampler unavailable: %s" % r)
    table = symbols(find_nm(a.nm), a.exe)
    keys = [t[0] for t in table]
    size = image_size(a.exe)
    per = {}
    for rva_s, n in r["top"]:
        rva = int(rva_s, 16)
        i = bisect.bisect_right(keys, rva) - 1
        if (size is not None and rva >= size) or i < 0:
            name = "(outside the executable: system code, waits)"
        else:
            name = table[i][1]
        per[name] = per.get(name, 0) + n
    total = r["samples"] or 1
    print("samples %d (binned top 400 addresses; %d not binned) seq %d" %
          (r["samples"], r["unbinned"], r["seq"]))
    for name, n in sorted(per.items(), key=lambda kv: -kv[1])[:a.top]:
        print("%6d %5.1f%%  %s" % (n, 100.0 * n / total, name))


if __name__ == "__main__":
    main()
