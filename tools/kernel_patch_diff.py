#!/usr/bin/env python3
"""Which kernel-RAM words does this game patch at runtime?

Boots the game headless, then diffs live kernel RAM against the BIOS image's
boot-time copy (the [[recompiler.address_model.copy]] window with
kernel_bless = true) and reports every differing word, joined to the compiled
kernel bodies from the generated <stem>_dispatch.c. The output says which
FUNCTIONS carry a patch, and which of those still fail the bless check and so
run interpreted.

This is how you find [[recompiler.install_slots]] ranges for a new image or a
new SDK version. Run from a game project root:

    python psxrecomp/tools/kernel_patch_diff.py                    # OpenBIOS
    python psxrecomp/tools/kernel_patch_diff.py --at 5 --at 30      # two snapshots
    python psxrecomp/tools/kernel_patch_diff.py --no-launch --port 4370
    python psxrecomp/tools/kernel_patch_diff.py \
        --bios psxrecomp/bios/SCPH1001.BIN \
        --profile psxrecomp/bios/SCPH1001.toml

Snapshot twice. Some patches land at boot and some on the first card access,
and a range that only appears late is still a range.

Reading the output: a differing word inside a compiled body and OUTSIDE every
declared range is what unblesses that body. A word inside a declared range is
already handled - the verifier skips it and the body runs native anyway. A
word in no body at all is data (event tables, TCB saves, pad tables); it
unblesses nothing, so do not declare it. A word whose writer you cannot
identify is a word you should not declare either: the declaration tells the
verifier to stop checking it (CLAUDE.md rule 14).

See docs/dynamic_handler_install.md for the patch shapes, the resume kinds,
and how the three halves of the mechanism compose.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kernel_patch_common as kp


def wait_port(host, port, secs=180):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            r = kp.q("frame", host, port, timeout=30.0)
            if r.get("ok") or "frame" in r:
                return time.time() - t0
        except Exception:
            pass
        time.sleep(1)
    raise SystemExit("debug port never came up")


def parse_seeds(path):
    """[(ram_addr, label)] for seeds, accepting the ROM-LMA form too."""
    d = json.load(open(path, encoding="utf-8"))
    rows = d if isinstance(d, list) else (d.get("functions") or d.get("seeds") or [])
    return [(int(r["address"], 16) & 0x1FFFFFFF, r.get("label", "?"))
            for r in rows if isinstance(r, dict)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split(chr(10) + chr(10), 1)[0])
    kp.add_project_args(ap, default_port=4371)
    ap.add_argument("--seeds", default=None,
                    help="seeds json for function names; default from the profile")
    ap.add_argument("--at", type=float, action="append", default=None,
                    help="seconds after the port is up to snapshot "
                         "(repeatable; default 8 and 30)")
    ap.add_argument("--no-launch", action="store_true",
                    help="diff against an already-running game on --port")
    ap.add_argument("--out", default="analysis/kernel_patch_diff.json")
    a = ap.parse_args()
    root = kp.resolve(a)
    ats = a.at or [8.0, 30.0]

    rom_rel, copies = kp.parse_profile(os.path.join(root, a.profile))
    rom = open(os.path.join(root, a.framework, rom_rel), "rb").read()
    bodies = kp.parse_bodies(os.path.join(root, a.dispatch))
    pranges = kp.parse_patch_ranges(os.path.join(root, a.dispatch))
    declared = lambda ram: any(lo <= ram < hi for lo, hi in pranges)
    if pranges:
        print("declared install-slot ranges (%s): %s"
              % (os.path.basename(a.dispatch),
                 ", ".join("0x%04X..0x%04X" % r for r in pranges)), flush=True)
    else:
        print("no install-slot ranges declared in %s - every patched word "
              "unblesses its body" % os.path.basename(a.dispatch), flush=True)

    if a.seeds is None:
        import re
        txt = open(os.path.join(root, a.profile), encoding="utf-8").read()
        m = re.search(r'^\s*seeds\s*=\s*"([^"]+)"', txt, re.M)
        a.seeds = os.path.join(a.framework, m.group(1)) if m else None
    seeds = parse_seeds(os.path.join(root, a.seeds)) if a.seeds else []

    # Map ROM-LMA seeds into RAM for each copy so a patched word can be named.
    ram_names = {}
    for name, rom_lo, ram_lo, ln, bless in copies:
        for addr, label in seeds:
            if rom_lo <= addr < rom_lo + ln:
                ram_names[addr - rom_lo + ram_lo] = label
            elif ram_lo <= addr < ram_lo + ln:
                ram_names[addr] = label

    def fn_of(ram):
        best = [k for k in ram_names if k <= ram]
        return ("%s+0x%X" % (ram_names[max(best)], ram - max(best))) if best else "?"

    proc = None
    if not a.no_launch:
        env = dict(os.environ, PSX_STARVATION_TIMEOUT_US="0")
        env.pop("PSX_LOAD_SLOT", None)
        argv = [os.path.join(root, a.exe), "--game", a.game, "--no-launcher",
                "--headless", "--debug-port", str(a.port)]
        if a.bios:
            argv += ["--bios", os.path.join(root, a.bios)]
        proc = subprocess.Popen(argv, cwd=root, env=env,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        print("launched pid %d" % proc.pid, flush=True)
    t_up = wait_port(a.host, a.port)
    print("port up after %.1fs" % t_up, flush=True)
    t0 = time.time()

    report = {"copies": [], "snapshots": [],
              "declared_ranges": ["0x%04X..0x%04X" % r for r in pranges]}
    try:
        for at in sorted(ats):
            while time.time() - t0 < at:
                time.sleep(0.5)
            fr = kp.q("frame", a.host, a.port)
            kb = kp.q("kernel_bless", a.host, a.port)
            snap = {"t": round(time.time() - t0, 1), "frame": fr.get("frame"),
                    "kernel_bless": kb, "diffs": []}
            print("\n=== t+%.0fs frame %s  kernel_bless: %s"
                  % (at, fr.get("frame"),
                     {k: v for k, v in kb.items() if k not in ("id", "ok")}),
                  flush=True)
            for name, rom_lo, ram_lo, ln, bless in copies:
                if not bless:
                    continue      # only the bless window gates native dispatch
                live = bytes.fromhex(kp.q("read_ram", a.host, a.port,
                                          addr="0x%08X" % ram_lo, len=ln)["hex"])
                src = rom[rom_lo - 0x1FC00000: rom_lo - 0x1FC00000 + ln]
                words = []
                for off in range(0, ln - 3, 4):
                    if live[off:off + 4] != src[off:off + 4]:
                        ram = ram_lo + off
                        inb = [b for b in bodies if b[1] <= ram < b[2]]
                        words.append({
                            "ram": "0x%04X" % ram,
                            "rom": struct.unpack("<I", src[off:off + 4])[0],
                            "live": struct.unpack("<I", live[off:off + 4])[0],
                            "in_code_body": bool(inb),
                            "declared": declared(ram),
                            "fn": fn_of(ram)})
                runs = []
                for w in words:
                    r = int(w["ram"], 16)
                    if runs and r == runs[-1]["end"] and \
                       runs[-1]["in_code_body"] == w["in_code_body"]:
                        runs[-1]["end"] = r + 4
                        runs[-1]["n"] += 1
                    else:
                        runs.append({"start": r, "end": r + 4, "n": 1,
                                     "in_code_body": w["in_code_body"],
                                     "fn": w["fn"]})
                code_words = sum(1 for w in words if w["in_code_body"])
                undecl = sum(1 for w in words
                             if w["in_code_body"] and not w["declared"])
                print("copy %-16s RAM 0x%04X..0x%04X: %d differing words, %d inside "
                      "compiled code bodies (%d declared, %d NOT declared), %d runs"
                      % (name, ram_lo, ram_lo + ln, len(words), code_words,
                         code_words - undecl, undecl, len(runs)), flush=True)
                for r in runs:
                    tag = "CODE" if r["in_code_body"] else "data"
                    print("   %s 0x%04X..0x%04X (%d words)  %s"
                          % (tag, r["start"], r["end"], r["n"], r["fn"]), flush=True)
                    if r["in_code_body"]:
                        for w in words:
                            if r["start"] <= int(w["ram"], 16) < r["end"]:
                                print("        %s rom %08X -> live %08X  %s"
                                      % (w["ram"], w["rom"], w["live"],
                                         "declared" if w["declared"]
                                         else "NOT DECLARED"), flush=True)
                snap["diffs"].append({"copy": name, "words": words, "runs": runs})
            report["snapshots"].append(snap)

            # Only an UNDECLARED patched word unblesses a body.
            dirty, saved = set(), set()
            for sd in snap["diffs"]:
                for w in sd["words"]:
                    r = int(w["ram"], 16)
                    for key, lo, hi in bodies:
                        if lo <= r < hi:
                            (saved if w["declared"] else dirty).add((lo, hi))
            saved -= dirty        # one undeclared word is enough to unbless
            if saved:
                print("compiled kernel bodies patched INSIDE a declared range "
                      "(these still run native):", flush=True)
                for lo, hi in sorted(saved):
                    print("   0x%04X..0x%04X  %s" % (lo, hi, fn_of(lo)), flush=True)
            if dirty:
                print("compiled kernel bodies containing an UNDECLARED patched "
                      "word (these interpret):", flush=True)
                for lo, hi in sorted(dirty):
                    print("   0x%04X..0x%04X  %s" % (lo, hi, fn_of(lo)), flush=True)
            else:
                print("no compiled kernel body carries an undeclared patched word",
                      flush=True)
            snap["dirty_bodies"] = ["0x%04X..0x%04X %s" % (lo, hi, fn_of(lo))
                                    for lo, hi in sorted(dirty)]
            snap["blessed_patched_bodies"] = ["0x%04X..0x%04X %s" % (lo, hi, fn_of(lo))
                                              for lo, hi in sorted(saved)]
    finally:
        if proc:
            proc.kill()
    out = os.path.join(root, a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(report, open(out, "w", encoding="utf-8"), indent=1)
    print("\nwrote " + a.out)


if __name__ == "__main__":
    main()
